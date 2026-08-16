// tests/test_ssgs.cpp
// SSGS (YMZ705) チップドライバ CSSGS / CSSGSAdPcm の回帰テスト。
//
// 検証の中心は、このチップに固有で壊れやすい点:
//   1. SSG互換部が2ユニット構成であること (ch0-2 → $00-$12、ch3-5 → $20-$32)。
//      レジスタ空間が0x20ごとに繰り返すため、ユニット索引を1箇所でも
//      落とすとch3-5の書き込みがch0-2を上書きして壊れる。
//   2. マスタークロック (4.096MHz / 6.144MHz) から SSGブロックの
//      2.048MHz を導くこと。分周比がクロック値に依存する。
//   3. パンポットのリセット値0が「中央」ではなく左端であること。
//      init()で中央を書いておかないと全chが左に張り付く。
//   4. ADPCM部が「ボイス番号だけを書く」方式であること
//      (開始/終了アドレスレジスタが存在しない)。

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include "fitom/PatchData.h"
#include "fitom/PcmBankData.h"
#include "fitom/FITOMdefine.h"
#include "fitom/DeviceFactory.h"
#include "fitom/Config.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

using namespace fitom;

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
    int clock = 0;
    int getClock() override { return clock; }

    bool wrote(uint16_t addr) const { return regs.count(addr) > 0; }
    uint8_t at(uint16_t addr) const {
        auto it = regs.find(addr);
        return it != regs.end() ? it->second : 0;
    }
};

// YMZ705 の 2 択のマスタークロックと、YMZ732 の唯一のマスタークロック。
// いずれも SSG ブロックは 2.048MHz で動く。
constexpr int kMaster4096  = 4096000;
constexpr int kMaster6144  = 6144000;
constexpr int kMaster12288 = 12288000;  // YMZ732 (SSGS2)
constexpr int kMaster16384 = 16384000;  // YMZ771 (SSGS3)
constexpr int kSsgBlock    = 2048000;

double noteHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// ソフトウェアエンベロープが即座に最大音量へ到達する音色
HwPatch makeTonePatch()
{
    HwPatch p{};
    p.id = 1;
    p.hw.ALG      = 0;   // トーンのみ
    p.hwOp[0].AR  = 31;
    p.hwOp[0].DR  = 0;
    p.hwOp[0].SL  = 0;
    p.hwOp[0].SR  = 0;
    p.hwOp[0].RR  = 15;
    p.hwOp[0].TL  = 0;
    p.hwOp[0].EGT = 0;
    return p;
}

HwPatch makeNoisePatch(uint8_t nfq)
{
    HwPatch p = makeTonePatch();
    p.hw.ALG = 1;        // ノイズのみ
    p.hw.NFQ = nfq;
    return p;
}

std::unique_ptr<ISoundDevice> makeSsgs(RecordingPort& port, int masterClock,
                                       uint32_t deviceType = DEVICE_SSGS)
{
    port.clock = masterClock;
    auto dev = DeviceFactory::create(deviceType, &port, masterClock);
    REQUIRE(dev != nullptr);
    dev->init();
    return dev;
}

// PSG系はソフトウェアエンベロープが無音から立ち上がるため、noteOn直後は
// 音量レジスタが0のまま (シャドウの初期値と同じで書き込みも起きない)。
// 音量レジスタを検証するテストは、アタックが終わるまでtickを回してから見る。
void runEnvelope(ISoundDevice& dev, int ticks = 64)
{
    for (int i = 0; i < ticks; ++i) dev.timerCallback(static_cast<uint32_t>(i));
}

std::unique_ptr<ISoundDevice> makeSsgsAdpcm(RecordingPort& port)
{
    port.clock = kMaster4096;
    auto dev = DeviceFactory::create(DEVICE_SSGS_ADPCM, &port, kMaster4096);
    REQUIRE(dev != nullptr);
    dev->init();
    return dev;
}

// ユニット内のトーン周期 (Fine 8bit + Coarse 4bit) を復元する
int decodePeriod(const RecordingPort& port, uint8_t ch)
{
    const uint16_t base = static_cast<uint16_t>((ch / 3) * 0x20 + (ch % 3) * 2);
    return ((port.at(static_cast<uint16_t>(base + 1)) & 0x0F) << 8) | port.at(base);
}

// SSGS3 (YMZ771) のトーン周期は $10+ch*2 (Fine) / +1 (Coarse)
int decodePeriodSsgs3(const RecordingPort& port, uint8_t ch)
{
    const uint16_t base = static_cast<uint16_t>(0x10 + ch * 2);
    return ((port.at(static_cast<uint16_t>(base + 1)) & 0x0F) << 8) | port.at(base);
}

