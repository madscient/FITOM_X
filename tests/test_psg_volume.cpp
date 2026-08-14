// tests/test_psg_volume.cpp
// PSG系(SSG/EPSG/DCSG/SCC)のトーン周期・音量レジスタに関する回帰テスト。
//
// これらのチップの周波数レジスタは「周期」(音程が上がるほど値が小さい)で
// あり、F-number系チップ用の基底実装 CSoundDevice::getFnumber() が行う
// 11bit F-number前提の正規化とはスケーリングが根本的に噛み合わない。
// また音量は fitom::linear2dB() の step 引数でマスクしてはならない
// (上位bitが落ちてレンジ内で音量が折り返す)。

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include "fitom/FITOMdefine.h"
#include "fitom/DeviceFactory.h"
#include <cmath>
#include <map>
#include <memory>
#include <vector>

using namespace fitom;

namespace {
class RecordingPort : public IPort {
public:
    std::map<uint16_t, uint8_t> regs;
    // DCSG はアドレスを持たず writeRaw のみでアクセスするため別に記録する。
    std::vector<uint8_t> rawWrites;

    void write(uint16_t addr, uint16_t data) override {
        regs[addr] = static_cast<uint8_t>(data);
    }
    uint8_t read(uint16_t addr) override {
        auto it = regs.find(addr);
        return it != regs.end() ? it->second : 0;
    }
    void writeRaw(uint16_t /*addr*/, uint16_t data) override {
        rawWrites.push_back(static_cast<uint8_t>(data));
    }

    // DeviceFactory がチップの実クロックを問い合わせる経路。
    int clock = 0;
    int getClock() override { return clock; }
};

// ソフトウェアエンベロープが即座に最大音量へ到達する音色
// (AR最大・DR/SR停止 → アタック完了後は減衰量0のまま保持される)。
HwPatch makeSoftEnvPatch()
{
    HwPatch p{};
    p.id  = 1;
    p.hw.ALG = 0;       // トーンのみ
    p.hwOp[0].AR  = 31;
    p.hwOp[0].DR  = 0;
    p.hwOp[0].SL  = 0;
    p.hwOp[0].SR  = 0;
    p.hwOp[0].RR  = 15;
    p.hwOp[0].TL  = 0;
    p.hwOp[0].EGT = 0;  // HW EGではなくソフトウェアエンベロープを使う
    return p;
}

double noteHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// ノートオンしてソフトウェアエンベロープがアタックし切るまでtickを回す。
void playNote(ISoundDevice& dev, uint8_t ch, uint8_t note)
{
    dev.setNoteFine(ch, note, 0, true);
    dev.noteOn(ch, 127);
    for (uint32_t t = 0; t < 100; ++t) dev.timerCallback(t);
}
} // namespace

