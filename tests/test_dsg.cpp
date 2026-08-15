// tests/test_dsg.cpp
// DSG (YM2163) チップドライバ CDSG / CDSGRhythm の回帰テスト。
//
// 検証の中心は、このチップに固有で壊れやすい3点:
//   1. 発音周波数 f = clock / (DV * 2^(3-oct)) のレジスタ分解
//      (DV は 1/4 きざみの10bit生値、oct は B2B1 の2bit)
//   2. ビルトイン音色 (エンベロープ4種 × 波形5種) の ProgChg からの機械生成
//   3. リズムのレベル書き込みがトリガーより「後」であること
//      (トリガーが内蔵リズムEGのレベルを最大へリセットするため、
//       順序が逆になるとベロシティが一切効かなくなる)

#include <catch2/catch_test_macros.hpp>
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include "fitom/FITOMdefine.h"
#include "fitom/DeviceFactory.h"
#include "fitom/PatchManager.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
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

    // 最後に addr へ書かれた値が history の何番目か (未書き込みなら -1)
    int lastIndexOf(uint16_t addr) const {
        for (int i = static_cast<int>(history.size()) - 1; i >= 0; --i)
            if (history[i].first == addr) return i;
        return -1;
    }
};

constexpr int kDsgClock = 1000000;   // YM2163 標準クロック 1MHz

double noteHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// ビルトイン音色と同じ持ち方の HwPatch を組む
// (hw.ALG = エンベロープ番号、hwOp[0].WS = 波形番号)
HwPatch makeDsgPatch(uint8_t env, uint8_t wave)
{
    HwPatch p{};
    p.id = 1;
    p.hw.ALG     = env;
    p.hwOp[0].WS = wave;
    return p;
}

// reg 0x80+ch / 0x84+ch から DV(10bit) と oct(0-3) を復元する
struct Pitch { int dv; int oct; };
Pitch decodePitch(const RecordingPort& port, uint8_t ch)
{
    uint8_t lo = port.regs.at(static_cast<uint16_t>(0x80 + ch));
    uint8_t hi = port.regs.at(static_cast<uint16_t>(0x84 + ch));
    Pitch r{};
    r.dv  = ((hi & 0x07) << 7) | (lo & 0x7F);
    r.oct = ((hi >> 4) & 1) * 2 + ((hi >> 3) & 1);   // B2*2 + B1
    return r;
}

double pitchToHz(const Pitch& p)
{
    return static_cast<double>(kDsgClock) / (p.dv * std::pow(2.0, 3 - p.oct));
}

} // namespace

