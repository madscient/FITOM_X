#pragma once
// fitom/VoiceProcessor.h
// ソフトウェアパラメータ処理エンジン
//
// FmSwOp / FmSwVoice を受け取り、MIDI コントローラ値・ベロシティ・
// タイマーティックを入力として、チップドライバに渡す最終 HW パラメータを算出する。
//
// ─── 責務の分離 ──────────────────────────────────────────────────────────────
//
//   チップドライバ (UpdateVoice):
//     FmVoice::hw / FmVoice::hwOp → レジスタへ直接書き込む
//
//   VoiceProcessor:
//     FmVoice::sw / FmVoice::swOp + MIDI 状態 → 実効 HW パラメータを計算
//     計算結果を RuntimeOp::effectiveTL 等として持ち、
//     チップドライバが UpdateVolExp / UpdateOpLFO の代わりに参照する
//
// ─── 呼び出しフロー ──────────────────────────────────────────────────────────
//
//   NoteOn:
//     VoiceProcessor::onNoteOn(velocity)
//       → baseTL[i]  = CalcLinearLevel(evol, hwOp[i].TL)
//       → velocityTL = velocity 感度補正を加算
//       → ソフト LFO の遅延・レートタイマーをリセット
//
//   TimerCallBack (1ms ごと):
//     VoiceProcessor::onTick()
//       → チャンネル LFO カウンタ更新 → UpdateFnumber 要求フラグ
//       → オペレータ LFO カウンタ更新 → effectiveTL を更新
//
//   チップドライバ UpdateVolExp:
//     チップドライバが VoiceProcessor::effectiveTL(op) を読む

#include "fitom/VoiceData.h"
#include <cstdint>
#include <array>

namespace fitom {

// ================================================================
//  LFO モード
// ================================================================
enum class LfoMode : uint8_t {
    Repeat      = 0,  // 繰り返し (通常 LFO)
    OneShotHold = 1,  // 1周期後に末尾値でホールド (ブラスの立ち上がり等)
    OneShotZero = 2,  // 1周期後に 0 にホールド (アタック後に消えるピッチ変化等)
};

// ================================================================
//  LFO コントローラ (全面再実装)
//
//  設計:
//    - delay / 波形位相 / フェードインを完全に分離
//    - rate: 波形の周期 (rateToTicks(rate)、対数カーブで0.5Hz〜50Hzに
//            マッピングする数式。0=無効。docs/voice-data-design.md参照)
//    - fadein: フェードイン速度 (0=即フルデプス, 1〜127 tick でフル到達)
//    - mode: Repeat / OneShotHold / OneShotZero
//    - S&H seed はインスタンス固有 (チャンネル間非共有)
// ================================================================
class LfoControl {
public:
    enum class Phase { Stopped, Delaying, Running, Held, FadingOut };

    // rate:    0=無効, 1〜127 → rateToTicks(rate) tick/周期 (対数カーブ)
    // delay:   0〜127 × 20ms ティック
    // fadein:  0=即フルデプス, 1〜127=フェードイン tick 数
    // mode:    Repeat / OneShotHold / OneShotZero
    void start(uint8_t rate, uint8_t delay_20ms,
               uint8_t fadein, LfoMode mode) noexcept;
    void stop()  noexcept;
    void reset() noexcept;

    // 1 tick 進める。波形更新が必要なら true を返す。
    bool tick() noexcept;

    // NoteOff 時に呼ぶ。fadeout_ticks かけて wave() 出力を 0 に減衰させる。
    // FadingOut 完了後は Stopped に遷移する。
    void fadeout(uint16_t fadeout_ticks) noexcept;

    // 現在の正規化波形値 (-120〜+120)、フェードイン/アウト適用済み
    // waveform: 0=up-saw/1=square/2=triangle/3=S&H/4=down-saw/5=delta/6=sine
    int8_t wave(uint8_t waveform) const noexcept;

    // フェードイン/アウト係数 (0〜127; 127=フル)
    uint8_t fadeLevel() const noexcept { return fade_level_; }

    Phase phase() const noexcept { return phase_; }
    bool  active() const noexcept { return phase_ != Phase::Stopped; }

private:
    Phase    phase_       = Phase::Stopped;
    LfoMode  mode_        = LfoMode::Repeat;

    uint16_t delay_left_  = 0;   // 残りディレイ tick 数
    uint16_t period_      = 0;   // 1周期の tick 数 (rateToTicks() 参照)
    uint16_t phase_tick_  = 0;   // 現在の位相 tick (0〜period_-1)
    bool     one_shot_done_ = false;  // OneShotHold/Zero: 1周期完了フラグ

    uint16_t fadein_len_  = 0;   // フェードイン全体 tick 数 (0=即フル)
    uint16_t fade_tick_   = 0;   // フェードイン経過 tick 数
    uint8_t  fade_level_  = 127; // 現在のフェードイン/アウト係数 (0〜127)

    uint16_t fadeout_len_ = 0;   // フェードアウト全体 tick 数
    uint16_t fadeout_tick_= 0;   // フェードアウト経過 tick 数

    uint16_t seed_        = 0;   // S&H 用シード (インスタンス固有)
};

// ================================================================
//  VoiceProcessor: ソフトパラメータ処理エンジン
//  チャンネル1本分の状態を保持する。
// ================================================================
class VoiceProcessor {
public:
    // ─── 初期化 ───────────────────────────────────────────────────
    void reset() noexcept;

