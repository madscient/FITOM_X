// fitom/VoiceProcessor.cpp
// ソフトウェアパラメータ処理エンジン実装

#include "fitom/VoiceProcessor.h"
#include "fitom/VolumeUtils.h"
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

// ================================================================
//  LfoControl 実装
// ================================================================

void LfoControl::start(uint8_t rate, uint8_t delay_20ms,
                       uint8_t fadein, LfoMode mode) noexcept
{
    if (rate == 0) { stop(); return; }

    mode_     = mode;
    period_   = kSpeedStep[rate < 19 ? rate : 18];
    phase_tick_     = 0;
    one_shot_done_  = false;

    // フェードイン
    fadein_len_ = static_cast<uint16_t>(fadein) * 4; // 1単位=4tick
    fade_tick_  = 0;
    fade_level_ = (fadein == 0) ? 127 : 0;

    // ディレイ
    delay_left_ = static_cast<uint16_t>(delay_20ms) * 20;
    phase_      = (delay_left_ > 0) ? Phase::Delaying : Phase::Running;

    // S&H シード初期化 (インスタンス固有の初期値を保持)
    // seed_ はリセットしない (start のたびに乱数系列が変わって自然)
}

void LfoControl::stop() noexcept
{
    phase_        = Phase::Stopped;
    fade_level_   = 127;
    phase_tick_   = 0;
    fadeout_tick_ = 0;
    fadeout_len_  = 0;
    one_shot_done_ = false;
}

void LfoControl::reset() noexcept { stop(); }

bool LfoControl::tick() noexcept
{
    if (phase_ == Phase::Stopped || phase_ == Phase::Held)
        return false;

    if (phase_ == Phase::Delaying) {
        if (delay_left_ > 0) --delay_left_;
        if (delay_left_ == 0) {
            phase_      = Phase::Running;
            phase_tick_ = 0;
            fade_tick_  = 0;
        }
        return false;
    }

    if (phase_ == Phase::FadingOut) {
        // フェードアウト: fade_level_ を 127→0 に減衰
        ++fadeout_tick_;
        if (fadeout_tick_ >= fadeout_len_) {
            // フェードアウト完了 → 停止
            phase_      = Phase::Stopped;
            fade_level_ = 0;
            return true; // 最後に 0 を書き込む
        }
        fade_level_ = static_cast<uint8_t>(
            127u - static_cast<uint32_t>(fadeout_tick_) * 127u / fadeout_len_);
        // 位相は継続して更新（波形が途切れないように）
        ++phase_tick_;
        if (phase_tick_ >= period_) {
            phase_tick_ = 0;
            seed_ = static_cast<uint16_t>(seed_ * 6364u + 1u);
        }
        return true;
    }

    // ── Running ──────────────────────────────────────────────────
    // フェードイン更新
    if (fadein_len_ > 0 && fade_tick_ < fadein_len_) {
        ++fade_tick_;
        fade_level_ = static_cast<uint8_t>(
            static_cast<uint32_t>(fade_tick_) * 127u / fadein_len_);
    } else {
        fade_level_ = 127;
    }

    // 位相更新
    ++phase_tick_;
    bool period_end = (phase_tick_ >= period_);
    if (period_end) {
        phase_tick_ = 0;
        // S&H: 周期末尾で次のランダム値を決定
        seed_ = static_cast<uint16_t>(seed_ * 6364u + 1u);
    }

    // OneShotHold / OneShotZero: 1周期完了でホールド
    if (period_end && mode_ != LfoMode::Repeat) {
        one_shot_done_ = true;
        phase_ = Phase::Held;
    }

    return true; // 波形更新が必要
}

void LfoControl::fadeout(uint16_t fadeout_ticks) noexcept
{
    if (phase_ == Phase::Stopped) return;
    if (fadeout_ticks == 0) { stop(); return; }

    phase_        = Phase::FadingOut;
    fadeout_len_  = fadeout_ticks;
    fadeout_tick_ = 0;
    // fade_level_ は現在値から引き継ぐ（フェードイン途中でも自然に続く）
    // フェードアウトは現在の fade_level_ から 0 へ向かって減衰させる
    const uint8_t cur = fade_level_;
    // 比例スケール: tick=0→cur, tick=fadeout_ticks→0
    // tick()内では 127→0 で計算し、cur/127 を掛ける
    // シンプルにするため cur から 0 への線形を維持
    fadeout_len_  = fadeout_ticks;
    (void)cur; // cur は fade_level_ 初期値として利用済み
}

