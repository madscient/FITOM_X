// tests/test_saa1099.cpp
// SAA1099 チップドライバ CSAA1099 の音程の回帰テスト。
//
// 発音周波数は f = (clock/512) × 2^oct / (511 - N) (N: 周波数レジスタ
// 0x08+ch、oct: オクターブレジスタ 0x10+ch/2 のnibble)。書かれたレジスタから
// この式で周波数を逆算し、音程の入力(ノート・kfs単位の微調整・チャンネル
// LFO・マスターピッチ)が他チップと同じ規約で反映されることを確かめる。

#include <catch2/catch_test_macros.hpp>
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include "fitom/FITOMdefine.h"
#include "fitom/FnumUtils.h"
#include "fitom/DeviceFactory.h"
#include <cmath>
#include <map>
#include <memory>

using namespace fitom;

namespace {

class RecordingPort : public IPort {
public:
    std::map<uint16_t, uint8_t> regs;

    void write(uint16_t addr, uint16_t data) override {
        regs[addr] = static_cast<uint8_t>(data);
    }
    uint8_t read(uint16_t addr) override {
        auto it = regs.find(addr);
        return it != regs.end() ? it->second : 0;
    }
    int clock = 8000000;
    int getClock() override { return clock; }
};

double writtenHz(RecordingPort& port, uint8_t ch)
{
    const uint8_t n = port.regs[static_cast<uint16_t>(0x08 + ch)];
    const uint8_t octReg = port.regs[static_cast<uint16_t>(0x10 + ch / 2)];
    const int oct = (ch % 2) ? ((octReg >> 4) & 7) : (octReg & 7);
    return (port.clock / 512.0) * std::pow(2.0, oct) / (511.0 - n);
}

uint16_t writtenRegs(RecordingPort& port, uint8_t ch)
{
    const uint8_t octReg = port.regs[static_cast<uint16_t>(0x10 + ch / 2)];
    const uint8_t oct = (ch % 2) ? ((octReg >> 4) & 7) : (octReg & 7);
    return static_cast<uint16_t>((oct << 8) | port.regs[static_cast<uint16_t>(0x08 + ch)]);
}

// FnumRegistry のマスターピッチは大域状態なので、テストの終わりに必ず戻す。
struct MasterPitchGuard {
    explicit MasterPitchGuard(double hz) { FnumRegistry::instance().setMasterPitch(hz); }
    ~MasterPitchGuard() { FnumRegistry::instance().setMasterPitch(440.0); }
};

} // namespace

TEST_CASE("SAA1099 treats fineFreq as kfs (64 per semitone): note 60 + 64 "
          "equals note 61", "[saa1099]")
{
    RecordingPort port;
    auto dev = DeviceFactory::create(DEVICE_SAA, &port, 44100);
    REQUIRE(dev != nullptr);
    dev->init();

    HwPatch patch{};
    patch.id = 1;

    uint8_t chA = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(chA != 0xFF);
    dev->setNoteFine(chA, 61, 0, true);
    dev->noteOn(chA, 100);

    uint8_t chB = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(chB != 0xFF);
    REQUIRE(chB != chA);
    dev->setNoteFine(chB, 60, 64, true);
    dev->noteOn(chB, 100);

    CHECK(writtenRegs(port, chA) == writtenRegs(port, chB));
}

// writtenHz() の式に頼らず、レジスタ値そのものを見る。
// 8MHz: 15625 × 2^3 / (511 - 227) = 440.14Hz
TEST_CASE("SAA1099 writes octave 3 and N=227 for A4 at 8MHz", "[saa1099]")
{
    RecordingPort port;
    auto dev = DeviceFactory::create(DEVICE_SAA, &port, 44100);
    REQUIRE(dev != nullptr);
    dev->init();

    HwPatch patch{};
    patch.id = 1;
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev->setNoteFine(ch, 69, 0, true);
    dev->noteOn(ch, 100);

    CHECK(writtenRegs(port, ch) == ((3 << 8) | 227));
}

TEST_CASE("SAA1099 follows the master pitch", "[saa1099]")
{
    MasterPitchGuard guard(432.0);

    RecordingPort port;
    auto dev = DeviceFactory::create(DEVICE_SAA, &port, 44100);
    REQUIRE(dev != nullptr);
    dev->init();

    HwPatch patch{};
    patch.id = 1;
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev->setNoteFine(ch, 69, 0, true);
    dev->noteOn(ch, 100);

    // A4=432Hz付近では周波数レジスタ1段がおよそ1.5Hzにあたる。
    CHECK(std::abs(writtenHz(port, ch) - 432.0) < 1.0);
}

// 他のPSG系と同じく、HWプラグインが報告するクロックで周波数を計算する
// (Game Blaster は 7.159MHz)。
TEST_CASE("SAA1099 computes the frequency from the port's clock", "[saa1099]")
{
    RecordingPort port;
    port.clock = 7159090;
    auto dev = DeviceFactory::create(DEVICE_SAA, &port, 44100);
    REQUIRE(dev != nullptr);
    dev->init();

    HwPatch patch{};
    patch.id = 1;
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev->setNoteFine(ch, 69, 0, true);
    dev->noteOn(ch, 100);

    CHECK(std::abs(writtenHz(port, ch) - 440.0) < 1.0);
}

TEST_CASE("SAA1099 applies the software pitch LFO to the frequency registers",
          "[saa1099]")
{
    RecordingPort port;
    auto dev = DeviceFactory::create(DEVICE_SAA, &port, 44100);
    REQUIRE(dev != nullptr);
    dev->init();

    HwPatch patch{};
    patch.id = 1;

    SwPatch sw;
    sw.id = 1;
    sw.sw.LFR = 64;          // ソフトLFO有効
    sw.sw.LFD = 0;           // ディレイなし
    sw.sw.LFI = 0;           // フェードインなし
    sw.sw.LWF = 6;           // sine
    sw.sw.LFM = 0;           // repeat
    sw.sw.depthCents = 200;  // ±200セント

    uint8_t ch = dev->allocCh(nullptr, &patch, 100, &sw);
    REQUIRE(ch != 0xFF);
    dev->setNoteFine(ch, 60, 0, true);
    dev->noteOn(ch, 100);
    REQUIRE(dev->getChState(ch)->proc.channelLfoActive());

    const uint16_t initial = writtenRegs(port, ch);
    bool changed = false;
    for (uint32_t t = 0; t < 60 && !changed; ++t) {
        dev->timerCallback(t);
        if (writtenRegs(port, ch) != initial) changed = true;
    }
    CHECK(changed);
}