namespace fitom {
std::unique_ptr<ISoundDevice> createCSSG(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCEPSG(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCDCSG(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCSCC(IPort* p, int sr, uint32_t deviceType);
}

// SSGのトーン周期は実機式 period = master / (16 * freq) でなければならない。
// 周期テーブル(FnumTableType::SSG)を基底のF-number用スケーリング
// (fnum >> (block+3)) に通していたため、周期が128分の1(=約7オクターブ上の
// 超音波)になり、SSGがまったく発音していないように聞こえていた。
TEST_CASE("CSSG tone period matches the AY-3-8910 formula master/(16*freq)",
          "[psg][ssg]")
{
    const int master = 3993600;
    for (int note : {48, 60, 69, 81}) {
        RecordingPort port;
        auto dev = createCSSG(&port, master);
        dev->init();

        HwPatch patch = makeSoftEnvPatch();
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        playNote(*dev, ch, static_cast<uint8_t>(note));

        int period = port.regs[static_cast<uint16_t>(ch * 2)]
                   | (port.regs[static_cast<uint16_t>(ch * 2 + 1)] << 8);
        double expected = master / (16.0 * noteHz(note));
        INFO("note=" << note << " period=" << period << " expected=" << expected);
        CHECK(std::abs(period - expected) <= 1.0);
    }
}

// PSG系のトーン周期はチップのマスタークロックから算出されるため、
// DeviceFactory は音声出力のサンプルレートではなく IPort::getClock() の
// 実クロックを渡さなければならない。さらにOPN系(YM2608/YM2610系)に内蔵
// されるSSG部はφM/4で動作するため、clockDivider=4 を適用する必要がある。
// (この2点が無いと、SSGはサンプルレート44100Hzをマスタークロックとして
//  扱い、トーン周期が2桁の値=可聴帯域外の超高音にしかならなかった)。
TEST_CASE("DeviceFactory gives PSG chips the real chip clock, divided for "
          "OPN-embedded SSG", "[psg][ssg][clock]")
{
    const int opnClock = 7159090;             // プロファイルのOPNAクロック
    const int ssgClock = opnClock / 4;        // YM2608のSSG部はφM/4

    HwPatch patch = makeSoftEnvPatch();

    SECTION("OPN内蔵SSG (clockDivider=4)") {
        RecordingPort port;
        port.clock = opnClock;
        auto dev = DeviceFactory::create(DEVICE_SSG, &port, 44100, nullptr, false, 4);
        REQUIRE(dev);
        dev->init();
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        playNote(*dev, ch, 69);

        int period = port.regs[static_cast<uint16_t>(ch * 2)]
                   | (port.regs[static_cast<uint16_t>(ch * 2 + 1)] << 8);
        double expected = ssgClock / (16.0 * noteHz(69));
        INFO("period=" << period << " expected=" << expected);
        CHECK(std::abs(period - expected) <= 1.0);
    }

    SECTION("単独SSG (分周なし)") {
        RecordingPort port;
        port.clock = ssgClock;
        auto dev = DeviceFactory::create(DEVICE_SSG, &port, 44100, nullptr, false, 1);
        REQUIRE(dev);
        dev->init();
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        playNote(*dev, ch, 69);

        int period = port.regs[static_cast<uint16_t>(ch * 2)]
                   | (port.regs[static_cast<uint16_t>(ch * 2 + 1)] << 8);
        double expected = ssgClock / (16.0 * noteHz(69));
        INFO("period=" << period << " expected=" << expected);
        CHECK(std::abs(period - expected) <= 1.0);
    }

    // クロックを報告しないポート(getClock()=0)では従来通りsampleRateへ
    // フォールバックする。
    SECTION("getClock()=0 ならsampleRateへフォールバック") {
        RecordingPort port;
        port.clock = 0;
        auto dev = DeviceFactory::create(DEVICE_SSG, &port, ssgClock, nullptr, false, 1);
        REQUIRE(dev);
        dev->init();
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        playNote(*dev, ch, 69);

        int period = port.regs[static_cast<uint16_t>(ch * 2)]
                   | (port.regs[static_cast<uint16_t>(ch * 2 + 1)] << 8);
        double expected = ssgClock / (16.0 * noteHz(69));
        INFO("period=" << period << " expected=" << expected);
        CHECK(std::abs(period - expected) <= 1.0);
    }
}

// SCCのトーン周期は period = master / (32 * freq)。
TEST_CASE("CSCC tone period matches master/(32*freq)", "[psg][scc]")
{
    const int master = 3579545;
    for (int note : {48, 60, 69, 81}) {
        RecordingPort port;
        auto dev = createCSCC(&port, master, DEVICE_SCC);
        dev->init();

        HwPatch patch = makeSoftEnvPatch();
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        playNote(*dev, ch, static_cast<uint8_t>(note));

        int period = port.regs[static_cast<uint16_t>(0xA0 + ch * 2)]
                   | (port.regs[static_cast<uint16_t>(0xA1 + ch * 2)] << 8);
        double expected = master / (32.0 * noteHz(note));
        INFO("note=" << note << " period=" << period << " expected=" << expected);
        CHECK(std::abs(period - expected) <= 1.0);
    }
}

// DCSG(SN76489)のトーン周期は period = master / (32 * freq)。
TEST_CASE("CDCSG tone period matches master/(32*freq)", "[psg][dcsg]")
{
    const int master = 3579545;
    for (int note : {48, 60, 69, 81}) {
        RecordingPort port;
        auto dev = createCDCSG(&port, master);
        dev->init();

        HwPatch patch = makeSoftEnvPatch();
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        port.rawWrites.clear();
        playNote(*dev, ch, static_cast<uint8_t>(note));

        int period = -1;
        for (size_t i = 0; i + 1 < port.rawWrites.size(); ++i) {
            // 0x80|(ch*32) = トーン周期ラッチ、続く1バイトが上位6bit
            if (port.rawWrites[i] == static_cast<uint8_t>(0x80 | (ch * 32))
                || (port.rawWrites[i] & 0xF0) == static_cast<uint8_t>(0x80 | (ch * 32))) {
                period = (port.rawWrites[i] & 0xF) | (port.rawWrites[i + 1] << 4);
                break;
            }
        }
        double expected = master / (32.0 * noteHz(note));
        INFO("note=" << note << " period=" << period << " expected=" << expected);
        CHECK(std::abs(period - expected) <= 1.0);
    }
}

// 音量レジスタは、vol/exp/velocityが全て最大かつTL=0のとき、チップの
// 最大音量値に到達しなければならない。linear2dB()にSTEP300DB(0x1F)を
// 渡してラウドネスの上位2bitを落としていたため、最大でも15段中7までしか
// 上がらず、ラウドネスが32の倍数付近では完全に無音になっていた。
TEST_CASE("PSG family volume registers reach full scale at max loudness",
          "[psg][volume]")
{
    HwPatch patch = makeSoftEnvPatch();

    SECTION("CSSG (0=無音, 15=最大)") {
        RecordingPort port;
        auto dev = createCSSG(&port, 3993600);
        dev->init();
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        playNote(*dev, ch, 69);
        CHECK((port.regs[static_cast<uint16_t>(0x08 + ch)] & 0x0F) == 15);
    }

    SECTION("CSCC (0=無音, 15=最大)") {
        RecordingPort port;
        auto dev = createCSCC(&port, 3579545, DEVICE_SCC);
        dev->init();
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        playNote(*dev, ch, 69);
        CHECK((port.regs[static_cast<uint16_t>(0xA8 + ch)] & 0x0F) == 15);
    }

    SECTION("CEPSG (5bit、0=無音, 31=最大)") {
        RecordingPort port;
        auto dev = createCEPSG(&port, 3993600);
        dev->init();
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        playNote(*dev, ch, 69);
        CHECK((port.regs[static_cast<uint16_t>(0x08 + ch)] & 0x1F) == 31);
    }

    // SN76489 は減衰量表現 (0=最大音量, 15=無音) で、他のPSG系とは極性が逆。
    SECTION("CDCSG (減衰量: 0=最大音量, 15=無音)") {
        RecordingPort port;
        auto dev = createCDCSG(&port, 3579545);
        dev->init();
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        port.rawWrites.clear();
        playNote(*dev, ch, 69);

        int att = -1;
        for (uint8_t w : port.rawWrites) {
            if ((w & 0xF0) == static_cast<uint8_t>(0x90 | (ch * 32))) att = w & 0xF;
        }
        INFO("attenuation=" << att);
        CHECK(att == 0);
    }
}

// ラウドネスがどの値でも、音量レジスタが飛び飛びに0(無音)へ落ちてはならない
// (STEP300DBマスクは0-127を0-31へ折り返し、32の倍数付近のラウドネスを
//  すべて無音にしていた)。
TEST_CASE("CSSG volume decreases monotonically with MIDI volume, never "
          "silently wrapping to zero", "[psg][volume]")
{
    RecordingPort port;
    auto dev = createCSSG(&port, 3993600);
    dev->init();

    HwPatch patch = makeSoftEnvPatch();
    uint8_t ch = dev->allocCh(nullptr, &patch, 127);
    REQUIRE(ch != 0xFF);
    playNote(*dev, ch, 69);

    int prev = 16;
    for (int vol = 127; vol >= 32; vol -= 1) {
        dev->setVolume(ch, static_cast<uint8_t>(vol), true);
        int v = port.regs[static_cast<uint16_t>(0x08 + ch)] & 0x0F;
        INFO("CC#7=" << vol << " reg=" << v << " prev=" << prev);
        CHECK(v <= prev);   // 単調非増加
        CHECK(v > 0);       // この音量域で無音になってはならない
        prev = v;
    }
}
