// tests/test_fm_clock.cpp
// FM系チップドライバが、ポートの報告するクロック(エミュレーターなら
// プラグイン設定のclock)から音程を計算することの回帰テスト。
//
// 期待値は、ドライバが書くF-Number/Blockを実チップが鳴らす周波数へ
// 戻して求める。戻す式はymfmの位相の進め方による(位相は2^20で1周期):
//   OPN系 : 1サンプルあたり F11 × 2^(B-1)  (ymfm_opn.cpp compute_phase_step)
//   OPL系 : 1サンプルあたり F10 × 2^B      (ymfm_opl.cpp opl_compute_phase_step)
//   OPLL系: 1サンプルあたり F9  × 2^(B+1)  (OPLと同じ式にblock_freq<<1を渡す)
// 1サンプルのクロック数は、YM2203が72、YM2608/YM2612/YM2610が144、
// YM3526/YM3812/YM2413が72、YMF262が288、YMF278BのFM部が684
// (ymfm_opn.hのprescaleの表、ymfm_opl.hのymf278b)。

#include <catch2/catch_test_macros.hpp>
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include "fitom/FITOMdefine.h"
#include "fitom/DeviceFactory.h"
#include "fitom/Config.h"
#include <cmath>
#include <map>
#include <memory>
#include <vector>

using namespace fitom;

namespace {

class ClockPort : public IPort {
public:
    explicit ClockPort(int c) : clock(c) {}
    std::map<uint16_t, uint8_t> regs;
    void write(uint16_t addr, uint16_t data) override {
        regs[addr] = static_cast<uint8_t>(data);
    }
    uint8_t read(uint16_t addr) override {
        auto it = regs.find(addr);
        return it != regs.end() ? it->second : 0;
    }
    int clock;
    int getClock() override { return clock; }
};

enum class Family { Opn, Opl, Opll };

struct Played {
    double hz;
    // F-Numberの量子化で生じうるずれ。OPL/OPLLは11bitで丸めた値を
    // 切り捨ててレジスタへ書くため、レジスタの1LSB未満までずれうる。
    double toleranceCents;
};

// 実チップがそのF-Number/Blockで鳴らす周波数。fnumはgetFnumber()の11bit表現
// (OPLは>>1、OPLLは>>2してレジスタへ書かれる。各ドライバのupdateFreq参照)。
Played playedHz(Family fam, const ChState::Fnum& fn, double clock, double clocksPerSample)
{
    const double fs = clock / clocksPerSample;
    double steps = 0.0, reg = 0.0;
    switch (fam) {
    case Family::Opn:
        reg = fn.fnum;
        steps = reg * std::pow(2.0, fn.block - 1);
        break;
    case Family::Opl:
        reg = fn.fnum >> 1;
        steps = reg * std::pow(2.0, fn.block);
        break;
    case Family::Opll:
        reg = fn.fnum >> 2;
        steps = reg * std::pow(2.0, fn.block + 1);
        break;
    }
    const double hz = steps * fs / 1048576.0;
    const double tol = (reg > 0) ? 1200.0 * std::log2((reg + 1.0) / reg) : 0.0;
    return {hz, tol};
}

double centsFrom440(double hz) { return 1200.0 * std::log2(hz / 440.0); }

struct Case {
    uint32_t    deviceType;
    const char* name;
    int         clock;
    int         clockDivider;
    int         clockMultiplier;
    Family      family;
    double      clocksPerSample;
};

// A4(note 69)を鳴らし、ドライバが書いたF-Numberを返す
ChState::Fnum fnumForA4(ISoundDevice& dev)
{
    HwPatch patch{};
    patch.id = 1;
    uint8_t ch = dev.allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev.setNoteFine(ch, 69, 0, true);
    const ChState* cs = dev.getChState(ch);
    REQUIRE(cs != nullptr);
    return cs->lastFnum;
}

// OPMのKC/KFを、1オクターブ768ステップの通し番号へ戻す
int opmPosition(const ChState::Fnum& fn)
{
    static const std::map<int, int> kNoteIndex = {
        {0, 0}, {1, 1}, {2, 2}, {4, 3}, {5, 4}, {6, 5},
        {8, 6}, {9, 7}, {10, 8}, {12, 9}, {13, 10}, {14, 11}};
    const int code = (fn.fnum >> 6) & 0x0F;
    const int kf   = fn.fnum & 0x3F;
    auto it = kNoteIndex.find(code);
    REQUIRE(it != kNoteIndex.end());
    return fn.block * 768 + it->second * 64 + kf;
}

} // namespace

