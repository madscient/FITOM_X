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
//  LFO コントローラ
//  旧 CSoundDevice::CLFOControl を独立クラスとして再実装
// ================================================================
class LfoControl {
public:
    enum class Phase { Delaying, Running, Stopped };

    void start(uint8_t delay_20ms, uint8_t rate) noexcept;
    void stop()  noexcept;
    void reset() noexcept;

    // 1 ティック進める。TL 更新が必要な場合 true を返す。
    bool tick() noexcept;

    uint8_t  level()  const noexcept { return static_cast<uint8_t>(level_); }
    uint16_t count()  const noexcept { return count_; }
    Phase    phase()  const noexcept { return phase_; }

private:
    Phase    phase_  = Phase::Stopped;
    uint16_t count_  = 0;
    uint16_t delay_  = 0;
    uint16_t rate_   = 0;
    int16_t  level_  = 0;
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
    void onNoteOff() noexcept;

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

    // チャンネル LFO の現在値 (ピッチ変調量, -128〜+127 の整数)
    int16_t channelLfoValue() const noexcept { return chLfoValue_; }

    // チャンネル LFO が有効か
    bool channelLfoActive() const noexcept {
        return chLfo_.phase() != LfoControl::Phase::Stopped;
    }

private:
    // ─── 内部状態 ─────────────────────────────────────────────────
    LfoControl chLfo_;              // チャンネルLFO (ピッチ変調)
    LfoControl opLfo_[4];           // オペレータLFO (トレモロ)

    int16_t  baseTL_[4]       = {}; // ベロシティ・ボリューム補正後の基準TL
    int16_t  effectiveTL_[4]  = {}; // LFO 変調後の最終TL
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

// ソフト LFO 波形値を取得 (-120〜+120)
int8_t getLfoWave(uint8_t waveform, uint8_t speed, uint16_t phase) noexcept;

} // namespace fitom