SampleZonePatch makeSamplePatch(uint16_t waveIndex)
{
    SampleZonePatch p;
    p.id = 0;
    SampleZone z;
    z.waveIndex = waveIndex;
    p.zones.push_back(z);
    return p;
}

} // namespace

// ================================================================
//  CSSGS — SSG 互換部 (2ユニット × 3ch)
// ================================================================

TEST_CASE("CSSGS: 6ch構成で、SSG-1/SSG-2のレジスタが0x20ずつ離れて配置される", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgs(port, kMaster4096);
    REQUIRE(dev->getChCount() == 6);

    HwPatch p = makeTonePatch();
    for (uint8_t ch = 0; ch < 6; ++ch) {
        dev->assignCh(ch, nullptr, &p, 100);
        dev->setNoteFine(ch, 69, 0);
        dev->noteOn(ch, 100);
    }
    runEnvelope(*dev);

    // トーン周期: ch0-2 → $00-$05、ch3-5 → $20-$25
    for (uint8_t ch = 0; ch < 6; ++ch) {
        const uint16_t base = static_cast<uint16_t>((ch / 3) * 0x20 + (ch % 3) * 2);
        CHECK(port.wrote(base));
        CHECK(port.wrote(static_cast<uint16_t>(base + 1)));
    }
    // 音量: ch0-2 → $08-$0A、ch3-5 → $28-$2A
    for (uint8_t ch = 0; ch < 6; ++ch) {
        CHECK(port.wrote(static_cast<uint16_t>((ch / 3) * 0x20 + 0x08 + (ch % 3))));
    }
    // ユニットをまたいだレジスタの衝突が無いこと
    // (SSG-2 のミキサーは $27 であり、$07 を上書きしない)
    CHECK(port.wrote(0x07));
    CHECK(port.wrote(0x27));
}

TEST_CASE("CSSGS: 同じ音程なら全chが同じトーン周期になる (ユニット間で計算が揃う)", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgs(port, kMaster4096);

    HwPatch p = makeTonePatch();
    for (uint8_t ch = 0; ch < 6; ++ch) {
        dev->assignCh(ch, nullptr, &p, 100);
        dev->setNoteFine(ch, 69, 0);
        dev->noteOn(ch, 100);
    }

    const int ref = decodePeriod(port, 0);
    for (uint8_t ch = 1; ch < 6; ++ch) {
        CHECK(decodePeriod(port, ch) == ref);
    }
}

TEST_CASE("CSSGS: マスタークロックからSSGブロックの2.048MHzを導く", "[ssgs]")
{
    // 実機の SSG ブロックはマスタークロックが 4.096MHz でも 6.144MHz でも
    // 2.048MHz で動く。周期は master/(16*f) なので、どちらでも同じ値になる。
    const int expected = static_cast<int>(std::lround(kSsgBlock / (16.0 * noteHz(69))));

    for (int master : {kMaster4096, kMaster6144}) {
        RecordingPort port;
        auto dev = makeSsgs(port, master);
        HwPatch p = makeTonePatch();
        dev->assignCh(0, nullptr, &p, 100);
        dev->setNoteFine(0, 69, 0);
        dev->noteOn(0, 100);

        // 周期テーブルの量子化があるため厳密一致ではなく1ステップの誤差を許容
        CHECK(std::abs(decodePeriod(port, 0) - expected) <= 1);
    }
}

TEST_CASE("CSSGS2: YMZ732は12.288MHzを1/6してSSGブロックの2.048MHzを得る", "[ssgs]")
{
    // YMZ705 と違いマスタークロックは1択で、S6Mピンの役割自体が
    // CPUインターフェイスモード選択へ変わっている。
    const int expected = static_cast<int>(std::lround(kSsgBlock / (16.0 * noteHz(69))));

    RecordingPort port;
    auto dev = makeSsgs(port, kMaster12288, DEVICE_SSGS2);
    HwPatch p = makeTonePatch();
    dev->assignCh(0, nullptr, &p, 100);
    dev->setNoteFine(0, 69, 0);
    dev->noteOn(0, 100);

    CHECK(std::abs(decodePeriod(port, 0) - expected) <= 1);
    CHECK(dev->getDescriptor() == "SSGS2 (YMZ732) SSG 6ch");
}

