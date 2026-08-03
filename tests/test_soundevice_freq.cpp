// tests/test_soundevice_freq.cpp
// CSoundDevice のラウンドロビン(動的ボイスアサイン)に関する回帰テスト。
// チャンネル再利用時にupdateFreqが新しいノートの周波数を正しく書き込むこと、
// および強制スティール後のチャンネル所有権(isChOwnedBy)が正しく更新される
// ことを検証する。

#include <catch2/catch_test_macros.hpp>
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include "fitom/MidiCh.h"
#include "fitom/MultiDevice.h"
#include <map>
#include <memory>
#include <utility>
#include <vector>

using namespace fitom;

namespace {
class RecordingPort : public IPort {
public:
    std::map<uint16_t, uint8_t> regs;
    // 書き込み順序そのものを検証したいテスト(例: 同一ownerによる
    // Running chの強制スティール時、Key Off→Key On の順で書かれているか)
    // 向けに、regsとは別に全書き込みを時系列で記録する。
    std::vector<std::pair<uint16_t, uint8_t>> history;
    void write(uint16_t addr, uint16_t data) override {
        regs[addr] = static_cast<uint8_t>(data);
        history.emplace_back(addr, static_cast<uint8_t>(data));
    }
    uint8_t read(uint16_t addr) override {
        auto it = regs.find(addr);
        return it != regs.end() ? it->second : 0;
    }
};

struct DummyMidiCh : IMidiCh {
    void timerCallback(uint32_t) override {}
};
} // namespace

