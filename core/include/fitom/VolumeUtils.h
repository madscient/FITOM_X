#pragma once
// fitom/VolumeUtils.h
// 音量・dB 変換ユーティリティ
//
// 旧 EnvLFO.cpp のグローバル関数を fitom 名前空間に収めた版。
// インターフェース互換を保ちつつ extern → inline で変換コストを削減する。
//
// ─── 定数 ───────────────────────────────────────────────────────────────────
//   RANGE96DB  : 127段階で -96dB (OPM / OPN TL 7bit)
//   RANGE48DB  : 127段階で -48dB (OPL TL 6bit)
//   RANGE24DB  : 127段階で -24dB
//   STEP075DB  : 0.75dB ステップ
//   STEP150DB  : 1.50dB ステップ
//   STEP300DB  : 3.00dB ステップ

#include <cstdint>
#include <cmath>
#include <algorithm>

// 後方互換: 旧コードからそのまま使えるようにマクロも定義する
#define RANGE96DB 0
#define RANGE48DB 1
#define RANGE24DB 2
#define RANGE12DB 3

#define STEP075DB 0x7F
#define STEP150DB 0x3F
#define STEP300DB 0x1F

namespace fitom {

// ================================================================
//  GM dB テーブル (volume_manager を使わない版)
//  MIDI volume 0-127 → dB 変換。ROM::GM2dB と同一データ。
// ================================================================
extern const double kGM2dB[128];

// ================================================================
//  音量カーブテーブル (0.75dB ステップ; VolCurveLin と同一)
// ================================================================
extern const uint8_t kVolCurveLin[128];
extern const uint8_t kVolCurveInv[128];

// ================================================================
//  CalcVolExpVel: vol × exp × vel → 実効レベル (0〜127)
//  GM ダイナミクス対応（log スケール加算）
// ================================================================
inline uint8_t calcVolExpVel(uint8_t vol, uint8_t exp, uint8_t vel) noexcept
{
    double dvol = kGM2dB[vol];
    double dexp = kGM2dB[exp];
    double dvel = kGM2dB[vel];
    double deff = 127.0 * std::pow(10.0, (dvol + dexp + dvel) / 40.0);
    int evol = static_cast<int>(std::round(deff));
    return static_cast<uint8_t>(std::clamp(evol, 0, 127));
}

// ================================================================
//  CalcLinearLevel: 実効レベル × TL → 補正後レベル
//  vev: 0(min)〜127(max)  tl: 減衰量(0=最大, 127=最小)
//  戻り値: 0(min)〜127(max)
// ================================================================
inline uint8_t calcLinearLevel(uint8_t vev, uint8_t tl) noexcept
{
    double dvev = kGM2dB[vev];
    double dtl  = static_cast<double>(tl) * -0.75;
    double deff = 127.0 * std::pow(10.0, (dvev + dtl) / 40.0);
    int evol = static_cast<int>(std::round(deff));
    return static_cast<uint8_t>(std::clamp(evol, 0, 127));
}

// ================================================================
//  Linear2dB: 線形レベル → チップ固有 dB TL 値
//  evol : 0(min)〜127(max) の線形レベル
//  range: RANGE96DB / RANGE48DB / RANGE24DB
//  step : STEP075DB / STEP150DB / STEP300DB (ビットマスク)
//  bw   : チップのビット幅 (7=7bit, 6=6bit, 4=4bit)
//  戻り値: チップレジスタに書く TL 値
// ================================================================
inline uint8_t linear2dB(uint8_t evol, int range, int step, int bw) noexcept
{
    evol = evol & static_cast<uint8_t>(step);
    uint8_t lim = static_cast<uint8_t>(127 >> range);
    uint8_t ret;
    if (evol == 0) {
        ret = lim;
    } else {
        double db  = kGM2dB[evol];
        int    val = static_cast<int>(std::round(db / -0.75));
        ret = static_cast<uint8_t>(std::min(val, static_cast<int>(lim - 1)));
    }
    ret >>= (7 - range - bw);
    return ret;
}

} // namespace fitom

// ================================================================
//  後方互換: グローバル関数シム (旧 EnvLFO.cpp と同じシグネチャ)
//  チップドライバが #include "VolumeUtils.h" するだけで動くよう
//  旧名の extern 宣言を using 宣言で転送する
// ================================================================
inline uint8_t CalcVolExpVel(int vol, int exp, int vel) noexcept {
    return fitom::calcVolExpVel(
        static_cast<uint8_t>(std::clamp(vol, 0, 127)),
        static_cast<uint8_t>(std::clamp(exp, 0, 127)),
        static_cast<uint8_t>(std::clamp(vel, 0, 127)));
}
inline uint8_t CalcLinearLevel(uint8_t vev, uint8_t tl) noexcept {
    return fitom::calcLinearLevel(vev, tl);
}
inline uint8_t Linear2dB(uint8_t evol, int range, int step, int bw) noexcept {
    return fitom::linear2dB(evol, range, step, bw);
}

namespace ROM {
    extern const uint8_t VolCurveLin[]; // = fitom::kVolCurveLin
    extern const uint8_t VolCurveInv[]; // = fitom::kVolCurveInv
}
