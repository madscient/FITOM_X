// tests/test_opm_lfo.cpp
// OPM/OPP/OPZ のハードウェアLFOまわりの回帰テスト。
// LFO の Rate($18)・Depth($19、PMD/AMD の2つのラッチ)はチップ全体で1系統で、
// ドライバは同じ値の書き込みを省くために控えを持つ。控えとチップの値が
// 食い違うと、CC#14/#15 の書き込みが省かれてチップに古い値が残る。
// チャンネルごとの感度($38+ch)は PMS(b6-4)と AMS(b1-0)を1バイトに持つ。

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include <map>
#include <memory>
#include <utility>
#include <vector>

using namespace fitom;

namespace fitom {
std::unique_ptr<ISoundDevice> createCOPM(IPort* p, int masterClock);
std::unique_ptr<ISoundDevice> createCOPP(IPort* p, int masterClock);
std::unique_ptr<ISoundDevice> createCOPZ(IPort* p, int masterClock);
}

namespace {

class RecordingPort : public IPort {
public:
    std::map<uint16_t, uint8_t> regs;
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

std::unique_ptr<ISoundDevice> makeDevice(int kind, IPort* port)
{
    switch (kind) {
    case 0:  return createCOPM(port, 0);
    case 1:  return createCOPP(port, 0);
    default: return createCOPZ(port, 0);
    }
}

} // namespace

TEST_CASE("OPM-family reset() clears the LFO rate and both depth latches on the chip",
          "[opm][lfo]")
{
    const int kind = GENERATE(0, 1, 2);
    INFO("kind(0=OPM,1=OPP,2=OPZ)=" << kind);
    RecordingPort port;
    auto dev = makeDevice(kind, &port);
    dev->init();

    dev->setLFORate(0, 30);
    dev->setLFODepth(0, 50);
    REQUIRE(port.regs[0x18] != 0);
    REQUIRE(port.regs[0x19] != 0);

    port.history.clear();
    dev->reset();
    CHECK(port.regs[0x18] == 0);
    bool pmdCleared = false, amdCleared = false;
    for (const auto& [addr, data] : port.history) {
        if (addr != 0x19) continue;
        if (data == 0x80) pmdCleared = true;
        if (data == 0x00) amdCleared = true;
    }
    CHECK(pmdCleared);
    CHECK(amdCleared);

    // リセット後に同じ値をもう一度送れば、省かれずにチップへ届く
    dev->setLFODepth(0, 50);
    CHECK(port.regs[0x19] == 50);
}

// 挙動を固定するためのテスト。$38+ch は PMS と AMS を独立に立て下げできる。
TEST_CASE("OPM-family enablePM/enableAM set PMS and AMS of $38+ch independently",
          "[opm][lfo]")
{
    const int kind = GENERATE(0, 1, 2);
    INFO("kind(0=OPM,1=OPP,2=OPZ)=" << kind);
    RecordingPort port;
    auto dev = makeDevice(kind, &port);
    dev->init();

    HwPatch patch{};
    patch.id = 1;
    patch.hw.PMS = 5;
    patch.hw.AMS = 2;
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    const uint16_t reg = static_cast<uint16_t>(0x38 + ch);
    CHECK(port.regs[reg] == 0x00);   // 音色の書き込みでいったん無効になる

    dev->enablePM(ch, true);
    CHECK(port.regs[reg] == 0x50);
    dev->enableAM(ch, true);
    CHECK(port.regs[reg] == 0x52);
    dev->enablePM(ch, false);
    CHECK(port.regs[reg] == 0x02);
    dev->enablePM(ch, true);
    dev->enableAM(ch, false);
    CHECK(port.regs[reg] == 0x50);
}