namespace fitom {
std::unique_ptr<ISoundDevice> createCOPN(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPN2(IPort* p, int sr, IPort* p2);
std::unique_ptr<ISoundDevice> createCOPNA(IPort* p, int sr, IPort* p2);
std::unique_ptr<ISoundDevice> createCOPNB(IPort* p, int sr, IPort* p2);
std::unique_ptr<ISoundDevice> createCOPL4AWM(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPL(IPort* p, int sr, bool rhythmMode = false);
std::unique_ptr<ISoundDevice> createCOPLRhythm(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPLLRhythm(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPNARhythm(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCAdPcm(IPort* p, int sr, uint32_t deviceType);
}

TEST_CASE("Round-robin channel reuse writes new note frequency", "[sounddevice]")
{
    RecordingPort port;
    auto dev = createCOPN(&port, 8000000);
    dev->init();

    HwPatch patch{};
    patch.id = 1;

    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev->setNoteFine(ch, 60, 0, true);
    dev->noteOn(ch, 100);

    uint16_t hiReg = static_cast<uint16_t>(0xA4 + ch);
    uint16_t loReg = static_cast<uint16_t>(0xA0 + ch);
    uint8_t hi60 = port.regs[hiReg];
    uint8_t lo60 = port.regs[loReg];

    dev->noteOff(ch);

    uint8_t ch2 = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch2 == ch); // 唯一使用中/Releasingのchなのでラウンドロビンで再利用されるはず

    dev->setNoteFine(ch2, 72, 0, true);
    dev->noteOn(ch2, 100);

    uint8_t hi72 = port.regs[static_cast<uint16_t>(0xA4 + ch2)];
    uint8_t lo72 = port.regs[static_cast<uint16_t>(0xA0 + ch2)];

    INFO("hi60=" << (int)hi60 << " lo60=" << (int)lo60
         << " hi72=" << (int)hi72 << " lo72=" << (int)lo72);
    CHECK((hi72 != hi60 || lo72 != lo60));
}

// 複数MIDIチャンネルが同一デバイスを共有し、全chが埋まった状態で
// 別ownerが強制スティール(findBestCh score=1)した場合、奪われた側の
// ownerがstale化した自分のブックキーピングでnoteOff()を呼んでも、
// 奪った側の新しい発音を誤って止めてはならない
// (isChOwnedBy()での事前確認が必要。CInstCh::noteOff()等の修正参照)。
TEST_CASE("Stolen channel is not owned by the original owner anymore", "[sounddevice]")
{
    RecordingPort port;
    auto dev = createCOPN(&port, 8000000); // 3ch
    dev->init();

    HwPatch patch{};
    patch.id = 1;

    DummyMidiCh ownerA, ownerB;

    // ownerAが全3chを埋める(各割り当ての間にtimerCallbackを挟み、
    // noteOnAgeに差を付けて「最古のch」が一意に決まるようにする)
    uint8_t chs[3];
    for (int i = 0; i < 3; ++i) {
        chs[i] = dev->allocCh(&ownerA, &patch, 100);
        REQUIRE(chs[i] != 0xFF);
        dev->setNoteFine(chs[i], static_cast<uint8_t>(60 + i), 0, true);
        dev->noteOn(chs[i], 100);
        for (int t = 0; t < 10; ++t) dev->timerCallback(static_cast<uint32_t>(t));
    }

    // 空きが無いので、ownerBのノートオンはownerA所有chを強制奪取する
    uint8_t stolen = dev->allocCh(&ownerB, &patch, 100);
    REQUIRE(stolen != 0xFF);
    dev->setNoteFine(stolen, 90, 0, true);
    dev->noteOn(stolen, 100);

    // 奪われたchはもうownerAの所有ではない
    CHECK_FALSE(dev->isChOwnedBy(stolen, &ownerA));
    CHECK(dev->isChOwnedBy(stolen, &ownerB));

    // ownerA側の(stale化した)ブックキーピングでnoteOffしようとしても、
    // isChOwnedByの確認さえ行えば、ownerBの発音を誤って止めない
    // (このガードが無いと dev->noteOff(stolen) が直接ownerBの発音を止めてしまう)。
    if (dev->isChOwnedBy(stolen, &ownerA)) {
        dev->noteOff(stolen);
    }
    const auto* cs = dev->getChState(stolen);
    REQUIRE(cs != nullptr);
    CHECK(cs->isRunning()); // ownerBのノートは消音されず鳴り続けている
}

// 実機再現報告: 同一ノートをリリースホールド期間(kReleasingHoldMs)中に
// チャンネル数以上連打し、その直後に別のノートを同じパターンで連打すると、
// lastNote/lastFnumはソフトウェア側で正しく更新されているのに、実際に
// チップへ書き込まれるレジスタ値が前のノートのままになる、という報告の再現を
// 試みる。setReg()の「regBak_と同じ値なら書き込みスキップ」最適化
// (forceWrite=false)が、書き込み漏れの原因になっていないかを検証する。
TEST_CASE("Rapid same-channel-count retrigger keeps register in sync with lastFnum", "[sounddevice]")
{
    RecordingPort port;
    auto dev = createCOPN(&port, 8000000); // 3ch
    dev->init();

    HwPatch patch{};
    patch.id = 1;
    DummyMidiCh owner;

    auto expectedHiLo = [&](uint8_t ch) {
        const auto* cs = dev->getChState(ch);
        REQUIRE(cs != nullptr);
        uint8_t hi = static_cast<uint8_t>((cs->lastFnum.block << 3) | ((cs->lastFnum.fnum >> 8) & 0x7));
        uint8_t lo = static_cast<uint8_t>(cs->lastFnum.fnum & 0xFF);
        return std::pair<uint8_t, uint8_t>{hi, lo};
    };

    auto playAndCheck = [&](uint8_t note, int repeats) {
        for (int i = 0; i < repeats; ++i) {
            // noteOffを挟まず積み重ねる(=前のノートがまだRunning中に次を
            // 発音、チャンネル数を超えたら最古のRunning chが強制スティール
            // される)。これによりowner+patch一致によるscore=4の優先とは
            // 無関係な、age基準の本来のラウンドロビンを誘発する。
            uint8_t ch = dev->allocCh(&owner, &patch, 100);
            REQUIRE(ch != 0xFF);
            dev->setNoteFine(ch, note, 0, true);
            dev->noteOn(ch, 100);
            for (int t = 0; t < 5; ++t) dev->timerCallback(static_cast<uint32_t>(t));

            auto [expHi, expLo] = expectedHiLo(ch);
            uint8_t actualHi = port.regs[static_cast<uint16_t>(0xA4 + ch)];
            uint8_t actualLo = port.regs[static_cast<uint16_t>(0xA0 + ch)];
            INFO("note=" << (int)note << " iter=" << i << " ch=" << (int)ch
                 << " expHi=" << (int)expHi << " actualHi=" << (int)actualHi
                 << " expLo=" << (int)expLo << " actualLo=" << (int)actualLo);
            CHECK(actualHi == expHi);
            CHECK(actualLo == expLo);
        }
    };

    // 同一ノートをチャンネル数(3)以上連打(リリースホールド中の再利用を誘発)
    playAndCheck(60, 8);
    // 直後に別のノートを同じパターンで連打
    playAndCheck(72, 8);

    // 最終的に全chのレジスタがlastFnumと一致していることを確認
    for (uint8_t ch = 0; ch < 3; ++ch) {
        auto [expHi, expLo] = expectedHiLo(ch);
        uint8_t actualHi = port.regs[static_cast<uint16_t>(0xA4 + ch)];
        uint8_t actualLo = port.regs[static_cast<uint16_t>(0xA0 + ch)];
        INFO("final ch=" << (int)ch << " expHi=" << (int)expHi << " actualHi=" << (int)actualHi
             << " expLo=" << (int)expLo << " actualLo=" << (int)actualLo);
        CHECK(actualHi == expHi);
        CHECK(actualLo == expLo);
    }
}

// 上と同じ再現手順を、CSpanDevice経由(OPNA/OPN2、2ポート6ch構成)で試す。
// ポートオフセット/OPN2Port2(0x28インターセプト)を挟むぶん、単体OPN(3ch)
// より実際のユーザー報告(チャンネル数以上での連打)に近い構成になる。
TEST_CASE("Rapid retrigger across CSpanDevice (OPNA 6ch) keeps registers in sync", "[sounddevice]")
{
    RecordingPort port;
    auto dev = createCOPNA(&port, 8000000, nullptr); // 6ch (port1: ch0-2, port2: ch3-5)
    dev->init();

    HwPatch patch{};
    patch.id = 1;
    DummyMidiCh owner;

    auto regAddr = [&](uint8_t ch, uint16_t base) -> uint16_t {
        uint16_t portOffset = (ch < 3) ? 0 : 0x100;
        uint8_t local = static_cast<uint8_t>(ch % 3);
        return static_cast<uint16_t>(portOffset + base + local);
    };

    auto expectedHiLo = [&](uint8_t ch) {
        const auto* cs = dev->getChState(ch);
        REQUIRE(cs != nullptr);
        uint8_t hi = static_cast<uint8_t>((cs->lastFnum.block << 3) | ((cs->lastFnum.fnum >> 8) & 0x7));
        uint8_t lo = static_cast<uint8_t>(cs->lastFnum.fnum & 0xFF);
        return std::pair<uint8_t, uint8_t>{hi, lo};
    };

    auto playAndCheck = [&](uint8_t note, int repeats) {
        for (int i = 0; i < repeats; ++i) {
            uint8_t ch = dev->allocCh(&owner, &patch, 100);
            REQUIRE(ch != 0xFF);
            dev->setNoteFine(ch, note, 0, true);
            dev->noteOn(ch, 100);
            for (int t = 0; t < 5; ++t) dev->timerCallback(static_cast<uint32_t>(t));

            auto [expHi, expLo] = expectedHiLo(ch);
            uint8_t actualHi = port.regs[regAddr(ch, 0xA4)];
            uint8_t actualLo = port.regs[regAddr(ch, 0xA0)];
            INFO("note=" << (int)note << " iter=" << i << " ch=" << (int)ch
                 << " expHi=" << (int)expHi << " actualHi=" << (int)actualHi
                 << " expLo=" << (int)expLo << " actualLo=" << (int)actualLo);
            CHECK(actualHi == expHi);
            CHECK(actualLo == expLo);
        }
    };

    playAndCheck(60, 14); // 6chの2倍以上連打
    playAndCheck(72, 14);

    for (uint8_t ch = 0; ch < 6; ++ch) {
        auto [expHi, expLo] = expectedHiLo(ch);
        uint8_t actualHi = port.regs[regAddr(ch, 0xA4)];
        uint8_t actualLo = port.regs[regAddr(ch, 0xA0)];
        INFO("final ch=" << (int)ch << " expHi=" << (int)expHi << " actualHi=" << (int)actualHi
             << " expLo=" << (int)expLo << " actualLo=" << (int)actualLo);
        CHECK(actualHi == expHi);
        CHECK(actualLo == expLo);
    }
}

// 報告者の実プロファイル(emu_opn.profile.json)構成の再現。ログ上、
// 実際に発音に使われるOPN2#1は「OPN2本体 + OPNA#2-FM + OPNB#3-FM + OPNBB#4-FM」
// (VoicePatchType=0x11一致)が mergeSpannableDevices() で束ねられた、
// 4物理チップ・24ch(うちOPNB系はch0/3無効化で実質22ch有効)のCSpanDevice
// である。COPNA/COPNBはそれ自体が内部で2つのCOPNを束ねるCSpanDeviceのため、
// 「CSpanDeviceの要素としてCSpanDeviceが入れ子になる」構成になる。
// 単純な単体OPN/2ポートOPNAでは再現しなかったため、この入れ子構成・
// チャンネル数不均一(OPNB系は一部ch無効)な状態でチャンネル数超の連打を
// 行った場合に、レジスタとlastFnumの乖離が起きないかを検証する。
TEST_CASE("Rapid retrigger across nested multi-chip span (OPN2+OPNA+OPNB) keeps registers in sync", "[sounddevice]")
{
    RecordingPort portA, portB, portC;
    auto chipOpn2 = createCOPN2(&portA, 8000000, nullptr); // 6ch
    auto chipOpna = createCOPNA(&portB, 8000000, nullptr); // 6ch
    auto chipOpnb = createCOPNB(&portC, 8000000, nullptr); // 6ch addressable / ch0,3 disabled

    ISoundDevice* rawOpn2 = chipOpn2.get();
    ISoundDevice* rawOpna = chipOpna.get();
    ISoundDevice* rawOpnb = chipOpnb.get();
    rawOpn2->init();
    rawOpna->init();
    rawOpnb->init();

    CSpanDevice outer;
    outer.addDevice(rawOpn2);
    outer.addDevice(rawOpna);
    outer.addDevice(rawOpnb);
    // globalCh 0-5 → OPN2, 6-11 → OPNA, 12-17 → OPNB(12,15は無効)

    HwPatch patch{};
    patch.id = 1;
    DummyMidiCh owner;

    auto portFor = [&](uint8_t gch) -> RecordingPort& {
        if (gch < 6)  return portA;
        if (gch < 12) return portB;
        return portC;
    };
    auto regAddr = [&](uint8_t gch, uint16_t base) -> uint16_t {
        uint8_t localInChip = static_cast<uint8_t>(gch % 6); // 各サブチップ内0-5
        uint16_t portOffset = (localInChip < 3) ? 0 : 0x100;
        uint8_t local = static_cast<uint8_t>(localInChip % 3);
        return static_cast<uint16_t>(portOffset + base + local);
    };

    auto expectedHiLo = [&](uint8_t gch) {
        const auto* cs = outer.getChState(gch);
        REQUIRE(cs != nullptr);
        uint8_t hi = static_cast<uint8_t>((cs->lastFnum.block << 3) | ((cs->lastFnum.fnum >> 8) & 0x7));
        uint8_t lo = static_cast<uint8_t>(cs->lastFnum.fnum & 0xFF);
        return std::pair<uint8_t, uint8_t>{hi, lo};
    };

    std::vector<uint8_t> usedChs;
    auto playAndCheck = [&](uint8_t note, int repeats) {
        for (int i = 0; i < repeats; ++i) {
            uint8_t gch = outer.allocCh(&owner, &patch, 100);
            REQUIRE(gch != 0xFF);
            usedChs.push_back(gch);
            outer.setNoteFine(gch, note, 0, true);
            outer.noteOn(gch, 100);
            for (int t = 0; t < 5; ++t) outer.timerCallback(static_cast<uint32_t>(t));

            auto [expHi, expLo] = expectedHiLo(gch);
            auto& p = portFor(gch);
            uint8_t actualHi = p.regs[regAddr(gch, 0xA4)];
            uint8_t actualLo = p.regs[regAddr(gch, 0xA0)];
            INFO("note=" << (int)note << " iter=" << i << " gch=" << (int)gch
                 << " expHi=" << (int)expHi << " actualHi=" << (int)actualHi
                 << " expLo=" << (int)expLo << " actualLo=" << (int)actualLo);
            CHECK(actualHi == expHi);
            CHECK(actualLo == expLo);
        }
    };

    // 22chの有効ch数を超える連打(同一ノート→別ノート)
    playAndCheck(60, 30);
    playAndCheck(72, 30);

    // 実際に使われた全globalchについて、最終状態でレジスタとlastFnumが
    // 一致しているかを再確認する。
    for (uint8_t gch : usedChs) {
        auto [expHi, expLo] = expectedHiLo(gch);
        auto& p = portFor(gch);
        uint8_t actualHi = p.regs[regAddr(gch, 0xA4)];
        uint8_t actualLo = p.regs[regAddr(gch, 0xA0)];
        INFO("final gch=" << (int)gch << " expHi=" << (int)expHi << " actualHi=" << (int)actualHi
             << " expLo=" << (int)expLo << " actualLo=" << (int)actualLo);
        CHECK(actualHi == expHi);
        CHECK(actualLo == expLo);
    }
}


// 実機再現報告: モノフォニックにもレイヤードパッチにもしていない、
// 高速に連続でノートオンが発生するトラック(ベースライン等)で、
// ノートオンが取りこぼされている(または直前のノートオフが効いていない)
// ように聞こえる、Fnumは更新されているという報告の再現を試みる。
//
// デバイスの全chが同一owner(同一MIDIチャンネル)のノートで埋まっている
// 状態で、さらに新しいノートオンが来ると、findBestCh()はscore=1
// (強制スティール、noteOnAge最大のchを選ぶ)で最古のchを選ぶ。この時
// 選ばれたchがたまたま「同じowner」が使っていたchだった場合、
// CSoundDevice::assignCh()の`s.owner != owner`ガードにより、本来
// 呼ばれるべきnoteOff()(Key Offレジスタ書き込みを含む)がスキップされて
// しまう。OPN系のKey On/Offレジスタ(0x28)はエッジトリガ(0→1で
// アタック開始)のため、Key Offを挟まずKey On(1)を再度書いても実機では
// エンベロープが再アタックせず、Fnumだけが新しいノートの値に変わって
// 音程だけ滑らかに(まるでポルタメントのように)変化して聞こえる、と
// いう症状に一致する。
TEST_CASE("Forced steal of a Running channel owned by the same owner still sends Key Off before Key On", "[sounddevice]")
{
    RecordingPort port;
    auto dev = createCOPN(&port, 8000000); // 3ch
    dev->init();

    HwPatch patch{};
    patch.id = 1;
    DummyMidiCh owner;

    // 3ch全てを同一owner+patchのノートで埋める(全てRunning)。
    // timerCallbackを挟んでnoteOnAgeに差を付け、ch0が最古になるようにする。
    for (int i = 0; i < 3; ++i) {
        uint8_t ch = dev->allocCh(&owner, &patch, 100);
        REQUIRE(ch == static_cast<uint8_t>(i));
        dev->setNoteFine(ch, static_cast<uint8_t>(60 + i), 0, true);
        dev->noteOn(ch, 100);
        for (int t = 0; t < 10; ++t) dev->timerCallback(static_cast<uint32_t>(t));
    }

    // 空きが無いので、同一owner(自分自身)の最古ch(ch0)を強制スティール
    // するはず。
    port.history.clear();
    uint8_t stolen = dev->allocCh(&owner, &patch, 100);
    REQUIRE(stolen == 0);
    dev->setNoteFine(stolen, 90, 0, true);
    dev->noteOn(stolen, 100);

    // reg 0x28 (Key On/Off, ch0はスロットマスク0xF0|0=0xF0でKeyOn、
    // 0x00でKeyOff)への書き込み履歴を調べ、新しいKeyOn(0xF0)より前に
    // KeyOff(0x00)が書かれていることを確認する。
    bool sawKeyOffCh0 = false;
    bool sawKeyOnCh0AfterOff = false;
    for (const auto& [addr, data] : port.history) {
        if (addr != 0x28) continue;
        if (data == 0x00) sawKeyOffCh0 = true;
        if (data == 0xF0 && sawKeyOffCh0) sawKeyOnCh0AfterOff = true;
    }
    INFO("history size=" << port.history.size());
    CHECK(sawKeyOffCh0);
    CHECK(sawKeyOnCh0AfterOff);
}

// 上のテストは「デバイス全chが同一ownerで埋まっている(forceSteal)」という
// やや特殊な状況を再現したものだったが、報告された「高速なベースライン」の
// 症状はチャンネルに余裕がある通常のround-robin再利用(Releasing chの
// score=4再利用、findBestCh参照)でも起きている可能性があるため、より
// シンプルな「チャンネルに十分な余裕がある状態での通常の連続ノートオン/
// オフ」でもKey Off→Key Onの順が保たれているかを別途検証する。
TEST_CASE("Normal round-robin reuse of a Releasing channel still sends Key Off before Key On", "[sounddevice]")
{
    RecordingPort port;
    auto dev = createCOPN(&port, 8000000); // 3ch (余裕あり、1noteしか使わない)
    dev->init();

    HwPatch patch{};
    patch.id = 1;
    DummyMidiCh owner;

    // ここから先の全書き込み(元のノートのnoteOff→再利用のnoteOnまで)を
    // 通しで記録し、Key Off(元ノート)がKey On(再利用後の新ノート)より
    // 前に来ているかを確認する(途中でclearしない。noteOff()自体が
    // 書くKey Offも検証対象に含める必要があるため)。
    uint8_t ch1 = dev->allocCh(&owner, &patch, 100);
    REQUIRE(ch1 != 0xFF);
    dev->setNoteFine(ch1, 60, 0, true);
    dev->noteOn(ch1, 100);
    for (int t = 0; t < 5; ++t) dev->timerCallback(static_cast<uint32_t>(t));

    port.history.clear();
    dev->noteOff(ch1); // → Releasing (kReleasingHoldMs残り)、Key Offを書くはず

    uint8_t ch2 = dev->allocCh(&owner, &patch, 100); // すぐ次のノート
    REQUIRE(ch2 == ch1); // 同一owner+patchなのでscore=4で同じchを再利用するはず
    dev->setNoteFine(ch2, 72, 0, true);
    dev->noteOn(ch2, 100); // Key Onを書くはず

    int keyOffIdx = -1, keyOnIdx = -1;
    for (size_t i = 0; i < port.history.size(); ++i) {
        const auto& [addr, data] = port.history[i];
        if (addr != 0x28) continue;
        if (data == static_cast<uint8_t>(ch1 & 3) && keyOffIdx < 0)
            keyOffIdx = static_cast<int>(i);
        if (data == static_cast<uint8_t>(0xF0 | (ch1 & 3)))
            keyOnIdx = static_cast<int>(i);
    }
    INFO("keyOffIdx=" << keyOffIdx << " keyOnIdx=" << keyOnIdx
         << " history size=" << port.history.size());
    CHECK(keyOffIdx >= 0);
    CHECK(keyOnIdx >= 0);
    CHECK(keyOffIdx < keyOnIdx);
}

// 実機再現報告: モノフォニックにもレイヤードパッチにもしていない通常の
// ポリフォニックチャンネルで、高速な連続ノートオン(ベースライン等)の際、
// リリース中の同一chを再利用して次のノートを鳴らすと、聴感上アタックが
// 再トリガーされず音程だけ滑らかに変化して聞こえるという報告の再現を試みる。
//
// ChState::wasReleasing は本来「run()直前がReleasing状態だったか」を見て、
// リリース中の同一chへの再ノートオン時にキャリアOPのRRを強制的に最大化
// (急速ダンプ)してからKey Onする「リリース中再トリガー対策」を発動させる
// フラグである。しかし通常のポリフォニック経路(CSoundDevice::assignCh()
// 経由)では、assign()がstatusをReleasing→Assignedへ書き換えてから
// run()が呼ばれるため、run()側の「status==Releasing」判定は常にfalseに
// なり、この対策が事実上一度も発動していなかった(2026年7月発見)。
TEST_CASE("Retriggering during Releasing phase (normal round-robin) sets wasReleasing and forces RR dump before Key On", "[sounddevice]")
{
    RecordingPort port;
    auto dev = createCOPN(&port, 8000000); // 3ch
    dev->init();

    HwPatch patch{}; // ALG=0 → キャリアはop3のみ(kCarrierMask[0]=0x08)
    patch.id = 1;
    DummyMidiCh owner;

    uint8_t ch1 = dev->allocCh(&owner, &patch, 100);
    REQUIRE(ch1 != 0xFF);
    dev->setNoteFine(ch1, 60, 0, true);
    dev->noteOn(ch1, 100);
    for (int t = 0; t < 5; ++t) dev->timerCallback(static_cast<uint32_t>(t));

    port.history.clear();
    dev->noteOff(ch1); // → Releasing

    // releaseTimerが満了する前に、同一owner+patchで即座に次のノートオン
    // (実際のログでは前のnoteOffから数msという間隔だった)。
    uint8_t ch2 = dev->allocCh(&owner, &patch, 100);
    REQUIRE(ch2 == ch1); // score=4でRelesing中の同じchを再利用するはず

    const auto* cs = dev->getChState(ch2);
    REQUIRE(cs != nullptr);
    // assignCh()内のassign()呼び出し時点で、既にwasReleasing=trueが
    // 確定していなければならない(この時点ではまだnoteOn()を呼んでいない
    // = run()未実行だが、assign()がここまでで正しく検出しているはず)。
    CHECK(cs->wasReleasing);

    dev->setNoteFine(ch2, 72, 0, true);
    dev->noteOn(ch2, 100);

    // run()実行後もwasReleasingがtrueのまま保持されていること。
    CHECK(cs->wasReleasing);

    // op3(キャリア、ALG=0)のRRレジスタ(0x80+kOpMap[3]+ch=0x80+12+0=0x8C)へ
    // SL<<4|0xF(RR強制最大化)が、Key On(reg 0x28, 0xF0)より前に
    // 書かれていることを確認する。
    int rrDumpIdx = -1, keyOnIdx = -1;
    for (size_t i = 0; i < port.history.size(); ++i) {
        const auto& [addr, data] = port.history[i];
        if (addr == 0x8C && data == 0x0F && rrDumpIdx < 0) rrDumpIdx = static_cast<int>(i);
        if (addr == 0x28 && data == 0xF0) keyOnIdx = static_cast<int>(i);
    }
    INFO("rrDumpIdx=" << rrDumpIdx << " keyOnIdx=" << keyOnIdx);
    for (size_t i = 0; i < port.history.size(); ++i) {
        INFO("write[" << i << "] addr=0x" << std::hex << (int)port.history[i].first
             << " data=0x" << (int)port.history[i].second);
    }
    CHECK(rrDumpIdx >= 0);
    CHECK(keyOnIdx >= 0);
    CHECK(rrDumpIdx < keyOnIdx);
}

// 実機報告: CC#1(ソフトウェアLFO)を使った後、まったく別のMIDIチャンネルで
// 発音した音にそのLFOが漏れて掛かることがある(2026年7月、[7c3d276]で
// CInstCh::noteOn()がdev->setCC1Modulation(devCh, pmDepth_, ...)を毎ノート
// オン強制pushするよう修正したはずだったが、後日同じ症状が再現すると
// 報告された)。
//
// 再調査の結果、CInstCh::noteOn()がこの強制pushを呼ぶタイミングは、
// dev->assignCh()(→ChState::status=Assigned)の直後・dev->noteOn()
// (→status=Running)より前であるのに対し、CSoundDevice::setCC1Modulation()
// 側のガードがisActive()(Running/Releasingのみtrue、Assignedはfalse)
// だったため、devChを他のMIDIチャンネルから奪う通常の経路では常に
// 早期returnし、強制pushが実質無効化されていたことが判明した(isAssigned()
// をガードへ追加して修正)。このテストはassignCh()直後・noteOn()より前の
// タイミングでsetCC1Modulation()を呼び、前の占有者(別owner)のソフトLFOが
// 残っていないことを検証する。
TEST_CASE("CC#1 soft LFO forced push during the Assigned window (after assignCh, "
          "before noteOn) actually takes effect on a channel stolen from another owner",
          "[sounddevice]")
{
    RecordingPort port;
    auto dev = createCOPN(&port, 8000000); // 3ch
    dev->init();

    HwPatch patch{}; // swPatch省略時は既定のSwPatch(LFR=0)が使われる
                      // → CC#1駆動ソフトLFOモードになる音色
    patch.id = 1;
    DummyMidiCh ownerA, ownerB;

    // ownerA: CC#1を送ってソフトLFOを起動させたまま発音終了する
    uint8_t chA = dev->allocCh(&ownerA, &patch, 100);
    REQUIRE(chA != 0xFF);
    dev->setNoteFine(chA, 60, 0, true);
    dev->noteOn(chA, 100);
    dev->setCC1Modulation(chA, 100, 600); // CC#1=100 → ソフトLFO起動
    REQUIRE(dev->getChState(chA)->proc.channelLfoActive());

    dev->noteOff(chA); // → Releasing。LFOはkReleasingHoldMsかけてfadeoutする
                        // だけで、ここではまだactive()==trueのまま

    // ownerB: 別のMIDIチャンネル。CC#1を一度も送っていない
    // (CInstCh::pmDepth_の既定値は0)。ラウンドロビンでchAを奪う。
    uint8_t chB = dev->allocCh(&ownerB, &patch, 100);
    REQUIRE(chB == chA); // 唯一使用中/Releasingのchなので再利用されるはず

    // CInstCh::noteOn()と同じ呼び出し順序: assignCh直後・noteOn()より前に
    // pmDepth_(=0、ownerBの既定値)を強制pushする。
    dev->setCC1Modulation(chB, 0, 600);
    dev->setNoteFine(chB, 72, 0, true);
    dev->noteOn(chB, 100);

    // 修正前はここでstatus==Assignedのガードによりpushが無視され、
    // ownerAの残留LFOがownerBへ漏れてchannelLfoActive()==trueのままに
    // なっていた。
    CHECK_FALSE(dev->getChState(chB)->proc.channelLfoActive());
}

// OPL4のAWM(PCM)部は実チップ上、FM部(port1/port2、addr 0x000-0x1FF)とは
// 独立した3つ目のレジスタバンク(addr 0x200以降、a_high=2)に配置される。
// CFITOM::resolveHighBankPort()はCOPL4AWMにOffsetPort(port,0x200)経由で
// アクセスさせることでこれを実現する(2026年7月、ユーザー指摘で発覚:
// 以前はAWM部がFM部と同じport1[a_high=0, addr 0x00-0xFF]にそのまま
// 割り当てられており、waveform番号レジスタ0x08等がFM部のレジスタと
// 衝突していた)。このテストはOffsetPort越しにCOPL4AWMを駆動し、実際に
// 書き込まれるアドレスが全てa_high=2の範囲(0x200-0x2FF)に収まることを
// 確認する回帰テスト。
// ユーザー報告: OPL系のピッチLFO(ソフトウェアビブラート)が「大変不自然に
// 聞こえる」、レジスタダンプ上KONビットも反転しているように見える、との
// こと。調査の結果、KONビット自体はCOPL::updateFreq()がgetReg(0xB0+ch)から
// 読み出したビットをOR合成して書き戻す設計になっており正しく保持されていた
// (OPL_new.cpp参照)。実際の根本原因は別にあった: CSoundDevice::timerCallback()
// がVoiceProcessor::onTick()へ渡すFmVoiceを手組みしており、hw/hwOpのみ埋めて
// sw/swOpを埋め忘れていたため(buildVoiceForCh()と不整合)、tick毎に
// recalcChLfo/recalcOpLfoが常にsw側デフォルト値(LFR=0/depthCents=0等)を
// 参照し、音色固有のソフトウェアピッチLFO/トレモロが実質常にデプス0で
// 無効化されていた(2026年7月修正、timerCallback()もbuildVoiceForCh()を
// 使うよう統一)。このテストは、ノートオン中にソフトウェアピッチLFOを
// 回した場合に (1) tick毎のFnum再書き込みでKONビットが落ちないこと、
// (2) LFOが実際にFnumへ反映されること、の両方を検証する。
TEST_CASE("OPL soft pitch LFO ticks keep the KON bit set in reg 0xB0 across Fnum updates",
          "[sounddevice][opl]")
{
    RecordingPort port;
    auto dev = createCOPL(&port, 3579545);
    dev->init();

    HwPatch patch{};
    patch.id = 1;

    SwPatch sw;
    sw.id = 1;
    sw.sw.LFR = 64;          // ソフトLFO有効 (rate中程度)
    sw.sw.LFD = 0;           // ディレイなし、即座に開始
    sw.sw.LFI = 0;           // フェードインなし、即フルデプス
    sw.sw.LWF = 6;           // sine
    sw.sw.LFM = 0;           // repeat
    sw.sw.depthCents = 200;  // ±200セント、明確にFnumが変化する深さ

    uint8_t ch = dev->allocCh(nullptr, &patch, 100, &sw);
    REQUIRE(ch != 0xFF);
    dev->setNoteFine(ch, 60, 0, true);
    dev->noteOn(ch, 100);
    REQUIRE(dev->getChState(ch)->proc.channelLfoActive());

    uint16_t a0reg = static_cast<uint16_t>(0xA0 + ch); // fnum[8:1]
    uint16_t b0reg = static_cast<uint16_t>(0xB0 + ch); // KON | block | fnum[10:9]
    REQUIRE((port.regs[b0reg] & 0x20) != 0); // noteOn直後、KONは立っているはず

    bool fnumChanged = false;
    uint8_t initialA0 = port.regs[a0reg];
    uint8_t initialB0 = port.regs[b0reg];
    for (uint32_t t = 0; t < 60; ++t) {
        dev->timerCallback(t);
        INFO("tick=" << t << " reg[0xA0+ch]=0x" << std::hex << (int)port.regs[a0reg]
             << " reg[0xB0+ch]=0x" << (int)port.regs[b0reg]);
        // ソフトLFOがアクティブな間、ノートオン中は常にKONビットが
        // 立っていなければならない(Fnum更新の副作用でKONが落ちてはならない)。
        CHECK((port.regs[b0reg] & 0x20) != 0);
        if (port.regs[a0reg] != initialA0 || port.regs[b0reg] != initialB0)
            fnumChanged = true;
    }
    // LFOが実際にFnum/Blockを動かしていること(そもそも変調が起きていない
    // 状態でKONが落ちないのは当然のため、変調が起きた上での検証であることを保証)。
    CHECK(fnumChanged);
}

TEST_CASE("COPL4AWM writes land in the 0x200 high bank via OffsetPort, "
          "not the FM part's 0x000/0x100 banks", "[sounddevice][opl4]")
{
    RecordingPort port;
    OffsetPort awmPort(&port, 0x200); // CFITOM::resolveHighBankPort()が生成するものと同じ
    auto dev = createCOPL4AWM(&awmPort, 44100);
    dev->init();

    HwPatch patch{};
    patch.id = 1;

    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev->setNoteFine(ch, 60, 0, true);
    dev->noteOn(ch, 100);

    REQUIRE_FALSE(port.history.empty());
    for (const auto& [addr, data] : port.history) {
        (void)data;
        INFO("write addr=0x" << std::hex << (int)addr);
        CHECK((addr & 0xFF00) == 0x200); // a_high=2、FM部の0x000/0x100とは衝突しない
    }
}

// ymfm(pcm_engine::write(), extern/ymfm/src/ymfm_pcm.cpp。YMEngineが使う
// エミュレーションコア)は、reg 0x08-0x1F(波形番号下位8bit)への書き込みで
// 即座にload_wavetable()をトリガーし、その時点のreg 0x20+ch bit0(波形番号
// 上位1bit)と組み合わせて9bit波形番号を確定させる。この順序を逆にする
// (reg 0x08を先に書く)と、新しいノートのbit0が反映される前の(1つ前の
// ノートの)bit0でロードがトリガーされ、意図した波形と256ずれた別の波形が
// 鳴ってしまう(2026年7月、ユーザー報告「AWMの音は出るが意図した波形でない」
// により発覚)。ALSAのopl4_synth.c(snd_opl4_note_on())でも
// OPL4_REG_F_NUMBER(reg 0x20+ch)をOPL4_REG_TONE_NUMBER(reg 0x08+ch)より
// 先に書いており、後者のコメントに明示的に「triggers header loading」と
// 書かれている(実チップ・エミュレータ双方でこの順序が必須であることの
// 裏付け)。
TEST_CASE("COPL4AWM writes the wave-number high bit (reg 0x20) before the "
          "low byte (reg 0x08) so ymfm's load-trigger sees the correct "
          "combined wave number", "[sounddevice][opl4]")
{
    RecordingPort port;
    OffsetPort awmPort(&port, 0x200);
    auto dev = createCOPL4AWM(&awmPort, 44100);
    dev->init();

    SampleZonePatch patch;
    patch.id = 1;
    patch.zones.push_back(SampleZone{0, 63, 0, 127, 44});    // bit8=0
    patch.zones.push_back(SampleZone{64, 127, 0, 127, 304}); // bit8=1 (0x130)

    uint8_t ch = dev->allocCh(nullptr, nullptr, 100, nullptr, &patch);
    REQUIRE(ch != 0xFF);

    // 1本目: bit8=0の波形を鳴らし、reg 0x20 bit0を0で確定させておく。
    dev->setNoteFine(ch, 40, 0, true);
    dev->noteOn(ch, 100);
    dev->noteOff(ch);

    port.history.clear(); // 2本目のノートオンで実際に書かれるレジスタだけを見る

    // 2本目: bit8=1の波形に切り替える。
    dev->setNoteFine(ch, 90, 0, true);
    dev->noteOn(ch, 100);

    // reg 0x20はupdateFnumber()(Fnum bits、bit0保持)とupdateVoice()
    // (bit0そのもの)の両方から書かれるため、「最初の」出現ではなく
    // updateVoice()内で最後に書かれる箇所(=各レジスタへの最後の書き込み)
    // で比較する必要がある(そうしないと、updateVoice()より前のupdateFnumber()
    // による無関係なreg 0x20書き込みを誤って捉えてしまい、updateVoice()内の
    // 順序バグを検出できない)。
    const uint16_t reg20 = static_cast<uint16_t>(0x200 + 0x20 + ch);
    const uint16_t reg08 = static_cast<uint16_t>(0x200 + 0x08 + ch);
    int lastIdx20 = -1, lastIdx08 = -1;
    for (size_t i = 0; i < port.history.size(); ++i) {
        if (port.history[i].first == reg20) lastIdx20 = static_cast<int>(i);
        if (port.history[i].first == reg08) lastIdx08 = static_cast<int>(i);
    }
    REQUIRE(lastIdx20 >= 0);
    REQUIRE(lastIdx08 >= 0);
    // updateVoice()内で、reg 0x20(bit8)への書き込みがreg 0x08(下位8bit、
    // ロード起動)への書き込みより後にずれ込んでいないこと。
    CHECK(lastIdx20 < lastIdx08);
    // reg 0x08書き込み(ロード起動)の時点で、reg 0x20のbit0が既に新しい
    // 波形の上位bitに更新されていること(=誤った波形番号でロードされていない
    // ことの直接的な確認)。
    CHECK((port.regs[reg20] & 0x01) == 1);
}

// OPL4 AWMのFnumber/Octaveはymfm(extern/ymfm/src/ymfm_pcm.cpp)の
// 「符号付き4bit octave(-8〜+7) + 1024〜2047正規化fnum」形式に従う必要が
// あり、かつ波形ごとの校正値(SampleZone::pitchOffset/keyScaling、ALSAの
// sound/drivers/opl4/yrw801.cから移植)を反映しなければならない
// (2026年8月、ユーザー報告「AWMは音は出るが意図した波形と異なる」の
// 調査で発覚。以前はOPN用のgetFnumberFromHz()を誤用しており、波形ごとの
// 校正が反映されず、octaveも0〜7にクランプされ負のoctaveを表現できて
// いなかった)。
// ALSAのsnd_opl4_update_pitch()の基準点(note=60, pitch_offset=0,
// key_scaling=100 → pitch=60*128=7680 → octave=7680/0x600-8=-3,
// fnum=snd_opl4_pitch_map[0]=0)と一致することを、独立した手計算値で確認する。
TEST_CASE("COPL4AWM Fnumber/Octave matches the ALSA opl4_synth.c reference "
          "point and honors per-wave pitch calibration (pitch_offset)",
          "[sounddevice][opl4]")
{
    RecordingPort port;
    OffsetPort awmPort(&port, 0x200);
    auto dev = createCOPL4AWM(&awmPort, 44100);
    dev->init();

    auto makePatch = [](int16_t pitchOffset) {
        SampleZonePatch patch;
        patch.id = 1;
        SampleZone zone{};
        zone.keyMin = 0; zone.keyMax = 127;
        zone.waveIndex = 300;
        zone.pitchOffset = pitchOffset;
        zone.keyScaling = 100;
        patch.zones.push_back(zone);
        return patch;
    };

    // pitch_offset=0: ALSAの基準点そのもの。octave=-3(2の補数4bitで0xD)、fnum=0。
    SampleZonePatch patch0 = makePatch(0);
    uint8_t ch0 = dev->allocCh(nullptr, nullptr, 100, nullptr, &patch0);
    REQUIRE(ch0 != 0xFF);
    dev->setNoteFine(ch0, 60, 0, true);
    dev->noteOn(ch0, 100);

    const uint16_t reg38_0 = static_cast<uint16_t>(0x200 + 0x38 + ch0);
    const uint16_t reg20_0 = static_cast<uint16_t>(0x200 + 0x20 + ch0);
    CHECK(((port.regs[reg38_0] >> 4) & 0xF) == 0xD);      // octave=-3
    CHECK((port.regs[reg38_0] & 0x07) == 0);              // fnum上位3bit=0
    CHECK(((port.regs[reg20_0] >> 1) & 0x7F) == 0);       // fnum下位7bit=0

    // pitch_offset=0x600(=1536、ちょうど1オクターブ分): 同じnote/key_scalingで
    // octaveだけ+1され、fnumは変わらないはず(within=0のまま)。
    SampleZonePatch patch1 = makePatch(0x600);
    uint8_t ch1 = dev->allocCh(nullptr, nullptr, 100, nullptr, &patch1);
    REQUIRE(ch1 != 0xFF);
    REQUIRE(ch1 != ch0);
    dev->setNoteFine(ch1, 60, 0, true);
    dev->noteOn(ch1, 100);

    const uint16_t reg38_1 = static_cast<uint16_t>(0x200 + 0x38 + ch1);
    const uint16_t reg20_1 = static_cast<uint16_t>(0x200 + 0x20 + ch1);
    CHECK(((port.regs[reg38_1] >> 4) & 0xF) == 0xE);      // octave=-2 (-3+1)
    CHECK((port.regs[reg38_1] & 0x07) == 0);
    CHECK(((port.regs[reg20_1] >> 1) & 0x7F) == 0);
}

// ユーザー報告(2026年7月): OPLビルトインリズムでバスドラム(BD)だけ発音せず、
// 他4楽器(HH/SD/TOM/CYM)は正常に鳴る。COPLRhythm::writeOperatorRegs()が、
// キャリア側のエンベロープ(AR/DR/SL/SR/RR)をVoiceProcessorから読む際、
// 実際のオペレータ番号(velOpIdx)ではなく常にop0を参照していたバグ
// (HH/SD/TOM/CYMは単一オペレータでhwOp[0]のみ使用するため偶然op0参照が
// 正しく、BDだけ2オペレータでキャリアがop1のため誤ってop0[モジュレータ]側
// の値を適用してしまっていた)。ベロシティ127(sens_vel=0)ならVAR等の感度
// に関わらずvelAR(op)==hwOp[op].ARになる(VoiceProcessor::onNoteOn参照)ため、
// hwOp[0].ARとhwOp[1].ARを大きく異なる値にしておけば、実際に書き込まれる
// レジスタがどちらの値を反映しているか直接判別できる。
TEST_CASE("COPLRhythm BD (bass drum) carrier envelope uses operator 1's "
          "VoiceProcessor values, not operator 0's (modulator)", "[sounddevice][opl][rhythm]")
{
    RecordingPort port;
    auto dev = createCOPLRhythm(&port, 3579545);
    dev->init();

    HwPatch patch{};
    patch.id = 1;
    patch.hw.ALG = 0; // 直列 (op1のみキャリア)
    patch.hwOp[0].AR = 2;  // モジュレータ: 誤って参照されると期待値と食い違う
    patch.hwOp[1].AR = 30; // キャリア: 本来参照されるべき値

    // BD(論理ch4)は物理ch6・スロット16に固定配置される(COPLRhythmクラス
    // 冒頭コメント参照)。PatchManager::resolveDirect()のforcedCh機構と
    // 同様、assignCh()でチャンネル番号を直接指定する。
    uint8_t ch = dev->assignCh(4, nullptr, &patch, 127);
    REQUIRE(ch == 4);
    dev->noteOn(ch, 127);

    // op1(キャリア)のAR/DRレジスタ: 0x60 + opIdx(1)*3 + slot(16) = 0x73
    uint16_t arDrReg = 0x73;
    uint8_t writtenAR = static_cast<uint8_t>(port.regs[arDrReg] >> 4); // 上位4bit
    CHECK(writtenAR == (30 >> 1)); // ar4(30) = 15 (修正後、op1基準)
    CHECK(writtenAR != (2 >> 1));  // ar4(2) = 1 (修正前のバグはこちらになっていた)
}

// ユーザー要望(2026年7月): OPL/OPLLビルトインリズムでFnumを共有する
// チャンネル(ch7=HH/SD共有、ch8=CYM/TOM共有)のうち、HH・CYMはノイズ性の
// 発振でFnumがほぼ発音に寄与しないため、この2チャンネルではFnum更新を
// 無効化してほしい。ピッチ変化に意味のあるSD・TOM側と競合するため。
TEST_CASE("COPLRhythm: HH (ch0) does not overwrite the Fnum SD (ch3) wrote "
          "to their shared physical channel (ch7)", "[sounddevice][opl][rhythm]")
{
    RecordingPort port;
    auto dev = createCOPLRhythm(&port, 3579545);
    dev->init();

    HwPatch patch{};
    patch.id = 1;

    uint8_t sdCh = dev->assignCh(3, nullptr, &patch, 100); // SD
    REQUIRE(sdCh == 3);
    dev->setNoteFine(sdCh, 40, 0, true);
    dev->noteOn(sdCh, 100);

    uint16_t a0reg = 0xA0 + 7; // physCh7 (HH/SD共有)
    uint16_t b0reg = 0xB0 + 7;
    uint8_t sdA0 = port.regs[a0reg];
    uint8_t sdB0 = port.regs[b0reg] & 0x1F; // block(bit4-2)+fnum上位(bit1-0)。bit5=KONは除外

    uint8_t hhCh = dev->assignCh(0, nullptr, &patch, 100); // HH (SDと異なる論理ch)
    REQUIRE(hhCh == 0);
    dev->setNoteFine(hhCh, 100, 0, true); // SD(note=40)と大きく異なるノート
    dev->noteOn(hhCh, 100);

    CHECK(port.regs[a0reg] == sdA0);
    CHECK((port.regs[b0reg] & 0x1F) == sdB0);
}

TEST_CASE("COPLLRhythm: HH (ch0) does not overwrite the Fnum SD (ch3) wrote "
          "to their shared physical channel (ch7)", "[sounddevice][opll][rhythm]")
{
    RecordingPort port;
    auto dev = createCOPLLRhythm(&port, 3579545);
    dev->init();

    HwPatch patch{};
    patch.id = 1;

    uint8_t sdCh = dev->assignCh(3, nullptr, &patch, 100); // SD
    REQUIRE(sdCh == 3);
    dev->setNoteFine(sdCh, 40, 0, true);
    dev->noteOn(sdCh, 100);

    uint16_t fnumLoReg = 0x10 + 7; // physCh7 (HH/SD共有)
    uint16_t blockReg  = 0x20 + 7;
    uint8_t sdFnumLo = port.regs[fnumLoReg];
    uint8_t sdBlock  = port.regs[blockReg] & 0x0F; // block(bit3-1)+fnum上位(bit0)。bit5-4=KON/sus等は除外

    uint8_t hhCh = dev->assignCh(0, nullptr, &patch, 100); // HH
    REQUIRE(hhCh == 0);
    dev->setNoteFine(hhCh, 100, 0, true); // SD(note=40)と大きく異なるノート
    dev->noteOn(hhCh, 100);

    CHECK(port.regs[fnumLoReg] == sdFnumLo);
    CHECK((port.regs[blockReg] & 0x0F) == sdBlock);
}

// ================================================================
//  ワンショット系デバイス (ADPCM-A / OPNA内蔵リズム) の消音制御
//
//  この2つは NoteOff で音が止まらない (ワンショットのドラム音は短い
//  ゲートタイムで途中から切られるより最後まで鳴らすほうが自然、という
//  意図的な設計)。そのため:
//    ① 明示的な消音要求 (チョークグループ / CC#120) は forceDamp() が
//       実際にダンプビットを書かなければならない。ISoundDevice の
//       forceDamp() 既定実装は noteOff() を呼ぶだけなので、これらの
//       チップで override を怠ると「止める手段が一切ない」状態になる。
//    ② 同一チャンネルを再割り当てするとき (CRhythmCh の同一ノート再打鍵)、
//       前のサンプルがまだ再生中である可能性が常にあるため、キーオンの
//       前にダンプを挟まないと確実な物理的リトリガーにならない。
//  (2026年8月、ドラムのチョークグループが機能していなかった件の修正)
// ================================================================

TEST_CASE("CAdPcm2610A does not stop on NoteOff but forceDamp writes the dump bit",
          "[sounddevice][adpcma][choke]")
{
    RecordingPort port;
    auto dev = createCAdPcm(&port, 8000000, DEVICE_ADPCMA);
    REQUIRE(dev);
    dev->init();

    uint8_t ch = dev->allocCh(nullptr, nullptr, 100);
    REQUIRE(ch != 0xFF);
    dev->noteOn(ch, 100);

    // NoteOff: キーオン/ダンプレジスタ($00)へは一切書かない
    port.history.clear();
    dev->noteOff(ch);
    for (const auto& [addr, data] : port.history) {
        (void)data;
        CHECK(addr != 0x00);
    }

    // forceDamp: 該当chのダンプビット(bit7)を立てて書く
    port.history.clear();
    dev->forceDamp(ch);
    bool dumped = false;
    for (const auto& [addr, data] : port.history) {
        if (addr == 0x00 && data == (0x80 | (1u << ch))) dumped = true;
    }
    CHECK(dumped);
}

TEST_CASE("CAdPcm2610A dumps the channel before keying it on again",
          "[sounddevice][adpcma][choke]")
{
    RecordingPort port;
    auto dev = createCAdPcm(&port, 8000000, DEVICE_ADPCMA);
    REQUIRE(dev);
    dev->init();

    uint8_t ch = dev->allocCh(nullptr, nullptr, 100);
    REQUIRE(ch != 0xFF);
    dev->noteOn(ch, 100);
    dev->noteOff(ch);

    // 同一chの再割り当て (CRhythmCh の同一ノート再打鍵と同じ経路)
    port.history.clear();
    REQUIRE(dev->assignCh(ch, nullptr, nullptr, 100) == ch);
    dev->noteOn(ch, 100);

    // reg $00 への書き込みが「ダンプ → キーオン」の順で現れること
    int dumpIdx = -1, keyOnIdx = -1;
    for (size_t i = 0; i < port.history.size(); ++i) {
        const auto& [addr, data] = port.history[i];
        if (addr != 0x00) continue;
        if (data == (0x80 | (1u << ch)) && dumpIdx < 0) dumpIdx = static_cast<int>(i);
        if (data == (1u << ch) && keyOnIdx < 0)         keyOnIdx = static_cast<int>(i);
    }
    REQUIRE(dumpIdx >= 0);
    REQUIRE(keyOnIdx >= 0);
    CHECK(dumpIdx < keyOnIdx);
}

TEST_CASE("COPNARhythm does not stop on NoteOff but forceDamp writes the dump bit",
          "[sounddevice][opna][rhythm][choke]")
{
    RecordingPort port;
    auto dev = createCOPNARhythm(&port, 8000000);
    dev->init();

    HwPatch patch{};
    patch.id = 1;

    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev->noteOn(ch, 100);

    port.history.clear();
    dev->noteOff(ch);
    for (const auto& [addr, data] : port.history) {
        (void)data;
        CHECK(addr != 0x10);
    }

    port.history.clear();
    dev->forceDamp(ch);
    bool dumped = false;
    for (const auto& [addr, data] : port.history) {
        if (addr == 0x10 && data == (0x80 | (1u << ch))) dumped = true;
    }
    CHECK(dumped);
}

TEST_CASE("COPNARhythm dumps the part before keying it on again",
          "[sounddevice][opna][rhythm][choke]")
{
    RecordingPort port;
    auto dev = createCOPNARhythm(&port, 8000000);
    dev->init();

    HwPatch patch{};
    patch.id = 1;

    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev->noteOn(ch, 100);
    dev->noteOff(ch);

    port.history.clear();
    REQUIRE(dev->assignCh(ch, nullptr, &patch, 100) == ch);
    dev->noteOn(ch, 100);

    int dumpIdx = -1, keyOnIdx = -1;
    for (size_t i = 0; i < port.history.size(); ++i) {
        const auto& [addr, data] = port.history[i];
        if (addr != 0x10) continue;
        if (data == (0x80 | (1u << ch)) && dumpIdx < 0) dumpIdx = static_cast<int>(i);
        if (data == (1u << ch) && keyOnIdx < 0)         keyOnIdx = static_cast<int>(i);
    }
    REQUIRE(dumpIdx >= 0);
    REQUIRE(keyOnIdx >= 0);
    CHECK(dumpIdx < keyOnIdx);
}