namespace fitom {
std::unique_ptr<ISoundDevice> createCDSG(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCDSGRhythm(IPort* p, int sr);
}

// ================================================================
//  楽音部: 発音周波数
// ================================================================

// データシートの例と一致すること: 分周数の生値 DV=568 / B2B1=01 で 440Hz。
// (f = (φ/4) / ΣDVi × (1/2)^(3-B1-2*B2) を DV(=ΣDVi*4) で書き直すと
//  f = φ / (DV * 2^(3-oct)))
TEST_CASE("CDSG reproduces the datasheet A4 example (DV=568, oct=1)", "[dsg][freq]")
{
    RecordingPort port;
    auto dev = createCDSG(&port, kDsgClock);
    dev->init();

    HwPatch patch = makeDsgPatch(1, 3);
    uint8_t ch = dev->allocCh(nullptr, &patch, 127);
    REQUIRE(ch != 0xFF);
    dev->setNoteFine(ch, 69, 0, true);      // A4
    dev->noteOn(ch, 127);

    Pitch p = decodePitch(port, ch);
    INFO("DV=" << p.dv << " oct=" << p.oct);
    CHECK(p.oct == 1);
    CHECK(std::abs(p.dv - 568) <= 1);
    CHECK(std::abs(pitchToHz(p) - 440.0) < 1.0);
}

// 実用音域全体でセント誤差が十分小さいこと。
// 最低音は DV=1023 / oct=0 の約122.2Hz (MIDIノート47 B2 = 123.5Hz のすぐ下)
// なので、そこから上を見る。
TEST_CASE("CDSG pitch is accurate across the playable range", "[dsg][freq]")
{
    for (int note : {48, 55, 60, 69, 72, 84, 96, 108}) {
        RecordingPort port;
        auto dev = createCDSG(&port, kDsgClock);
        dev->init();

        HwPatch patch = makeDsgPatch(1, 1);
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        dev->setNoteFine(ch, static_cast<uint8_t>(note), 0, true);
        dev->noteOn(ch, 127);

        Pitch p = decodePitch(port, ch);
        double cents = 1200.0 * std::log2(pitchToHz(p) / noteHz(note));
        INFO("note=" << note << " DV=" << p.dv << " oct=" << p.oct
             << " hz=" << pitchToHz(p) << " cents=" << cents);
        CHECK(std::abs(cents) < 10.0);
    }
}

// oct(B2B1) は 2^(3-oct) の分周段数であり、oct が大きいほど DV の有効桁が
// 増える。DV が10bitに収まる範囲で最大の oct を選ばないと、低い音ほど
// 分解能を無駄に捨てて音痴になる。
// DV は10bit・octは2bitしか無いため、約122.2Hz (DV=1023, oct=0) より下は
// 表現できずクランプされる。無音にはせず最低音で頭打ちにする。
TEST_CASE("CDSG clamps notes below the chip's lowest frequency", "[dsg][freq]")
{
    for (int note : {0, 24, 36, 46}) {
        RecordingPort port;
        auto dev = createCDSG(&port, kDsgClock);
        dev->init();

        HwPatch patch = makeDsgPatch(1, 1);
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        dev->setNoteFine(ch, static_cast<uint8_t>(note), 0, true);
        dev->noteOn(ch, 127);

        Pitch p = decodePitch(port, ch);
        INFO("note=" << note << " DV=" << p.dv << " oct=" << p.oct
             << " hz=" << pitchToHz(p));
        CHECK(p.dv == 1023);
        CHECK(p.oct == 0);
        CHECK(std::abs(pitchToHz(p) - 122.19) < 0.1);
    }
}

TEST_CASE("CDSG maximizes DV resolution by picking the largest usable octave",
          "[dsg][freq]")
{
    for (int note = 48; note <= 108; ++note) {
        RecordingPort port;
        auto dev = createCDSG(&port, kDsgClock);
        dev->init();

        HwPatch patch = makeDsgPatch(1, 1);
        uint8_t ch = dev->allocCh(nullptr, &patch, 127);
        REQUIRE(ch != 0xFF);
        dev->setNoteFine(ch, static_cast<uint8_t>(note), 0, true);
        dev->noteOn(ch, 127);

        Pitch p = decodePitch(port, ch);
        INFO("note=" << note << " DV=" << p.dv << " oct=" << p.oct);
        CHECK(p.dv <= 0x3FF);
        CHECK(p.oct <= 3);
        // oct をもう1段上げられる (=DVを2倍にしても10bitに収まる) なら、
        // その方が分解能が高いので選ばれているはず。
        if (p.oct < 3) CHECK(p.dv * 2 > 0x3FF);
    }
}

// ================================================================
//  楽音部: 音色 / サスティン / キーオン
// ================================================================

// reg 0x88+ch は E2E1(bit6-5) / SUS(bit4) / W3W2W1(bit2-0)。
TEST_CASE("CDSG writes envelope and waveform selection to reg 0x88", "[dsg][voice]")
{
    for (uint8_t env = 0; env < 4; ++env) {
        for (uint8_t wave = 1; wave <= 5; ++wave) {
            RecordingPort port;
            auto dev = createCDSG(&port, kDsgClock);
            dev->init();

            HwPatch patch = makeDsgPatch(env, wave);
            uint8_t ch = dev->allocCh(nullptr, &patch, 127);
            REQUIRE(ch != 0xFF);

            uint8_t reg = port.regs.at(static_cast<uint16_t>(0x88 + ch));
            INFO("env=" << (int)env << " wave=" << (int)wave
                 << " reg88=" << (int)reg);
            CHECK(((reg >> 5) & 3) == env);
            CHECK((reg & 7) == wave);
            CHECK(((reg >> 4) & 1) == 0);   // サスティンペダル未踏
        }
    }
}

// サスティンペダル(CC#64)はハードウェアの SUS ビットへ直結する。
// SUS=1 でキーオフ後のリリースが1.2秒へ伸びる(データシート図2)。
TEST_CASE("CDSG maps the sustain pedal onto the hardware SUS bit", "[dsg][sustain]")
{
    RecordingPort port;
    auto dev = createCDSG(&port, kDsgClock);
    dev->init();

    HwPatch patch = makeDsgPatch(2, 3);
    uint8_t ch = dev->allocCh(nullptr, &patch, 127);
    REQUIRE(ch != 0xFF);
    const uint16_t reg = static_cast<uint16_t>(0x88 + ch);

    dev->setSustain(ch, true, true);
    CHECK((port.regs.at(reg) & 0x10) == 0x10);
    // 音色部分は壊さない
    CHECK(((port.regs.at(reg) >> 5) & 3) == 2);
    CHECK((port.regs.at(reg) & 7) == 3);

    dev->setSustain(ch, false, true);
    CHECK((port.regs.at(reg) & 0x10) == 0);
}

// KON は reg 0x84+ch bit6。実機/エミュレータともエッジ検出なので、
// 同じレジスタに同居する分周数上位/オクターブを壊してはならない。
TEST_CASE("CDSG key on/off touches only the KON bit of reg 0x84", "[dsg][keyon]")
{
    RecordingPort port;
    auto dev = createCDSG(&port, kDsgClock);
    dev->init();

    HwPatch patch = makeDsgPatch(1, 1);
    uint8_t ch = dev->allocCh(nullptr, &patch, 127);
    REQUIRE(ch != 0xFF);
    dev->setNoteFine(ch, 69, 0, true);
    dev->noteOn(ch, 127);

    const uint16_t reg = static_cast<uint16_t>(0x84 + ch);
    Pitch onPitch = decodePitch(port, ch);
    CHECK((port.regs.at(reg) & 0x40) == 0x40);
    CHECK((port.regs.at(reg) & 0x20) == 0);      // FD は立てない

    dev->noteOff(ch);
    CHECK((port.regs.at(reg) & 0x40) == 0);
    Pitch offPitch = decodePitch(port, ch);
    CHECK(offPitch.dv == onPitch.dv);
    CHECK(offPitch.oct == onPitch.oct);
}

// CC#120 (All Sound Off) は SUS を落としたうえで FD(フォーシングダンプ)を
// 立て、通常のリリースより速く消音する。
TEST_CASE("CDSG forceDamp clears SUS and pulses the FD bit", "[dsg][keyon]")
{
    RecordingPort port;
    auto dev = createCDSG(&port, kDsgClock);
    dev->init();

    HwPatch patch = makeDsgPatch(1, 1);
    uint8_t ch = dev->allocCh(nullptr, &patch, 127);
    REQUIRE(ch != 0xFF);
    dev->setNoteFine(ch, 69, 0, true);
    dev->setSustain(ch, true, true);
    dev->noteOn(ch, 127);

    dev->forceDamp(ch);

    CHECK((port.regs.at(static_cast<uint16_t>(0x88 + ch)) & 0x10) == 0); // SUS解除
    // FD の立ち上がりが記録に残り、最終的にKONも落ちていること
    const uint16_t reg = static_cast<uint16_t>(0x84 + ch);
    bool sawFd = false;
    for (const auto& [addr, val] : port.history)
        if (addr == reg && (val & 0x20)) sawFd = true;
    CHECK(sawFd);
    CHECK((port.regs.at(reg) & 0x40) == 0);
}

// 音量 VL2VL1 は 0 / -6 / -12 / -∞ dB の4段階しか無い。
// 最大音量では 0dB(=0)、無音では -∞(=3) にならなければならない。
TEST_CASE("CDSG maps loudness onto the 2bit volume field", "[dsg][volume]")
{
    RecordingPort port;
    auto dev = createCDSG(&port, kDsgClock);
    dev->init();

    HwPatch patch = makeDsgPatch(1, 1);
    uint8_t ch = dev->allocCh(nullptr, &patch, 127);
    REQUIRE(ch != 0xFF);
    const uint16_t reg = static_cast<uint16_t>(0x8C + ch);

    dev->setVolume(ch, 127, false);
    dev->setExpression(ch, 127, false);
    dev->noteOn(ch, 127);
    CHECK(((port.regs.at(reg) >> 4) & 3) == 0);      // 0dB
    CHECK((port.regs.at(reg) & 0x0F) == 0x01);       // OR1 のみ

    dev->setVolume(ch, 0, true);
    CHECK(((port.regs.at(reg) >> 4) & 3) == 3);      // -∞

    // 音量を下げるほど減衰段が単調に増えること
    int prev = -1;
    for (int vol : {127, 110, 96, 80, 64, 32, 0}) {
        dev->setVolume(ch, static_cast<uint8_t>(vol), true);
        int vl = (port.regs.at(reg) >> 4) & 3;
        INFO("vol=" << vol << " VL=" << vl);
        CHECK(vl >= prev);
        prev = vl;
    }
}

// ================================================================
//  ビルトイン音色バンク (暗黙のパッチバンク)
// ================================================================

// YM2163 はユーザー音色を持たず、音色は「エンベロープ4種 × 波形5種」の
// 20通りしかない。ProgChg の値から機械的にこの組み合わせが決まること。
TEST_CASE("PatchManager generates the 20 DSG builtin voices from the program number",
          "[dsg][patch]")
{
    PatchManager pm;
    const auto& bank = pm.getDsgBuiltinPatches();
    REQUIRE(bank.size() == 20);

    for (int prog = 0; prog < kDsgBuiltinPatchCount; ++prog) {
        const HwPatch* p = pm.getDsgBuiltinPatchByProg(static_cast<uint8_t>(prog));
        REQUIRE(p != nullptr);
        INFO("prog=" << prog << " name=" << p->name);
        CHECK(p->isValid());
        CHECK(p->id == static_cast<uint32_t>(prog));
        // 波形メジャー(同じ波形の4エンベロープが連続する)
        CHECK(p->hwOp[0].WS == static_cast<uint8_t>(prog / 4 + 1));
        CHECK(p->hw.ALG     == static_cast<uint8_t>(prog % 4));
        // 未定義の波形番号(0/6/7 = 無音)が混ざっていないこと
        CHECK(p->hwOp[0].WS >= 1);
        CHECK(p->hwOp[0].WS <= 5);
        CHECK(p->name[0] != '\0');
    }

    // 音色名も「<波形名>.<エンベロープ名>」で機械的に生成される。
    static const char* const kWave[5] = {"St", "Or", "Cl", "Pf", "Hc"};
    static const char* const kEnv[4]  = {"Percussive", "Wind", "Sustain", "Plateau"};
    for (int prog = 0; prog < kDsgBuiltinPatchCount; ++prog) {
        std::string expected = std::string(kWave[prog / 4]) + "." + kEnv[prog % 4];
        INFO("prog=" << prog);
        CHECK(std::string(bank[prog].name) == expected);
    }
    CHECK(std::string(pm.getDsgBuiltinPatchByProg(0)->name)  == "St.Percussive");
    CHECK(std::string(pm.getDsgBuiltinPatchByProg(12)->name) == "Pf.Percussive");
    CHECK(std::string(pm.getDsgBuiltinPatchByProg(16)->name) == "Hc.Percussive");
    CHECK(std::string(pm.getDsgBuiltinPatchByProg(1)->name)  == "St.Wind");
    CHECK(std::string(pm.getDsgBuiltinPatchByProg(3)->name)  == "St.Plateau");

    // 範囲外は解決できない (ユーザー音色は存在しないのでフォールバックも無い)
    CHECK(pm.getDsgBuiltinPatchByProg(20) == nullptr);
    CHECK(pm.getDsgBuiltinPatchByProg(127) == nullptr);

    // 20音色すべてが相異なる (波形, エンベロープ) の組であること
    std::vector<std::pair<uint8_t, uint8_t>> seen;
    for (const auto& p : bank) seen.emplace_back(p.hwOp[0].WS, p.hw.ALG);
    std::sort(seen.begin(), seen.end());
    CHECK(std::unique(seen.begin(), seen.end()) == seen.end());
}

// ================================================================
//  内蔵リズム音源
// ================================================================

// パート番号はリズムトリガー reg 0x90 の D0-D4 に一致する:
//   0=BD / 1=HC / 2=SDN / 3=HHO / 4=HHD
// トリガーは自動的に0へ戻る「書いたビットだけを立てる」レジスタなので、
// 他パートのビットをORしてはならない。
TEST_CASE("CDSGRhythm triggers each part with its own bit in reg 0x90",
          "[dsg][rhythm]")
{
    static constexpr uint8_t kExpect[5] = {0x01, 0x02, 0x04, 0x08, 0x10};

    RecordingPort port;
    auto dev = createCDSGRhythm(&port, kDsgClock);
    dev->init();
    REQUIRE(dev->getChCount() == 5);

    for (uint8_t part = 0; part < 5; ++part) {
        HwPatch patch{};
        patch.id = part;
        patch.hw.ALG = part;                       // パート番号指定
        uint8_t ch = dev->assignCh(part, nullptr, &patch, 127);
        REQUIRE(ch == part);
        dev->noteOn(ch, 127);
        INFO("part=" << (int)part);
        CHECK(port.regs.at(0x90) == kExpect[part]);
    }
}

// レベルレジスタは HH/BD/HC/SDN の4本しかなく、HHO と HHD は同じ 0x94 を
// 共有する (実機のリズム発振器を共有しているため)。
TEST_CASE("CDSGRhythm routes each part to its rhythm level register",
          "[dsg][rhythm]")
{
    static constexpr uint16_t kLevelReg[5] = {0x95, 0x96, 0x97, 0x94, 0x94};

    for (uint8_t part = 0; part < 5; ++part) {
        RecordingPort port;
        auto dev = createCDSGRhythm(&port, kDsgClock);
        dev->init();

        HwPatch patch{};
        patch.id = part;
        patch.hw.ALG = part;
        uint8_t ch = dev->assignCh(part, nullptr, &patch, 100);
        REQUIRE(ch == part);
        port.history.clear();
        dev->noteOn(ch, 100);

        INFO("part=" << (int)part);
        CHECK(port.lastIndexOf(kLevelReg[part]) >= 0);
    }
}

// リズムトリガーは内蔵リズムEGのレベルを最大へリセットするため、
// レベル書き込みが「トリガーより後」でなければベロシティが一切効かない。
TEST_CASE("CDSGRhythm writes the level after the trigger so velocity applies",
          "[dsg][rhythm]")
{
    RecordingPort port;
    auto dev = createCDSGRhythm(&port, kDsgClock);
    dev->init();

    HwPatch patch{};
    patch.id = 0;
    patch.hw.ALG = 0;                       // BD (レベルは 0x95)
    uint8_t ch = dev->assignCh(0, nullptr, &patch, 127);
    REQUIRE(ch == 0);

    port.history.clear();
    dev->noteOn(ch, 127);

    int trig  = port.lastIndexOf(0x90);
    int level = port.lastIndexOf(0x95);
    INFO("trigger@" << trig << " level@" << level);
    REQUIRE(trig >= 0);
    REQUIRE(level >= 0);
    CHECK(level > trig);
}

// リズムレベル LV4-LV0 は「0が最大音量、31が最小音量」の線形減衰。
// LH(bit0)=0 のままにして内蔵リズムEGの減衰を働かせる。
TEST_CASE("CDSGRhythm scales the rhythm level with velocity, keeping LH=0",
          "[dsg][rhythm]")
{
    int prev = -1;
    for (int vel : {127, 100, 80, 60, 40, 1}) {
        RecordingPort port;
        auto dev = createCDSGRhythm(&port, kDsgClock);
        dev->init();

        HwPatch patch{};
        patch.id = 0;
        patch.hw.ALG = 0;                   // BD
        uint8_t ch = dev->assignCh(0, nullptr, &patch, static_cast<uint8_t>(vel));
        REQUIRE(ch == 0);
        dev->noteOn(ch, static_cast<uint8_t>(vel));

        uint8_t reg = port.regs.at(0x95);
        int lv = (reg >> 1) & 0x1F;
        INFO("vel=" << vel << " LV=" << lv);
        CHECK((reg & 1) == 0);              // LH=0 (内蔵EGで減衰させる)
        CHECK(lv >= prev);                  // ベロシティが下がるほど減衰量が増える
        prev = lv;
    }
    CHECK(prev > 0);                        // 最弱ベロシティでは実際に絞られている
}

// 最大ベロシティでは減衰0 (LV=0) でなければならない。
// (COPLLRhythm で実際に起きた「ラウドネス空間の極性反転で強打ほど静かに
//  なる」種のバグを直接押さえる)
TEST_CASE("CDSGRhythm plays at full level for maximum velocity", "[dsg][rhythm]")
{
    RecordingPort port;
    auto dev = createCDSGRhythm(&port, kDsgClock);
    dev->init();

    HwPatch patch{};
    patch.id = 0;
    patch.hw.ALG = 0;
    uint8_t ch = dev->assignCh(0, nullptr, &patch, 127);
    REQUIRE(ch == 0);
    dev->noteOn(ch, 127);

    CHECK(((port.regs.at(0x95) >> 1) & 0x1F) == 0);
}

// パート番号は音色データの hw.ALG で直接指定する (COPLLRhythm と同じ規約)。
TEST_CASE("CDSGRhythm selects the part from hw.ALG", "[dsg][rhythm]")
{
    RecordingPort port;
    auto dev = createCDSGRhythm(&port, kDsgClock);
    dev->init();

    for (uint8_t part = 0; part < 5; ++part) {
        HwPatch patch{};
        patch.hw.ALG = part;
        CHECK(dev->queryCh(nullptr, &patch, 0) == part);
    }
    HwPatch oob{};
    oob.hw.ALG = 5;
    CHECK(dev->queryCh(nullptr, &oob, 0) == 0xFF);
    CHECK(dev->queryCh(nullptr, nullptr, 0) == 0xFF);
}

// ================================================================
//  デバイス構成
// ================================================================

// DSG は楽音4ch と内蔵リズム5パートを持ち、両者のレジスタ空間
// (0x80-0x8F / 0x90-0x97) は独立している。
TEST_CASE("DeviceFactory reports the DSG channel layout", "[dsg][factory]")
{
    CHECK(DeviceFactory::defaultChCount(DEVICE_DSG) == 4);
    CHECK(DeviceFactory::defaultChCount(DEVICE_DSG_RHY) == 5);

    RecordingPort port;
    port.clock = kDsgClock;
    auto tone = DeviceFactory::create(DEVICE_DSG, &port, 44100);
    REQUIRE(tone != nullptr);
    CHECK(tone->getChCount() == 4);
    CHECK(tone->getDeviceType() == DEVICE_DSG);

    auto rhy = DeviceFactory::create(DEVICE_DSG_RHY, &port, 44100);
    REQUIRE(rhy != nullptr);
    CHECK(rhy->getChCount() == 5);
    CHECK(rhy->getDeviceType() == DEVICE_DSG_RHY);
}

// ROM固定音色しか持たないため、他チップの音色データを受け取ってはならない
// (OPLLのプリセット音色をフォールバック対象外にしているのと同じ方針)。
TEST_CASE("DSG never accepts a voice fallback from another chip", "[dsg][factory]")
{
    HwPatch patch = makeDsgPatch(0, 1);
    for (int src : {VOICE_PATCH_SSG, VOICE_PATCH_DCSG, VOICE_PATCH_SCC,
                    VOICE_PATCH_EPSG, VOICE_PATCH_SAA, VOICE_PATCH_OPLL}) {
        INFO("source voicePatchType=" << src);
        CHECK_FALSE(DeviceFactory::acceptsFallback(
            DEVICE_DSG, static_cast<uint8_t>(src), patch));
    }
}
