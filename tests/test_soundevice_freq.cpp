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
    void write(uint16_t addr, uint16_t data) override { regs[addr] = static_cast<uint8_t>(data); }
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