TEST_CASE("CSSGS2: レジスタ配置はSSGSと完全に同一", "[ssgs]")
{
    // YMZ732 データシート注記「YMZ705(SSGS)とレジスタコンパチブルです」。
    // 同じ音程・同じパンで両チップを駆動し、書き込まれたレジスタが
    // アドレス・値ともに1バイトも違わないことを確認する。
    auto drive = [](RecordingPort& port, uint32_t deviceType, int masterClock) {
        auto dev = makeSsgs(port, masterClock, deviceType);
        HwPatch tone  = makeTonePatch();
        HwPatch noise = makeNoisePatch(0x15);
        for (uint8_t ch = 0; ch < 5; ++ch) {
            dev->assignCh(ch, nullptr, &tone, 100);
            dev->setNoteFine(ch, static_cast<uint8_t>(60 + ch), 0);
            dev->noteOn(ch, 100);
            dev->setPanpot(ch, static_cast<int8_t>(-64 + ch * 25));
        }
        dev->assignCh(5, nullptr, &noise, 100);
        dev->noteOn(5, 100);
        runEnvelope(*dev);
    };

    RecordingPort ssgs, ssgs2;
    drive(ssgs,  DEVICE_SSGS,  kMaster4096);
    drive(ssgs2, DEVICE_SSGS2, kMaster12288);

    CHECK(ssgs.regs == ssgs2.regs);
}

// ================================================================
//  CSSGS3 — YMZ771 (レジスタ配置が機能ごとにまとめ直されている)
// ================================================================

TEST_CASE("CSSGS3: レジスタが機能ごとに $10-$32 へまとめて配置される", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgs(port, kMaster16384, DEVICE_SSGS3);
    REQUIRE(dev->getChCount() == 6);
    CHECK(dev->getDescriptor() == "SSGS3 (YMZ771) SSG 6ch");

    HwPatch tone = makeTonePatch();
    for (uint8_t ch = 0; ch < 6; ++ch) {
        dev->assignCh(ch, nullptr, &tone, 100);
        dev->setNoteFine(ch, 69, 0);
        dev->noteOn(ch, 100);
    }
    runEnvelope(*dev);

    for (uint8_t ch = 0; ch < 6; ++ch) {
        // トーン周期 TP1A..TP2C: $10-$1B (2バイトずつ連続)
        CHECK(port.wrote(static_cast<uint16_t>(0x10 + ch * 2)));
        CHECK(port.wrote(static_cast<uint16_t>(0x10 + ch * 2 + 1)));
        // 音量: $20-$25
        CHECK(port.wrote(static_cast<uint16_t>(0x20 + ch)));
        // パンポット: $2C-$31
        CHECK(port.wrote(static_cast<uint16_t>(0x2C + ch)));
    }
    // ミックスはユニットごとに $1E / $1F
    CHECK(port.wrote(0x1E));
    CHECK(port.wrote(0x1F));
    // SSGS/SSGS2 のアドレス ($00-$0D) には一切書かない
    for (uint16_t r = 0x00; r < 0x10; ++r) CHECK_FALSE(port.wrote(r));
}

TEST_CASE("CSSGS3: 16.384MHzを1/8してSSGブロックの2.048MHzを得る", "[ssgs]")
{
    const int expected = static_cast<int>(std::lround(kSsgBlock / (16.0 * noteHz(69))));

    RecordingPort port;
    auto dev = makeSsgs(port, kMaster16384, DEVICE_SSGS3);
    HwPatch p = makeTonePatch();
    dev->assignCh(0, nullptr, &p, 100);
    dev->setNoteFine(0, 69, 0);
    dev->noteOn(0, 100);

    CHECK(std::abs(decodePeriodSsgs3(port, 0) - expected) <= 1);
}

TEST_CASE("CSSGS3: init() が両ユニットを消音し、パンポット中央とトータル音量を書く", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgs(port, kMaster16384, DEVICE_SSGS3);

    CHECK(port.at(0x1E) == 0x3F);
    CHECK(port.at(0x1F) == 0x3F);
    // パンポットは5bitなので中央は16 (4bitの8ではない)
    for (uint8_t ch = 0; ch < 6; ++ch) {
        CHECK(port.wrote(static_cast<uint16_t>(0x2C + ch)));
        CHECK(port.at(static_cast<uint16_t>(0x2C + ch)) == 16);
    }
    // SSGトータルボリューム($32)のリセット値0は完全無音なので必ず書く
    CHECK(port.wrote(0x32));
    CHECK(port.at(0x32) == 128);
}