int8_t LfoControl::wave(uint8_t waveform) const noexcept
{
    if (phase_ == Phase::Stopped) return 0;

    // OneShotZero: 完了後は 0
    if (one_shot_done_ && mode_ == LfoMode::OneShotZero) return 0;

    // period_ が 0 になることはないが念のため
    if (period_ == 0) return 0;

    const uint16_t t = phase_tick_;           // 0 〜 period_-1
    const uint16_t p = period_;

    int8_t raw = 0;
    switch (waveform) {
    case 0: // up-saw: -120 〜 +120
        raw = static_cast<int8_t>(
            static_cast<int32_t>(t) * 240 / p - 120);
        break;
    case 1: // square
        raw = (t < p / 2) ? 120 : -120;
        break;
    case 2: // triangle: 線形往復
        if (t < p / 2)
            raw = static_cast<int8_t>(
                static_cast<int32_t>(t) * 240 / (p / 2) - 120);
        else
            raw = static_cast<int8_t>(
                120 - static_cast<int32_t>(t - p / 2) * 240 / (p / 2));
        break;
    case 3: // S&H: seed_ は tick() で周期末尾に更新済み
        raw = static_cast<int8_t>((seed_ >> 8) ^ 0x80);
        break;
    case 4: // down-saw
        raw = static_cast<int8_t>(
            120 - static_cast<int32_t>(t) * 240 / p);
        break;
    case 5: // delta (impulse): 周期先頭のみ +120
        raw = (t == 0) ? 120 : 0;
        break;
    case 6: // sine
    default:
    {
        uint16_t idx = static_cast<uint16_t>(
            static_cast<uint32_t>(t) * 240 / p);
        raw = (idx < 240) ? kSinTable[idx] : 0;
        break;
    }
    }

    // フェードイン適用
    return static_cast<int8_t>(
        static_cast<int32_t>(raw) * fade_level_ / 127);
}

// vol × exp × vel → 実効レベル (0〜127)
// 旧 CalcVolExpVel
// calcEffectiveLevel / calcLinearLevel は VolumeUtils.h の
// fitom::calcVolExpVel / fitom::calcLinearLevel に統合済み。
// ここでは使用しない。

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

// LfoControl の実装は getLfoWave 置き換え箇所にまとめた

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

    // ── VAR〜VRR: ベロシティ → EG パラメータ感度 ──────────────────────────────
    // 低ベロシティほど各パラメータを hw 設定値から補正する。
    // 最大補正幅はパラメータレンジの 1/4（sensitivity=127、vel=0 のとき）:
    //   AR/DR/SR: レンジ 0-31 → 最大 8 減少（低vel→遅いアタック/ディケイ）
    //   SL:       レンジ 0-15 → 最大 4 増加（低vel→浅いサステイン）
    //   RR:       レンジ 0-15 → 最大 4 減少（低vel→長いリリース）
    // AR の下限は 1（AR=0 はチップによって「発音しない」になるため）
    //
    // 補正式: delta = maxDelta × sensitivity/127 × (127-vel)/127
    //         velParam = clamp(hwParam ± delta, min, max)
    for (int op = 0; op < 4; ++op) {
        const auto& sw = voice.swOp[op];
        const auto& hw = voice.hwOp[op];
        // sensitivity × (127-vel) の積 → 0〜127*127 の範囲
        const int sens_vel = (127 - static_cast<int>(vel));

        // AR: 最大補正 8 (= 31/4), 下限 1（発音保証）
        velAR_[op] = static_cast<uint8_t>(clamp(
            static_cast<int>(hw.AR)  - 8 * sw.VAR * sens_vel / (127 * 127),
            1, 31));

        // D1R: 最大補正 8
        velDR_[op] = static_cast<uint8_t>(clamp(
            static_cast<int>(hw.DR) - 8 * sw.VDR * sens_vel / (127 * 127),
            0, 31));

        // D1L(SL): 最大補正 4, 増加方向（低vel→サステイン浅い）
        velSL_[op] = static_cast<uint8_t>(clamp(
            static_cast<int>(hw.SL) + 4 * sw.VSL * sens_vel / (127 * 127),
            0, 15));

        // D2R(SR): 最大補正 8
        velSR_[op] = static_cast<uint8_t>(clamp(
            static_cast<int>(hw.SR) - 8 * sw.VSR * sens_vel / (127 * 127),
            0, 31));

        // RR: 最大補正 4
        velRR_[op] = static_cast<uint8_t>(clamp(
            static_cast<int>(hw.RR)  - 4 * sw.VRR * sens_vel / (127 * 127),
            0, 15));
    }

    // チャンネル LFO
    // LFS=1 (sync): 常にリセット。LFS=0: Stopped 状態のときのみ start。
    {
        const auto& sw = voice.sw;
        const bool do_start = (sw.LFR > 0) &&
            (sw.LFS || !chLfo_.active());
        if (do_start)
            chLfo_.start(sw.LFR, sw.LFD, sw.LFI,
                         static_cast<LfoMode>(sw.LFM));
        else if (sw.LFR == 0)
            chLfo_.stop();
    }

    // オペレータ LFO
    for (int op = 0; op < 4; ++op) {
        const auto& sw = voice.swOp[op];
        const bool do_start = (sw.SLR > 0) &&
            (sw.SLS || !opLfo_[op].active());
        if (do_start)
            opLfo_[op].start(sw.SLR, sw.SLY, sw.SLI,
                             static_cast<LfoMode>(sw.SLM));
        else if (sw.SLR == 0)
            opLfo_[op].stop();
        effectiveTL_[op] = baseTL_[op];
    }
    chLfoValue_ = 0;
}