    // ─── NoteOn 時に呼ぶ ──────────────────────────────────────────
    // vol:   MIDIボリューム (0〜127)
    // exp:   MIDIエクスプレッション (0〜127)
    // vel:   MIDIベロシティ (0〜127)
    // voice: 適用するボイスデータ
    void onNoteOn(uint8_t vol, uint8_t exp, uint8_t vel,
                  const FmVoice& voice) noexcept;

    // ─── NoteOff 時に呼ぶ ─────────────────────────────────────────
    void onNoteOff(uint16_t fadeout_ms = 0) noexcept;
    void onNoteOff() noexcept;

    // ─── CC#1 Modulation Wheel ────────────────────────────────────
    // LFR=0 の音色でのみ有効。LFR>0 の音色は CC#1 を無視する。
    // maxDepth: CC#1=127 時のデプス [Fnum steps] (RPN#5 で変更可能)
    void setCC1Modulation(uint8_t cc1, int16_t maxDepth) noexcept;

    // ─── ボリューム/エクスプレッション変更時に呼ぶ ────────────────
    // チャンネルレベルが変化した場合に effectiveTL を再計算する。
    // 戻り値: true = TL が変化したのでチップへの書き込みが必要
    bool onVolumeChange(uint8_t vol, uint8_t exp,
                        const FmVoice& voice) noexcept;

    // ─── タイマーティック (1ms ごと) ─────────────────────────────
    // 戻り値: needsFreqUpdate (チャンネル LFO) | needsTLUpdate (OP LFO)
    struct TickResult {
        bool needsFreqUpdate = false;     // チャンネル LFO: F-number 更新
        uint8_t tlUpdateMask = 0;         // bit[i]=1: op[i] の TL 更新
    };
    TickResult onTick(const FmVoice& voice) noexcept;

    // ─── 実効パラメータ取得 ───────────────────────────────────────
    // チップドライバが UpdateVolExp / UpdateOpLFO の代わりに参照する。

    // 実効 TL (dB 変換前の線形値, 0〜127)
    uint8_t effectiveTL(int op) const noexcept {
        return static_cast<uint8_t>(clamp(effectiveTL_[op], 0, 127));
    }

    // ベロシティ補正済み EG レート getter
    uint8_t velAR (int op) const noexcept { return velAR_[op];  }
    uint8_t velDR (int op) const noexcept { return velDR_[op];  }
    uint8_t velSL (int op) const noexcept { return velSL_[op];  }
    uint8_t velSR (int op) const noexcept { return velSR_[op];  }
    uint8_t velRR (int op) const noexcept { return velRR_[op];  }

    // チャンネル LFO の現在値 (ピッチ変調量, -128〜+127 の整数)
    int16_t channelLfoValue() const noexcept { return chLfoValue_; }

    // チャンネル LFO が有効か
    bool channelLfoActive() const noexcept { return chLfo_.active(); }

private:
    // ─── 内部状態 ─────────────────────────────────────────────────
    LfoControl chLfo_;              // チャンネル LFO (ピッチ変調)
    LfoControl opLfo_[4];           // オペレータ LFO (トレモロ / TL 変調)

    int16_t  baseTL_[4]       = {}; // ベロシティ・ボリューム補正後の基準TL
    int16_t  effectiveTL_[4]  = {}; // LFO 変調後の最終TL

    // CC#1 Modulation Wheel (LFR=0 の音色のみ)
    bool     cc1LfoMode_      = false; // true = CC#1 駆動モード (LFR=0)
    uint8_t  cc1Value_        = 0;     // 現在の CC#1 値 (0-127)
    int16_t  cc1LfoMaxDepth_  = 32;   // CC#1=127 時の最大デプス [steps]

    // ベロシティ補正済み EG レート (onNoteOn 時に設定)
    uint8_t velAR_[4]  = {};   // 補正後 AR  (0-31)
    uint8_t velDR_[4]  = {};   // 補正後 D1R (0-31)
    uint8_t velSL_[4]  = {};   // 補正後 D1L (0-15)
    uint8_t velSR_[4]  = {};   // 補正後 D2R (0-31)
    uint8_t velRR_[4]  = {};   // 補正後 RR  (0-15)
    int16_t  chLfoValue_      = 0;  // チャンネルLFO現在値

    uint8_t  lastVol_  = 127;
    uint8_t  lastExp_  = 127;
    uint8_t  lastVel_  = 100;
    bool     noteOn_   = false;

    // ─── 内部計算 ─────────────────────────────────────────────────
    void recalcBaseTL(uint8_t vol, uint8_t exp, uint8_t vel,
                      const FmVoice& voice) noexcept;
    void recalcOpLfo(int op, const FmVoice& voice) noexcept;
    void recalcChLfo(const FmVoice& voice) noexcept;

    static int clamp(int v, int lo, int hi) noexcept {
        return v < lo ? lo : (v > hi ? hi : v);
    }
};

// ================================================================
//  ベロシティ・ボリューム計算ユーティリティ (旧 SoundDev.cpp の関数群)
//  チップドライバや VoiceProcessor から利用する。
// ================================================================

// vol × exp × vel → 0〜127 の実効レベル
uint8_t calcEffectiveLevel(uint8_t vol, uint8_t exp, uint8_t vel) noexcept;

// 実効レベル × ボイスの TL → 補正後 TL (線形スケール)
uint8_t calcLinearLevel(uint8_t effectiveLevel, uint8_t tl) noexcept;

// 線形 TL → dB TL 変換 (range/step/bw で各チップのスケールに対応)
uint8_t linearToDb(uint8_t linearTL, int range, int step, int bw) noexcept;

// (getLfoWave は LfoControl::wave() に統合済み)

} // namespace fitom