TEST_CASE("CSSGS3: パンポットは5bit (0=左端 / 16=中央 / 31=右端)", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgs(port, kMaster16384, DEVICE_SSGS3);

    HwPatch p = makeTonePatch();
    for (uint8_t ch = 0; ch < 3; ++ch) dev->assignCh(ch, nullptr, &p, 100);

    dev->setPanpot(0, -64);
    dev->setPanpot(1,   0);
    dev->setPanpot(2,  63);
    CHECK(port.at(0x2C) == 0);
    CHECK(port.at(0x2D) == 16);
    CHECK(port.at(0x2E) == 31);
}

TEST_CASE("CSSGS3: ノイズ周波数・HWエンベロープもユニットごとに分かれる", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgs(port, kMaster16384, DEVICE_SSGS3);

    // ノイズ音色は各ユニットの最終ch (ch2 / ch5) へ割り当てられる
    HwPatch n1 = makeNoisePatch(0x15);
    dev->assignCh(2, nullptr, &n1, 100);
    CHECK(port.at(0x1C) == 0x15);     // NP1
    CHECK_FALSE(port.wrote(0x1D));

    HwPatch n2 = makeNoisePatch(0x0A);
    dev->assignCh(5, nullptr, &n2, 100);
    CHECK(port.at(0x1D) == 0x0A);     // NP2
    CHECK(port.at(0x1C) == 0x15);

    // HWエンベロープ: EP2 = $28(Fine)/$29(Coarse)、形状 = $2B
    HwPatch e = makeTonePatch();
    e.hwOp[0].EGT = 0x08 | 0x0E;
    e.ext.HWEP    = 0x1234;
    dev->assignCh(4, nullptr, &e, 100);
    CHECK(port.at(0x28) == 0x34);
    CHECK(port.at(0x29) == 0x12);
    CHECK(port.at(0x2B) == 0x0E);
    CHECK_FALSE(port.wrote(0x26));    // EP1側は触らない
    CHECK_FALSE(port.wrote(0x27));
    CHECK_FALSE(port.wrote(0x2A));
    CHECK((port.at(0x24) & 0x10) != 0); // 音量レジスタ(ch4=$24)のHW EG使用ビット
}

TEST_CASE("CSSGS3: 音程・音量はSSGSと同じ値になる (配置だけが違う)", "[ssgs]")
{
    // 機能はSSGS/SSGS2と同一。同じ音程・同じベロシティで駆動したとき、
    // 書かれる「値」はアドレスが違うだけで一致するはず
    // (パンポットだけは分解能が4bit/5bitで異なるため対象外)。
    auto drive = [](RecordingPort& port, uint32_t deviceType, int masterClock) {
        auto dev = makeSsgs(port, masterClock, deviceType);
        HwPatch tone = makeTonePatch();
        for (uint8_t ch = 0; ch < 6; ++ch) {
            dev->assignCh(ch, nullptr, &tone, 100);
            dev->setNoteFine(ch, static_cast<uint8_t>(60 + ch), 0);
            dev->noteOn(ch, 100);
        }
        runEnvelope(*dev);
    };

    RecordingPort ssgs, ssgs3;
    drive(ssgs,  DEVICE_SSGS,  kMaster4096);
    drive(ssgs3, DEVICE_SSGS3, kMaster16384);

    for (uint8_t ch = 0; ch < 6; ++ch) {
        CHECK(decodePeriod(ssgs, ch) == decodePeriodSsgs3(ssgs3, ch));
        const uint16_t volSsgs  = static_cast<uint16_t>((ch / 3) * 0x20 + 0x08 + (ch % 3));
        const uint16_t volSsgs3 = static_cast<uint16_t>(0x20 + ch);
        CHECK(ssgs.at(volSsgs) == ssgs3.at(volSsgs3));
    }
}

TEST_CASE("CSSGS: init() が両ユニットを消音し、パンポットを中央にする", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgs(port, kMaster4096);

    // ミキサー: トーン/ノイズとも全ch無効 (Active Low)
    CHECK(port.at(0x07) == 0x3F);
    CHECK(port.at(0x27) == 0x3F);

    // パンポットのリセット値0は左端なので、中央(8)を明示的に書いてある
    for (uint8_t ch = 0; ch < 6; ++ch) {
        const uint16_t reg = static_cast<uint16_t>((ch / 3) * 0x20 + 0x10 + (ch % 3));
        CHECK(port.wrote(reg));
        CHECK(port.at(reg) == 8);
    }
}

