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
