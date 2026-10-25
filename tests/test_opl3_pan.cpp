// tests/test_opl3_pan.cpp
// OPL3(4OP/2OP)のパンポットが、0xC0+chの出力イネーブルをCHA(bit4)=L、
// CHB(bit5)=Rの向きで書くことの回帰テスト。ymfmはbit4の出力をdata[0]へ出し、
// YMFMEngineはdata[0]を左へ送る(OPL_new.cppのCOPL::outputEnableBits参照)。

#include <catch2/catch_test_macros.hpp>
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include "fitom/FITOMdefine.h"
#include "fitom/DeviceFactory.h"
#include <map>
#include <utility>
#include <vector>

using namespace fitom;

namespace {

class LoggingPort : public IPort {
public:
    std::map<uint16_t, uint8_t> regs;
    std::vector<std::pair<uint16_t, uint8_t>> log;
    void write(uint16_t addr, uint16_t data) override {
        regs[addr] = static_cast<uint8_t>(data);
        log.emplace_back(addr, static_cast<uint8_t>(data));
    }
    uint8_t read(uint16_t addr) override {
        auto it = regs.find(addr);
        return it != regs.end() ? it->second : 0;
    }
};

// setPanpot()が書いた0xC0-0xC8(両バンク)の値のbit5-4を集める
std::vector<uint8_t> panBitsWritten(ISoundDevice& dev, LoggingPort& port, uint8_t ch, int8_t pan)
{
    port.log.clear();
    dev.setPanpot(ch, pan, true);
    std::vector<uint8_t> bits;
    for (const auto& [addr, data] : port.log) {
        const uint16_t low = addr & 0xFF;
        if (low >= 0xC0 && low <= 0xC8) bits.push_back(data & 0x30);
    }
    return bits;
}

} // namespace

TEST_CASE("OPL3 panpot writes CHA (bit4) for left and CHB (bit5) for right", "[opl3][pan]")
{
    for (uint32_t type : {uint32_t(DEVICE_OPL3), uint32_t(DEVICE_OPL3_2)}) {
        INFO("device=0x" << std::hex << type);
        LoggingPort port;
        auto dev = DeviceFactory::create(type, &port, 44100);
        REQUIRE(dev != nullptr);
        dev->init();
        HwPatch patch{};
        patch.id = 1;
        const uint8_t ch = dev->allocCh(nullptr, &patch, 100);
        REQUIRE(ch != 0xFF);

        struct Expect { int8_t pan; uint8_t bits; };
        for (const auto& e : {Expect{-64, 0x10}, Expect{63, 0x20}, Expect{0, 0x30}}) {
            INFO("pan=" << int(e.pan));
            const auto bits = panBitsWritten(*dev, port, ch, e.pan);
            REQUIRE_FALSE(bits.empty());
            for (uint8_t b : bits) CHECK(b == e.bits);
        }
    }
}