TEST_CASE("CSSGS: パンポットが $10-$12 / $30-$32 へ4bitで書かれる", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgs(port, kMaster4096);

    HwPatch p = makeTonePatch();
    for (uint8_t ch = 0; ch < 6; ++ch) dev->assignCh(ch, nullptr, &p, 100);

    dev->setPanpot(0, -64);   // 左端
    dev->setPanpot(1,   0);   // 中央
    dev->setPanpot(2,  63);   // 右端
    dev->setPanpot(3, -64);
    dev->setPanpot(4,   0);
    dev->setPanpot(5,  63);

    CHECK(port.at(0x10) == 0);
    CHECK(port.at(0x11) == 8);
    CHECK(port.at(0x12) == 15);
    CHECK(port.at(0x30) == 0);
    CHECK(port.at(0x31) == 8);
    CHECK(port.at(0x32) == 15);
}

TEST_CASE("CSSGS: ノイズ周波数がそのchのユニットのレジスタへ書かれる", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgs(port, kMaster4096);

    HwPatch p = makeNoisePatch(0x15);
    // ノイズ音色は各ユニットの最終ch (ch2 / ch5) へ割り当てられる
    dev->assignCh(2, nullptr, &p, 100);
    CHECK(port.at(0x06) == 0x15);
    CHECK_FALSE(port.wrote(0x26));

    HwPatch p2 = makeNoisePatch(0x0A);
    dev->assignCh(5, nullptr, &p2, 100);
    CHECK(port.at(0x26) == 0x0A);
    CHECK(port.at(0x06) == 0x15);   // SSG-1側は影響を受けない
}

TEST_CASE("CSSGS: ノイズ音色は各ユニットの最終chへ割り当てられる", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgs(port, kMaster4096);
    HwPatch p = makeNoisePatch(0x10);

    // 空きがあるうちは ch2 (SSG-1 の最終ch)
    CHECK(dev->queryCh(nullptr, &p, 1) == 2);

    // ch2 を埋めると ch5 (SSG-2 の最終ch) へ回る
    dev->assignCh(2, nullptr, &p, 100);
    dev->noteOn(2, 100);
    CHECK(dev->queryCh(nullptr, &p, 1) == 5);

    // 両方埋まったら割り当て不能
    dev->assignCh(5, nullptr, &p, 100);
    dev->noteOn(5, 100);
    CHECK(dev->queryCh(nullptr, &p, 1) == 0xFF);
}

TEST_CASE("CSSGS: ミキサービットがユニット内chだけに作用する", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgs(port, kMaster4096);

    HwPatch p = makeTonePatch();   // ALG=0: トーンのみ (ノイズ無効ビットを立てる)
    dev->assignCh(3, nullptr, &p, 100);   // SSG-2 の ch0

    // SSG-2 のミキサーは ch0 のトーンだけ有効になる (bit0=0、他は無効のまま)
    CHECK(port.at(0x27) == 0x3E);
    // SSG-1 のミキサーは init() の値のまま
    CHECK(port.at(0x07) == 0x3F);
}

TEST_CASE("CSSGS: HWエンベロープのレジスタもユニットごとに分かれる", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgs(port, kMaster4096);

    HwPatch p = makeTonePatch();
    p.hwOp[0].EGT = 0x08 | 0x0E;   // bit3=HW EG使用、下位4bit=シェイプ
    p.ext.HWEP    = 0x1234;

    dev->assignCh(4, nullptr, &p, 100);   // SSG-2 の ch1

    CHECK(port.at(0x2B) == 0x34);   // Envelope Fine
    CHECK(port.at(0x2C) == 0x12);   // Envelope Coarse
    CHECK(port.at(0x2D) == 0x0E);   // Envelope Shape
    CHECK_FALSE(port.wrote(0x0B));
    CHECK_FALSE(port.wrote(0x0C));
    CHECK_FALSE(port.wrote(0x0D));
    // 音量レジスタ側の HW EG 使用ビット (bit4)
    CHECK((port.at(0x29) & 0x10) != 0);
}

TEST_CASE("CSSGS: 単体SSG(CSSG)のレジスタ配置は従来どおり変わらない", "[ssgs]")
{
    // ユニット索引の導入で CSSG (maxChs=3) のアドレスが1バイトも
    // ずれていないことを確認する回帰テスト。
    RecordingPort port;
    port.clock = kSsgBlock;
    auto dev = DeviceFactory::create(DEVICE_SSG, &port, kSsgBlock);
    REQUIRE(dev != nullptr);
    dev->init();
    CHECK(port.at(0x07) == 0x3F);

    HwPatch p = makeTonePatch();
    for (uint8_t ch = 0; ch < 3; ++ch) {
        dev->assignCh(ch, nullptr, &p, 100);
        dev->setNoteFine(ch, 69, 0);
        dev->noteOn(ch, 100);
    }
    runEnvelope(*dev);
    for (uint8_t ch = 0; ch < 3; ++ch) {
        CHECK(port.wrote(static_cast<uint16_t>(ch * 2)));
        CHECK(port.wrote(static_cast<uint16_t>(ch * 2 + 1)));
        CHECK(port.wrote(static_cast<uint16_t>(0x08 + ch)));
    }
    // SSG-2 側のアドレスには一切書かない
    for (uint16_t r = 0x20; r < 0x40; ++r) CHECK_FALSE(port.wrote(r));
}

