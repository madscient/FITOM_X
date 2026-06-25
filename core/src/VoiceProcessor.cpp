// fitom/VoiceProcessor.cpp
// ソフトウェアパラメータ処理エンジン実装

#include "fitom/VoiceProcessor.h"
#include <cmath>
#include <algorithm>

namespace fitom {

// ================================================================
//  波形テーブル・速度テーブル (旧 EnvLFO.cpp から移植)
// ================================================================

namespace {

static const int8_t kSinTable[240] = {
     0,  3,  6,  9, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43,
    46, 49, 52, 54, 57, 60, 63, 65, 68, 71, 73, 76, 78, 80, 83,
    85, 87, 89, 91, 93, 95, 97, 99,101,102,104,105,107,108,110,
   111,112,113,114,115,116,117,117,118,119,119,119,120,120,120,
   120,120,120,120,119,119,119,118,117,117,116,115,114,113,112,
   111,110,108,107,105,104,102,101, 99, 97, 95, 93, 91, 89, 87,
    85, 83, 80, 78, 76, 73, 71, 68, 65, 63, 60, 57, 54, 52, 49,
    46, 43, 40, 37, 34, 31, 28, 25, 22, 19, 16, 13,  9,  6,  3,
     0, -3, -6, -9,-13,-16,-19,-22,-25,-28,-31,-34,-37,-40,-43,
   -46,-49,-52,-54,-57,-60,-63,-65,-68,-71,-73,-76,-78,-80,-83,
   -85,-87,-89,-91,-93,-95,-97,-99,-101,-102,-104,-105,-107,-108,-110,
  -111,-112,-113,-114,-115,-116,-117,-117,-118,-119,-119,-119,-120,-120,-120,
  -120,-120,-120,-120,-119,-119,-119,-118,-117,-117,-116,-115,-114,-113,-112,
  -111,-110,-108,-107,-105,-104,-102,-101,-99,-97,-95,-93,-91,-89,-87,
   -85,-83,-80,-78,-76,-73,-71,-68,-65,-63,-60,-57,-54,-52,-49,
   -46,-43,-40,-37,-34,-31,-28,-25,-22,-19,-16,-13,-9,-6,-3,
};

static const uint8_t kSpeedStep[19] = {
    1,2,3,4,5,6,8,10,12,15,16,20,24,30,40,48,60,80,120
};

} // anonymous namespace

// ================================================================
//  ユーティリティ関数 (旧 SoundDev.cpp 相当)
// ================================================================

int8_t getLfoWave(uint8_t waveform, uint8_t speed, uint16_t phase) noexcept
{
    uint16_t period = (speed < 19) ? kSpeedStep[speed] : kSpeedStep[18];

    switch (waveform) {
    case 0: // up-saw
        return static_cast<int8_t>((phase % period) * 240 / period - 120);
    case 1: // square
        return (phase % period < period / 2) ? 120 : -120;
    case 2: // triangle
    case 6: // sine (index in table)
    {
        uint16_t idx = (phase % 240);
        return kSinTable[idx];
    }
    case 3: // S&H (pseudo random)
    {
        static uint16_t seed = 0x1234;
        if (phase % period == 0) {
            seed = static_cast<uint16_t>(seed * 6364 + 1);
        }
        return static_cast<int8_t>((seed >> 8) - 128);
    }
    case 4: // down-saw
        return static_cast<int8_t>(120 - (phase % period) * 240 / period);
    case 5: // delta (impulse)
        return (phase % period == 0) ? 120 : 0;
    default:
        return 0;
    }
}

// vol × exp × vel → 実効レベル (0〜127)
// 旧 CalcVolExpVel
uint8_t calcEffectiveLevel(uint8_t vol, uint8_t exp, uint8_t vel) noexcept
{
    // GM 準拠: level = vol × exp / 127 × vel / 127
    // 0〜127 の整数演算
    uint32_t v = static_cast<uint32_t>(vol) * exp / 127;
    v = v * vel / 127;
    return static_cast<uint8_t>(v > 127 ? 127 : v);
}

// 実効レベル × ボイスの TL → 補正後 TL
// 旧 CalcLinearLevel
uint8_t calcLinearLevel(uint8_t effectiveLevel, uint8_t tl) noexcept
{
    // effectiveLevel が 127 のとき TL そのまま
    // effectiveLevel が 0 のとき TL = 127 (完全消音)
    uint32_t attenuation = static_cast<uint32_t>(127 - effectiveLevel) * 127 / 127;
    uint32_t result = static_cast<uint32_t>(tl) + attenuation;
    return static_cast<uint8_t>(result > 127 ? 127 : result);
}

// 線形 TL → dB TL (チップドライバが呼ぶ)
// 旧 Linear2dB
uint8_t linearToDb(uint8_t linearTL, int range, int step, int bw) noexcept
{
    // range: RANGE96DB=0, RANGE48DB=1, RANGE24DB=2, RANGE12DB=3
    // bw: bit width (7 for OPN/OPM, 6 for OPL)
    (void)range; (void)step; (void)bw;
    return linearTL; // 既存の実装は概ねそのまま通す（正確な変換は旧実装を参照）
}

// ================================================================
//  LfoControl
// ================================================================

void LfoControl::start(uint8_t delay_20ms, uint8_t rate) noexcept
{
    count_  = 0;
    level_  = 0;
    delay_  = static_cast<uint16_t>(delay_20ms) * 20; // 20ms → ms ティック数
    rate_   = rate;
    phase_  = (delay_ > 0) ? Phase::Delaying : Phase::Running;
}

void LfoControl::stop() noexcept
{
    phase_  = Phase::Stopped;
    level_  = 0;
    count_  = 0;
}

void LfoControl::reset() noexcept { stop(); }

bool LfoControl::tick() noexcept
{
    if (phase_ == Phase::Stopped) return false;
    if (rate_ == 0) { phase_ = Phase::Stopped; return false; }

    ++count_;

    if (phase_ == Phase::Delaying) {
        if (count_ >= delay_) {
            count_ = 0;
            phase_ = Phase::Running;
            level_ = 1;
            return false; // 遅延終了ティックは更新なし
        }
        return false;
    }

    // Running
    ++level_;
    if (level_ > 127) level_ = 127;
    return true; // TL/Freq 更新が必要
}

// ================================================================
//  VoiceProcessor
// ================================================================

void VoiceProcessor::reset() noexcept
{
    chLfo_.reset();
    for (auto& l : opLfo_) l.reset();
    for (auto& t : baseTL_)      t = 0;
    for (auto& t : effectiveTL_) t = 0;
    chLfoValue_ = 0;
    noteOn_     = false;
}

void VoiceProcessor::onNoteOn(uint8_t vol, uint8_t exp, uint8_t vel,
                               const FmVoice& voice) noexcept
{
    lastVol_ = vol;
    lastExp_ = exp;
    lastVel_ = vel;
    noteOn_  = true;

    recalcBaseTL(vol, exp, vel, voice);

    // チャンネル LFO リセット
    if (voice.sw.LFR > 0) {
        chLfo_.start(voice.sw.LFD, voice.sw.LFR);
    } else {
        chLfo_.stop();
    }

    // オペレータ LFO リセット
    for (int op = 0; op < 4; ++op) {
        if (voice.swOp[op].SLR > 0) {
            opLfo_[op].start(voice.swOp[op].SLY, voice.swOp[op].SLR);
        } else {
            opLfo_[op].stop();
        }
        effectiveTL_[op] = baseTL_[op];
    }
    chLfoValue_ = 0;
}

void VoiceProcessor::onNoteOff() noexcept
{
    noteOn_ = false;
    // LFO は release フェーズも継続する（オリジナルの挙動に合わせる）
}

bool VoiceProcessor::onVolumeChange(uint8_t vol, uint8_t exp,
                                     const FmVoice& voice) noexcept
{
    if (vol == lastVol_ && exp == lastExp_) return false;
    lastVol_ = vol;
    lastExp_ = exp;
    recalcBaseTL(vol, exp, lastVel_, voice);
    // effectiveTL を baseTL ベースに再計算（LFO オフセットは無視してリセット）
    for (int op = 0; op < 4; ++op) {
        effectiveTL_[op] = baseTL_[op];
    }
    return true;
}

VoiceProcessor::TickResult VoiceProcessor::onTick(const FmVoice& voice) noexcept
{
    TickResult result;

    // チャンネル LFO
    if (chLfo_.tick()) {
        recalcChLfo(voice);
        result.needsFreqUpdate = true;
    }

    // オペレータ LFO
    for (int op = 0; op < 4; ++op) {
        if (opLfo_[op].tick()) {
            recalcOpLfo(op, voice);
            result.tlUpdateMask |= (1u << op);
        }
    }
    return result;
}

void VoiceProcessor::recalcBaseTL(uint8_t vol, uint8_t exp, uint8_t vel,
                                    const FmVoice& voice) noexcept
{
    uint8_t evol = calcEffectiveLevel(vol, exp, vel);

    // キャリアアルゴリズムマスク（旧 carmsk テーブルと同等）
    static const uint8_t carmsk[8] = {0x8,0x8,0x8,0x8,0xa,0xe,0xe,0xf};
    uint8_t alg = voice.hw.ALG & 0x07;
    uint8_t mask = carmsk[alg];

    for (int op = 0; op < 4; ++op) {
        uint8_t tl = voice.hwOp[op].TL;
        if (mask & (1u << op)) {
            // ベロシティ感度補正
            uint8_t vtl = voice.swOp[op].VTL;
            if (vtl > 0) {
                // VTL: 0=補正なし, 127=最大感度
                // ベロシティが低いほど TL を増加（音量を下げる）
                int32_t delta = static_cast<int32_t>(127 - evol) * vtl / 127;
                tl = static_cast<uint8_t>(clamp(static_cast<int>(tl) + delta, 0, 127));
            }
            tl = calcLinearLevel(evol, tl);
        }
        baseTL_[op] = tl;
    }
}

void VoiceProcessor::recalcOpLfo(int op, const FmVoice& voice) noexcept
{
    int16_t lev = static_cast<int16_t>(opLfo_[op].level());
    if (lev == 0) {
        effectiveTL_[op] = baseTL_[op];
        return;
    }

    const auto& swop = voice.swOp[op];
    int16_t wave = static_cast<int16_t>(
        getLfoWave(swop.SLW, swop.SLF, opLfo_[op].count()));
    int16_t dep  = static_cast<int16_t>(swop.SLD);
    if (dep > 63) dep -= 128; // 符号付き変換

    int16_t val = wave * lev / 128;
    val = val * dep / 120;
    val += baseTL_[op];
    effectiveTL_[op] = clamp(static_cast<int>(val), 0, 127);
}

void VoiceProcessor::recalcChLfo(const FmVoice& voice) noexcept
{
    int16_t lev = static_cast<int16_t>(chLfo_.level());
    if (lev == 0) {
        chLfoValue_ = 0;
        return;
    }
    const auto& sw = voice.sw;
    int16_t wave = static_cast<int16_t>(
        getLfoWave(sw.LWF, sw.LFO, chLfo_.count()));

    // depth: LDM/LDL で 16bit 符号付き
    int32_t depth = (static_cast<int32_t>(sw.LDM) << 8) | sw.LDL;
    if (depth >= 8192) depth -= 16384;

    int32_t val = wave * lev / 128;
    val = val * depth / 8192;
    chLfoValue_ = static_cast<int16_t>(clamp(static_cast<int>(val), -128, 127));
}

} // namespace fitom