TEST_CASE("FM drivers compute A4 from the clock the port reports", "[fm][clock]")
{
    const std::vector<Case> cases = {
        {DEVICE_OPN,   "OPN",   3993600,  1, 1, Family::Opn,  72},
        {DEVICE_OPN,   "OPN",   4000000,  1, 1, Family::Opn,  72},
        {DEVICE_OPNA,  "OPNA",  8000000,  1, 1, Family::Opn,  144},
        {DEVICE_OPNA,  "OPNA",  7987200,  1, 1, Family::Opn,  144},
        {DEVICE_OPNA,  "OPNA",  7159090,  1, 1, Family::Opn,  144},
        {DEVICE_OPN2,  "OPN2",  7670453,  1, 1, Family::Opn,  144},
        {DEVICE_OPNB,  "OPNB",  8000000,  1, 1, Family::Opn,  144},
        {DEVICE_2610B, "OPNBB", 7159090,  1, 1, Family::Opn,  144},
        {DEVICE_OPL,   "OPL",   3579545,  1, 1, Family::Opl,  72},
        {DEVICE_OPL2,  "OPL2",  4000000,  1, 1, Family::Opl,  72},
        {DEVICE_Y8950, "Y8950", 3579545,  1, 1, Family::Opl,  72},
        {DEVICE_OPL2EX,"OPL2EX",3579545,  1, 1, Family::Opl,  72},
        // OPL3/OPL4のサブデバイスへは、FITOMConfig::resolveCompositeSpec()と
        // 同じ換算比を渡す(下のテストで換算比そのものを確かめる)
        {DEVICE_OPL3,   "OPL3-4OP", 14318180, 4,  1, Family::Opl, 288},
        {DEVICE_OPL3_2, "OPL3-2OP", 14318180, 4,  1, Family::Opl, 288},
        {DEVICE_OPL3,   "OPL3-4OP", 14000000, 4,  1, Family::Opl, 288},
        {DEVICE_OPL3,   "OPL4-4OP", 33868800, 19, 2, Family::Opl, 684},
        {DEVICE_OPL3_2, "OPL4-2OP", 33868800, 19, 2, Family::Opl, 684},
        {DEVICE_OPLL,   "OPLL",   3579545, 1, 1, Family::Opll, 72},
        {DEVICE_OPLL,   "OPLL",   3600000, 1, 1, Family::Opll, 72},
        {DEVICE_OPLLEX, "OPLLEX", 3579545, 1, 1, Family::Opll, 72},
        {DEVICE_VRC7,   "VRC7",   3579545, 1, 1, Family::Opll, 72},
    };

    for (const auto& c : cases) {
        INFO(c.name << " clock=" << c.clock);
        ClockPort port(c.clock);
        auto dev = DeviceFactory::create(c.deviceType, &port, 44100, nullptr, false,
                                         c.clockDivider, c.clockMultiplier);
        REQUIRE(dev != nullptr);
        dev->init();
        const ChState::Fnum fn = fnumForA4(*dev);
        const Played p = playedHz(c.family, fn, c.clock, c.clocksPerSample);
        INFO("block=" << int(fn.block) << " fnum=" << fn.fnum << " hz=" << p.hz
             << " cents=" << centsFrom440(p.hz) << " tol=" << p.toleranceCents);
        CHECK(std::fabs(centsFrom440(p.hz)) <= p.toleranceCents);
    }
}