// ================================================================
//  CSSGSAdPcm — ADPCM 部 (8ch)
// ================================================================

TEST_CASE("CSSGSAdPcm: 8ch構成でレジスタが $40 から16byteおきに並ぶ", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgsAdpcm(port);
    REQUIRE(dev->getChCount() == 8);

    // init() が全chのKON/LOOPをクリアし、パンポットを中央にする
    for (uint8_t ch = 0; ch < 8; ++ch) {
        const uint16_t base = static_cast<uint16_t>(0x40 + ch * 0x10);
        CHECK(port.wrote(static_cast<uint16_t>(base + 3)));
        CHECK(port.at(static_cast<uint16_t>(base + 3)) == 0x00);
        CHECK(port.at(static_cast<uint16_t>(base + 2)) == 8);
    }
}

TEST_CASE("CSSGSAdPcm: ボイス番号とサンプリング周波数コードを1つのレジスタへ書く", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgsAdpcm(port);

    PcmBankRegistry reg;
    PcmBank& bank = reg.getOrCreate(0);
    bank.sampleRate = 16000;          // S1S0 = 10
    PcmEntry e;
    e.entryNo    = 5;
    e.startOffset = 0x1000;
    e.paddedSize = 0x400;
    bank.setEntry(5, e);
    dev->setPcmRegistry(&reg, 0);
    dev->initPcmData();

    SampleZonePatch sp = makeSamplePatch(5);
    dev->assignCh(2, nullptr, nullptr, 100, nullptr, &sp);

    // $60 = ch2 のボイス指定レジスタ。S1S0=10 (16kHz) + ボイス番号5
    CHECK(port.at(0x60) == static_cast<uint8_t>((2 << 6) | 5));
    // 開始/終了アドレスレジスタは存在しないため、1chあたり4レジスタ
    // ($60-$63) しか使わない。残り12バイトには一切書かない。
    for (uint16_t r = 0x64; r <= 0x6F; ++r) CHECK_FALSE(port.wrote(r));
}

TEST_CASE("CSSGSAdPcm: サンプリング周波数コードは 4k/8k/16k/32k に対応する", "[ssgs]")
{
    const std::pair<uint32_t, uint8_t> cases[] = {
        {4000, 0}, {8000, 1}, {16000, 2}, {32000, 3},
    };
    for (const auto& [rate, code] : cases) {
        RecordingPort port;
        auto dev = makeSsgsAdpcm(port);

        PcmBankRegistry reg;
        PcmBank& bank = reg.getOrCreate(0);
        bank.sampleRate = rate;
        PcmEntry e;
        e.entryNo = 0;
        e.paddedSize = 0x100;
        bank.setEntry(0, e);
        dev->setPcmRegistry(&reg, 0);
        dev->initPcmData();

        SampleZonePatch sp = makeSamplePatch(0);
        dev->assignCh(0, nullptr, nullptr, 100, nullptr, &sp);
        CHECK(port.at(0x40) == static_cast<uint8_t>(code << 6));
    }
}

TEST_CASE("CSSGSAdPcm: sample_rate未指定・非対応値は最高レート(32kHz)へ丸める", "[ssgs]")
{
    for (uint32_t rate : {uint32_t{0}, uint32_t{24000}}) {
        RecordingPort port;
        auto dev = makeSsgsAdpcm(port);

        PcmBankRegistry reg;
        PcmBank& bank = reg.getOrCreate(0);
        bank.sampleRate = rate;
        PcmEntry e;
        e.entryNo = 0;
        e.paddedSize = 0x100;
        bank.setEntry(0, e);
        dev->setPcmRegistry(&reg, 0);
        dev->initPcmData();

        SampleZonePatch sp = makeSamplePatch(0);
        dev->assignCh(0, nullptr, nullptr, 100, nullptr, &sp);
        CHECK(port.at(0x40) == static_cast<uint8_t>(3 << 6));
    }
}