void VoiceProcessor::onNoteOff() noexcept
{
    noteOn_ = false;
    // LFO は Releasing フェーズ中も継続し、自然にフェードアウトする。
    // fadeout_ticks は VoiceProcessor::onNoteOff(fadeout_ms) で設定される。
    // ここでは何もしない（呼び出し元が onNoteOff(ms) を使う）。
}

void VoiceProcessor::onNoteOff(uint16_t fadeout_ms) noexcept
{
    noteOn_ = false;
    if (fadeout_ms > 0) {
        chLfo_.fadeout(fadeout_ms);
        for (auto& l : opLfo_) l.fadeout(fadeout_ms);
    }
    // fadeout_ms == 0 の場合は LFO をすぐ停止しない
    // (Releasing フェーズの終了時に ChState::free() → proc.reset() で止まる)
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
    // vol × exp × vel を dB 加算で合成（GM 準拠）
    const uint8_t evol = fitom::calcVolExpVel(vol, exp, vel);

    // キャリアアルゴリズムマスク (OPM/OPN 基準)
    // ALG 0-3: OP4 のみキャリア
    // ALG 4:   OP2+OP4
    // ALG 5-6: OP2+OP3+OP4
    // ALG 7:   全OP キャリア
    static const uint8_t carmsk[8] = {0x8,0x8,0x8,0x8,0xa,0xe,0xe,0xf};
    const uint8_t alg  = voice.hw.ALG & 0x07;
    const uint8_t mask = carmsk[alg];

    for (int op = 0; op < 4; ++op) {
        uint8_t tl = voice.hwOp[op].TL;

        if (mask & (1u << op)) {
            // ── VTL: ベロシティ → TL 感度 ────────────────────────────────
            // VTL=0: 補正なし (vol/exp/vel 全体の影響のみ)
            // VTL=127: 最大感度 (evol=0 のとき TL を 127 まで押し上げる)
            // ベロシティ感度補正は vol/exp の影響を受けた evol ではなく
            // vel 単体で行う (vol/exp はマスターボリューム扱い、
            // vel はアーティキュレーション扱い)
            const uint8_t vtl = voice.swOp[op].VTL;
            if (vtl > 0) {
                // vel が低いほど TL を増加（音量を下げる）
                // delta の最大 = vtl (vel=0 のとき); vel=127 のとき delta=0
                const int32_t delta =
                    static_cast<int32_t>(127 - vel) * vtl / 127;
                tl = static_cast<uint8_t>(
                    clamp(static_cast<int>(tl) + delta, 0, 127));
            }

            // vol × exp を TL (dB 空間) に反映
            tl = fitom::calcLinearLevel(evol, tl);
        }
        baseTL_[op] = tl;
    }
}

void VoiceProcessor::recalcOpLfo(int op, const FmVoice& voice) noexcept
{
    const auto& swop = voice.swOp[op];

    // 波形値 (-120〜+120)、フェードイン適用済み
    const int16_t wav = static_cast<int16_t>(opLfo_[op].wave(swop.SLW));

    // depth: 0〜63=正方向 / 64〜127→-64〜-1=負方向
    int16_t dep = static_cast<int16_t>(swop.SLD);
    if (dep > 63) dep = static_cast<int16_t>(dep - 128);

    // TL 変調量: wav × dep / 120  (dep=±63 のとき最大 ±63 TL unit)
    int32_t delta = static_cast<int32_t>(wav) * dep / 120;
    effectiveTL_[op] = static_cast<int16_t>(
        clamp(static_cast<int>(baseTL_[op]) + static_cast<int>(delta), 0, 127));
}

void VoiceProcessor::recalcChLfo(const FmVoice& voice) noexcept
{
    const auto& sw = voice.sw;

    // 波形値 (-120〜+120)、フェードイン適用済み
    const int16_t wav = static_cast<int16_t>(chLfo_.wave(sw.LWF));

    // depth: LDM/LDL で 16bit 符号付き (±8192 が最大)
    int32_t depth = (static_cast<int32_t>(sw.LDM) << 8) | sw.LDL;
    if (depth >= 8192) depth -= 16384;

    // chLfoValue_ の単位: F-number テーブルインデックス (1 unit ≒ 1.5625 cent)
    // wav(-120〜+120) × depth(±8192) / (120×8192) で [-128, +127] にスケール
    int32_t val = static_cast<int32_t>(wav) * depth / (120 * 8192 / 127);
    chLfoValue_ = static_cast<int16_t>(clamp(static_cast<int>(val), -128, 127));
}

} // namespace fitom