TEST_CASE("FM rhythm parts follow the reported clock on OPL3 and OPL4", "[fm][clock][rhythm]")
{
    struct RhyCase { const char* name; int clock; int div; int mul; double clocksPerSample; };
    for (const auto& c : {RhyCase{"OPL",  3579545,  1,  1, 72},
                          RhyCase{"OPL3", 14318180, 4,  1, 288},
                          RhyCase{"OPL4", 33868800, 19, 2, 684}}) {
        INFO(c.name);
        ClockPort port(c.clock);
        auto dev = DeviceFactory::create(DEVICE_OPL_RHY, &port, 44100, nullptr, false,
                                         c.div, c.mul);
        REQUIRE(dev != nullptr);
        dev->init();
        // SD(パート3)はch7のFnumを書く、音程を持つパート
        const uint8_t ch = 3;
        dev->setNoteFine(ch, 69, 0, true);
        const ChState* cs = dev->getChState(ch);
        REQUIRE(cs != nullptr);
        const Played p = playedHz(Family::Opl, cs->lastFnum, c.clock, c.clocksPerSample);
        INFO("hz=" << p.hz);
        CHECK(std::fabs(centsFrom440(p.hz)) <= p.toleranceCents);
    }
}

TEST_CASE("FM drivers fall back to the standard clock when the port reports none",
          "[fm][clock]")
{
    struct FallbackCase { uint32_t type; int standardClock; };
    for (const auto& c : {FallbackCase{DEVICE_OPN,  3993600},
                          FallbackCase{DEVICE_OPNA, 8000000},
                          FallbackCase{DEVICE_OPL2, 3579545},
                          FallbackCase{DEVICE_OPLL, 3579545}}) {
        INFO("device=0x" << std::hex << c.type);
        ClockPort silent(0);
        ClockPort standard(c.standardClock);
        auto a = DeviceFactory::create(c.type, &silent, 44100);
        auto b = DeviceFactory::create(c.type, &standard, 44100);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        a->init();
        b->init();
        const ChState::Fnum fa = fnumForA4(*a);
        const ChState::Fnum fb = fnumForA4(*b);
        CHECK(fa.block == fb.block);
        CHECK(fa.fnum == fb.fnum);
    }
}

TEST_CASE("OPM and OPZ key codes shift by the clock ratio", "[fm][clock][opm]")
{
    for (uint32_t type : {uint32_t(DEVICE_OPM), uint32_t(DEVICE_OPZ)}) {
        INFO("device=0x" << std::hex << type);
        ClockPort standard(3579545);
        auto base = DeviceFactory::create(type, &standard, 44100);
        REQUIRE(base != nullptr);
        base->init();
        const int basePos = opmPosition(fnumForA4(*base));

        ClockPort silent(0);
        auto fallback = DeviceFactory::create(type, &silent, 44100);
        REQUIRE(fallback != nullptr);
        fallback->init();
        CHECK(opmPosition(fnumForA4(*fallback)) == basePos);

        // X68000 (4MHz): 同じKCなら高く鳴るので、その分だけ下のKC/KFを書く
        for (int clock : {4000000, 3000000}) {
            ClockPort port(clock);
            auto dev = DeviceFactory::create(type, &port, 44100);
            REQUIRE(dev != nullptr);
            dev->init();
            const int pos = opmPosition(fnumForA4(*dev));
            const int expected = static_cast<int>(
                std::lround(768.0 * std::log2(3579545.0 / clock)));
            INFO("clock=" << clock << " pos-base=" << (pos - basePos)
                 << " expected=" << expected);
            CHECK(std::abs((pos - basePos) - expected) <= 1);
        }
    }
}

TEST_CASE("OPL3 and OPL4 composites convert the chip clock for their FM sub-devices",
          "[fm][clock][config]")
{
    struct Expect { uint32_t base; int div; int mul; };
    for (const auto& e : {Expect{DEVICE_OPL3, 4, 1}, Expect{DEVICE_OPL4, 19, 2}}) {
        INFO("base=0x" << std::hex << e.base);
        std::vector<FITOMConfig::SubDeviceSpec> spec;
        REQUIRE(FITOMConfig::resolveCompositeSpec(e.base, true, spec));
        int fmParts = 0;
        for (const auto& s : spec) {
            if (s.deviceType == DEVICE_OPL4AWM) {
                CHECK(s.clockDivider == 1);
                CHECK(s.clockMultiplier == 1);
                continue;
            }
            INFO("sub=0x" << std::hex << s.deviceType);
            CHECK(s.clockDivider == e.div);
            CHECK(s.clockMultiplier == e.mul);
            ++fmParts;
        }
        CHECK(fmParts == 3); // 4OP + 2OP + RHYTHM
    }
}