TEST_CASE("CSSGSAdPcm: ボイス番号は6bit、範囲外なら発音しない", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgsAdpcm(port);

    PcmBankRegistry reg;
    PcmBank& bank = reg.getOrCreate(0);
    bank.sampleRate = 32000;
    dev->setPcmRegistry(&reg, 0);
    dev->initPcmData();

    // waveIndex=64 はマスクすると0 (全く別のサンプル) になってしまうため、
    // 丸めずに書き込み自体を見送る。
    const uint8_t before = port.at(0x40);
    SampleZonePatch sp = makeSamplePatch(64);
    dev->assignCh(0, nullptr, nullptr, 100, nullptr, &sp);
    CHECK(port.at(0x40) == before);
}

TEST_CASE("CSSGSAdPcm: KONはbit1、LOOPは常に0", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgsAdpcm(port);

    PcmBankRegistry reg;
    PcmBank& bank = reg.getOrCreate(0);
    bank.sampleRate = 32000;
    PcmEntry e;
    e.entryNo = 1;
    e.paddedSize = 0x100;
    bank.setEntry(1, e);
    dev->setPcmRegistry(&reg, 0);
    dev->initPcmData();

    SampleZonePatch sp = makeSamplePatch(1);
    dev->assignCh(3, nullptr, nullptr, 100, nullptr, &sp);
    dev->noteOn(3, 100);
    CHECK(port.at(0x73) == 0x02);   // KON=1 / LOOP=0

    dev->noteOff(3);
    CHECK(port.at(0x73) == 0x00);
}

TEST_CASE("CSSGSAdPcm: パンポットが $42+ch*$10 へ4bitで書かれる", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgsAdpcm(port);

    PcmBankRegistry reg;
    PcmBank& bank = reg.getOrCreate(0);
    bank.sampleRate = 32000;
    PcmEntry e;
    e.entryNo = 0;
    e.paddedSize = 0x100;
    bank.setEntry(0, e);
    dev->setPcmRegistry(&reg, 0);
    dev->initPcmData();

    SampleZonePatch sp = makeSamplePatch(0);
    for (uint8_t ch = 0; ch < 3; ++ch)
        dev->assignCh(ch, nullptr, nullptr, 100, nullptr, &sp);

    dev->setPanpot(0, -64);
    dev->setPanpot(1,   0);
    dev->setPanpot(2,  63);
    CHECK(port.at(0x42) == 0);
    CHECK(port.at(0x52) == 8);
    CHECK(port.at(0x62) == 15);
}

TEST_CASE("CSSGSAdPcm: 音量は4bit(0=無音/15=最大)で、CC#7に単調に追従する", "[ssgs]")
{
    RecordingPort port;
    auto dev = makeSsgsAdpcm(port);

    PcmBankRegistry reg;
    PcmBank& bank = reg.getOrCreate(0);
    bank.sampleRate = 32000;
    PcmEntry e;
    e.entryNo = 0;
    e.paddedSize = 0x100;
    bank.setEntry(0, e);
    dev->setPcmRegistry(&reg, 0);
    dev->initPcmData();

    SampleZonePatch sp = makeSamplePatch(0);
    dev->assignCh(0, nullptr, nullptr, 100, nullptr, &sp);

    dev->setVolume(0, 127);
    const uint8_t full = port.at(0x41);
    CHECK(full == 0x0F);   // 最大音量でフルスケール

    int prev = 0x10;
    for (uint8_t vol : {uint8_t{127}, uint8_t{96}, uint8_t{64}, uint8_t{32}, uint8_t{0}}) {
        dev->setVolume(0, vol);
        const int v = port.at(0x41);
        CHECK(v <= 0x0F);
        CHECK(v <= prev);
        prev = v;
    }
    CHECK(prev == 0);      // CC#7=0 で無音
}

// ================================================================
//  構成 (composite) と分類
// ================================================================

TEST_CASE("SSGS/SSGS2: SSG部とADPCM部の2サブデバイスへcomposite展開される", "[ssgs]")
{
    for (uint32_t base : {uint32_t{DEVICE_SSGS}, uint32_t{DEVICE_SSGS2}}) {
        std::vector<FITOMConfig::SubDeviceSpec> spec;
        REQUIRE(FITOMConfig::resolveCompositeSpec(base, false, spec));
        REQUIRE(spec.size() == 2);
        CHECK(spec[0].deviceType == base);
        // ADPCM部はSSGS/SSGS2で制御が完全に同一のためdeviceTypeを共有する
        // (PCMバンク/メモリイメージも1つで足りる)
        CHECK(spec[1].deviceType == DEVICE_SSGS_ADPCM);
        // 両サブデバイスとも同じ8bitレジスタ空間を共有するため extraPort は使わない
        CHECK_FALSE(spec[0].usesExtraPort);
        CHECK_FALSE(spec[1].usesExtraPort);
    }
}

TEST_CASE("SSGS3: AMM部が非対応のためcomposite展開されない", "[ssgs]")
{
    // ADPCMの代わりに搭載されたAMM(MPEG Audio系コーデック)部は当面非対応で、
    // 生成すべきサブデバイスがSSG部しかないため単一デバイスのままにする。
    std::vector<FITOMConfig::SubDeviceSpec> spec;
    CHECK_FALSE(FITOMConfig::resolveCompositeSpec(DEVICE_SSGS3, false, spec));
}

TEST_CASE("SSGS/SSGS2/SSGS3: 混在構成でもSSG部・ADPCM部がそれぞれ束ねの対象になる", "[ssgs]")
{
    // CSpanDeviceの束ねのグループ化キーはVoicePatchType
    // (mergeSpannableDevices、docs/chip-driver-architecture.md 3節)。
    // deviceTypeはサブチップごとに独立して保持されるため、分周比や
    // レジスタ配置の違いは束ねの妨げにならない。
    CHECK(FITOMConfig::deviceTypeToVoicePatchType(DEVICE_SSGS)
          == FITOMConfig::deviceTypeToVoicePatchType(DEVICE_SSGS2));
    CHECK(FITOMConfig::deviceTypeToVoicePatchType(DEVICE_SSGS)
          == FITOMConfig::deviceTypeToVoicePatchType(DEVICE_SSGS3));

    // ADPCM部はcomposite展開の時点で同一deviceTypeになるため自明に同一グループ
    std::vector<FITOMConfig::SubDeviceSpec> s1, s2;
    REQUIRE(FITOMConfig::resolveCompositeSpec(DEVICE_SSGS,  false, s1));
    REQUIRE(FITOMConfig::resolveCompositeSpec(DEVICE_SSGS2, false, s2));
    CHECK(s1[1].deviceType == s2[1].deviceType);
}

TEST_CASE("SSGS: VoicePatchType と パンポット種別の分類", "[ssgs]")
{
    // SSG部は YM2149 とレジスタ互換なので音色データも SSG と共有する
    CHECK(FITOMConfig::deviceTypeToVoicePatchType(DEVICE_SSGS) == VOICE_PATCH_SSG);
    CHECK(FITOMConfig::deviceTypeToVoicePatchType(DEVICE_SSGS2) == VOICE_PATCH_SSG);
    CHECK(FITOMConfig::deviceTypeToVoicePatchType(DEVICE_SSGS3) == VOICE_PATCH_SSG);
    CHECK(FITOMConfig::deviceTypeToVoicePatchType(DEVICE_SSGS_ADPCM)
          == VOICE_PATCH_SSGS_ADPCM);

    // ADPCM部はサンプルベース音源系
    CHECK(isSampleBasedVoicePatchType(VOICE_PATCH_SSGS_ADPCM));
    CHECK(FITOMConfig::voicePatchTypeToVoiceGroup(VOICE_PATCH_SSGS_ADPCM)
          == VOICE_GROUP_PCM);
    CHECK(FITOMConfig::stringToVoicePatchType("SSGS_ADPCM") == VOICE_PATCH_SSGS_ADPCM);
    CHECK(std::string(FITOMConfig::voicePatchTypeToString(VOICE_PATCH_SSGS_ADPCM))
          == "SSGS_ADPCM");

    // 連続的なパンポットレジスタを持つ (リニアステレオ化の対象外)
    using PanType = FITOMConfig::ChipPanType;
    CHECK(FITOMConfig::getChipPanType(DEVICE_SSGS)       == PanType::Continuous);
    CHECK(FITOMConfig::getChipPanType(DEVICE_SSGS2)      == PanType::Continuous);
    CHECK(FITOMConfig::getChipPanType(DEVICE_SSGS3)      == PanType::Continuous);
    CHECK(FITOMConfig::getChipPanType(DEVICE_SSGS_ADPCM) == PanType::Continuous);
    CHECK_FALSE(FITOMConfig::subDeviceAcceptsStereoPair(DEVICE_SSGS));
    CHECK_FALSE(FITOMConfig::subDeviceAcceptsStereoPair(DEVICE_SSGS2));
    CHECK_FALSE(FITOMConfig::subDeviceAcceptsStereoPair(DEVICE_SSGS3));
    CHECK_FALSE(FITOMConfig::subDeviceAcceptsStereoPair(DEVICE_SSGS_ADPCM));
}
