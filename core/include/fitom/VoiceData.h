#pragma once
// fitom/VoiceData.h
// FM音源ボイスデータ構造体 — ハードウェアパラメータとソフトウェアパラメータの分離
//
// ─── 設計方針 ────────────────────────────────────────────────────────────────
//
//   従来の FMVOICE / FMOP は以下の異なる性質のパラメータが混在していた:
//
//     (A) HW パラメータ  … チップのレジスタに直接書き込む値
//                          チップドライバ (UpdateVoice) が使う
//
//     (B) ベロシティ感度 … NoteOn 時に (A) のパラメータを補正する係数
//                          SoundDev::SetVelocity / UpdateVolExp が使う
//
//     (C) ソフト LFO    … タイマーコールバックで TL を周期変調する
//                          SoundDev::TimerCallBack / UpdateOpLFO が使う
//
//     (D) チャンネル LFO … タイマーコールバックでピッチを周期変調する
//                           SoundDev::TimerCallBack / UpdateFnumber が使う
//
//     (E) チップ拡張    … OPZ/OPP 固有の追加パラメータ (REV, EGS, DM0 等)
//                          特定チップのドライバのみが使う
//
//   新設計:
//     FmHwOp     … (A) オペレータ HW パラメータのみ。チップドライバが直接参照。
//     FmHwVoice  … (A) チャンネル HW パラメータのみ。
//     FmSwOp     … (B)(C) オペレータ単位のソフト処理パラメータ。
//     FmSwVoice  … (D) チャンネル単位のソフト LFO パラメータ。
//     FmChipExt  … (E) チップ固有拡張パラメータ。
//     FmVoice    … 上記をまとめたトップレベル構造体。
//
//   後方互換:
//     旧 FMVOICE との互換レイアウトは保証しない。
//     既存バンクファイルの読み込みは FITOMCfg の変換関数で吸収する。
//
// ─── チップ別 HW パラメータ対応表 ────────────────────────────────────────────
//
//   フィールド  | OPN/OPNA | OPM/OPP | OPL/OPL2 | OPL3 | OPLL
//   ------------|----------|---------|----------|------|-----
//   AR (5bit)   |    ○    |    ○   |  ○(4bit)|  ○  |  ○
//   DR (5bit)   |    ○    |    ○   |  ○(4bit)|  ○  |  ○
//   SL (4bit)   |    ○    |    ○   |    ○    |  ○  |  ○
//   SR (5bit)   |    ○    |    ○   |  ×(1bit)|  ○  |  ×
//   RR (4bit)   |    ○    |    ○   |    ○    |  ○  |  ○
//   TL (7bit)   |    ○    |    ○   |  ○(6bit)|  ○  |  ○
//   KSR (2bit)  |    ○    |    ○   |  ×(1bit)|  ×  |  ×
//   KSL (2bit)  |    ×    |    ×   |    ○    |  ○  |  ×
//   MUL (4bit)  |    ○    |    ○   |    ○    |  ○  |  ○
//   DT1 (3bit)  |    ○    |    ○   |    ×    |  ×  |  ×
//   DT2 (2bit)  |    ×    |    ○   |    ×    |  ×  |  ×
//   AM  (1bit)  |    ○    |    ○   |    ○    |  ○  |  ×
//   VIB (1bit)  |    ×    |    ×   |    ○    |  ○  |  ×
//   EGT (4bit)  |    ○    |    ×   |    ×    |  ×  |  ×
//   WS  (3bit)  |    ×    |  ○(OPZ)|  ○(2bit)|○(3bit)|○(1bit)
//   FB  (3bit)  |    ○    |    ○   |    ○    |  ○  |  ○
//   ALG (3bit)  |    ○    |    ○   |  ×(1bit)|×(4bit)|×(1bit)
//   AMS (2bit)  |    ×    |    ○   |    ×    |  ×  |  ×
//   PMS (3bit)  |    ×    |    ○   |    ×    |  ×  |  ×
//   NFQ (5bit)  |    ×    |  ○(5bit)|   ×    |  ×  |  ×

#include <cstdint>
#include <cstring>
#include <string>
#include <array>

namespace fitom {

// ================================================================
//  HW パラメータ — チップレジスタへ直接書き込む値
// ================================================================

#pragma pack(push, 1)

// ----------------------------------------------------------------
//  FmHwOp: オペレータ HW パラメータ
//  全チップ共通の上位ビット表現で保持する。
//  チップドライバが GET_AR 等のマクロで必要なビット幅に切り出す。
// ----------------------------------------------------------------
struct FmHwOp {
    // ─── エンベロープジェネレータ ─────────────────────────────────
    uint8_t AR;   // Attack Rate:    5bit (OPN/OPM: そのまま / OPL: 上位4bit使用)
    uint8_t DR;   // Decay Rate:     5bit
    uint8_t SL;   // Sustain Level:  4bit
    uint8_t SR;   // Sustain Rate:   5bit (OPL は未使用 / OPLL は1bit)
    uint8_t RR;   // Release Rate:   4bit
    uint8_t TL;   // Total Level:    7bit (OPL は6bit / 上位1bit無視)

    // ─── キースケール ─────────────────────────────────────────────
    uint8_t KSR;  // Key Scale Rate:  2bit (OPL は1bit)
    uint8_t KSL;  // Key Scale Level: 2bit (OPL のみ / OPN/OPM: 0固定)

    // ─── 発振器 ───────────────────────────────────────────────────
    uint8_t MUL;  // Multiple:  4bit
    uint8_t DT1;  // Detune 1:  3bit (OPN/OPM のみ / OPL: 0固定)
    uint8_t DT2;  // Detune 2:  2bit (OPM/OPZ のみ)

    // ─── LFO (HW) / AM・VIB ──────────────────────────────────────
    uint8_t AM;   // AM enable:   1bit
    uint8_t VIB;  // VIB enable:  1bit (OPL のみ)

    // ─── EG 拡張 ─────────────────────────────────────────────────
    uint8_t EGT;  // SSG-EG type: 4bit (OPN/OPNA のみ / 他: 0固定)
    uint8_t WS;   // Wave Select: 3bit (OPL2: 2bit / OPL3/OPZ: 3bit / OPM: 0固定)

    constexpr FmHwOp() noexcept
        : AR(31), DR(0), SL(0), SR(0), RR(7), TL(0)
        , KSR(0), KSL(0), MUL(1), DT1(0), DT2(0)
        , AM(0), VIB(0), EGT(0), WS(0) {}
};

// ----------------------------------------------------------------
//  FmHwVoice: チャンネル HW パラメータ
// ----------------------------------------------------------------
struct FmHwVoice {
    uint8_t FB;   // Feedback:    3bit
    uint8_t ALG;  // Algorithm:   3bit (OPN/OPM: 3bit / OPL: 1bit / OPL3: 4bit)
    uint8_t AMS;  // AM Sensitivity:  2bit (OPM のみ / 他: 0固定)
    uint8_t PMS;  // PM Sensitivity:  3bit (OPM のみ / 他: 0固定)
    uint8_t NFQ;  // Noise Frequency: 5bit (OPM/OPZ: ノイズ周波数 / 他: 0固定)

    constexpr FmHwVoice() noexcept
        : FB(0), ALG(0), AMS(0), PMS(0), NFQ(0) {}
};

// ================================================================
//  チップ固有拡張パラメータ
//  特定チップのドライバのみが参照する。不要なチップでは全フィールド0。
// ================================================================
struct FmChipExt {
    // OPZ (YM2414) 固有
    uint8_t REV;  // Reverberation: 4bit (OPZ のみ)
    uint8_t EGS;  // EG bias:       7bit (OPZ のみ)
    uint8_t DM0;  // Osc fixed freq flag: 1bit (OPZ/OPP ノイズ OR フラグ)
    uint8_t DT3;  // Fine frequency: 4bit OPZ ratio mode

    // OPZ: 2OP 拡張アルゴリズムフラグ (AL の bit3)
    uint8_t ALG_EXT;  // ALG 拡張ビット (OPN FX mode / OPM noise / OPZ 2OP 拡張)

    // AY-3-8910 / YM2149 (PSG) 固有
    // HW Envelope Period: レジスタ 0x0B(Fine)+0x0C(Coarse) に対応する
    // 16bit 値をそのまま指定する。実機は Fine/Coarse の2レジスタに
    // 分割されているが、意味的には不可分の単一の16bit周期値であるため
    // ここでは1つの16bitフィールドとして保持する。
    uint16_t HWEP;

    constexpr FmChipExt() noexcept
        : REV(0), EGS(0), DM0(0), DT3(0), ALG_EXT(0), HWEP(0) {}
};

// ================================================================
//  SW パラメータ — FITOM 内部でソフトウェア処理するもの
// ================================================================

// ----------------------------------------------------------------
//  FmSwOp: オペレータ単位のソフトウェア処理パラメータ
// ----------------------------------------------------------------
struct FmSwOp {
    // ─── ベロシティ感度 ───────────────────────────────────────────
    // NoteOn 時に HW パラメータを動的補正する係数 (0=無感度)
    uint8_t VTL;  // Velocity → TL 感度:  0〜127 (大きいほど感度高)
    uint8_t VAR;  // Velocity → AR 感度
    uint8_t VDR;  // Velocity → DR 感度
    uint8_t VSL;  // Velocity → SL 感度
    uint8_t VSR;  // Velocity → SR 感度
    uint8_t VRR;  // Velocity → RR 感度
    uint8_t VLD;  // Velocity → ソフト LFO Depth 感度 (未実装・予約)
    uint8_t VLR;  // Velocity → ソフト LFO FadeIn 感度 (未実装・予約)

    // ─── オペレータ単位ソフト LFO (Tremolo / TL 変調) ───────────
    // TL を周期変調する。1ms ティックごとに処理する。
    uint8_t SLW;  // Soft LFO waveform: 0=up-saw/1=square/2=triangle
                  //                    3=S&H/4=down-saw/5=delta/6=sine
    uint8_t SLS;  // Soft LFO sync:  0=NoteOnでリセットしない / 1=リセット
    uint8_t SLM;  // Soft LFO mode:  0=repeat / 1=one-shot(hold) / 2=one-shot(zero)
    uint8_t SLD;  // Soft LFO depth: 0〜63=正方向 / 64〜127→-64〜-1=負方向
    uint8_t SLY;  // Soft LFO delay: 0〜127 [20ms 単位]
    uint8_t SLR;  // Soft LFO rate:  0〜127 (0=LFO 無効, kSpeedStep参照)
    uint8_t SLI;  // Soft LFO fade-in: 0=即フルデプス / 1〜127=フェードイン速度

    constexpr FmSwOp() noexcept
        : VTL(0), VAR(0), VDR(0), VSL(0), VSR(0), VRR(0), VLD(0), VLR(0)
        , SLW(0), SLS(0), SLM(0), SLD(0), SLY(0), SLR(0), SLI(0) {}
};

// ----------------------------------------------------------------
//  FmSwVoice: チャンネル単位のソフトウェア処理パラメータ
// ----------------------------------------------------------------
struct FmSwVoice {
    // ─── チャンネルソフト LFO (Vibrato / Pitch Modulation) ───────
    // F-number インデックスを周期変調する (1unit ≒ 1.5625 cent)。
    // HW LFO はパフォーマンスパラメータ扱いのため、ここには含まない。
    uint8_t LWF;  // LFO waveform: 0=up-saw/1=square/2=triangle
                  //               3=S&H/4=down-saw/5=delta/6=sine
    uint8_t LFS;  // LFO sync: 0=NoteOnでリセットしない / 1=リセット
    uint8_t LFM;  // LFO mode: 0=repeat / 1=one-shot(hold) / 2=one-shot(zero)
    uint8_t LFD;  // LFO delay: 0〜127 [20ms 単位]
    uint8_t LFR;  // LFO rate:  0〜127 (0=LFO 無効, kSpeedStep参照)
    uint8_t LFI;  // LFO fade-in: 0=即フルデプス / 1〜127=フェードイン速度
    uint8_t LDM;  // LFO depth (MSB): 0〜8191=正方向 / 8192〜16383→-8192〜-1
    uint8_t LDL;  // LFO depth (LSB)

    constexpr FmSwVoice() noexcept
        : LWF(0), LFS(0), LFM(0), LFD(0), LFR(0), LFI(0), LDM(0), LDL(0) {}
};

#pragma pack(pop)

// ================================================================
//  FmVoice: トップレベルボイス構造体
// ================================================================
struct FmVoice {
    // ─── メタ情報 ─────────────────────────────────────────────────
    uint32_t    id;           // ボイス固有 ID (バンク内一意)
    char        name[32];     // ボイス名 (UTF-8、null 終端)

    // ─── HW パラメータ ────────────────────────────────────────────
    FmHwVoice   hw;           // チャンネル HW パラメータ
    FmHwOp      hwOp[4];      // オペレータ HW パラメータ [op0..op3]

    // ─── SW パラメータ ────────────────────────────────────────────
    FmSwVoice   sw;           // チャンネルソフト LFO
    FmSwOp      swOp[4];      // オペレータ単位ソフトパラメータ [op0..op3]

    // ─── チップ固有拡張 ───────────────────────────────────────────
    FmChipExt   ext;          // OPZ 等の拡張パラメータ (不使用なら全0)

    FmVoice() noexcept : id(0xFFFFFFFFu) {
        name[0] = '\0';
    }

    bool isValid() const noexcept { return id != 0xFFFFFFFFu; }
};

// ================================================================
//  後方互換: 旧 FMOP / FMVOICE との変換ヘルパー
//  FITOMCfg の既存バンク読み込みコードが使用する。
//  チップドライバは FmVoice を使い、旧 FMVOICE は参照しない。
// ================================================================
namespace legacy {

// 旧 FMOP 構造体（バンクファイル読み込み専用・新規使用禁止）
#pragma pack(push, 1)
struct FMOP {
    uint8_t AR, DR, SL, SR, RR, REV, TL, EGT, EGS, KSL, KSR, WS;
    uint8_t AM, VIB, SLW, SLD, SLY, SLR;     // OP LFO
    uint8_t SLS, SLM, SLI;                   // OP LFO sync/mode/fadein (新規)
    uint8_t DM0, MUL, DT1, DT2, DT3;
    uint8_t VTL, VAR, VDR, VSL, VSR, VRR, VLD, VLR;
};

struct FMVOICE {
    uint32_t ID;
    char     name[32];
    uint8_t  FB, AL, AMS, PMS;
    uint8_t  LDM, LDL, LWF, LFS, LFD, LFR, NFQ;  // ch LFO
    uint8_t  LFM, LFI;                               // ch LFO mode/fadein (新規)
    FMOP     op[4];
};
#pragma pack(pop)

// 変換関数: 旧 FMVOICE → 新 FmVoice
inline FmVoice fromLegacy(const FMVOICE& src) noexcept {
    FmVoice dst;
    dst.id = src.ID;
    std::strncpy(dst.name, src.name, sizeof(dst.name) - 1);
    dst.name[sizeof(dst.name) - 1] = '\0';

    // HW (チャンネル)
    dst.hw.FB   = src.FB;
    dst.hw.ALG  = src.AL & 0x07;     // bit3 は ALG_EXT へ
    dst.hw.AMS  = src.AMS;
    dst.hw.PMS  = src.PMS;
    dst.hw.NFQ  = src.NFQ;

    // SW (チャンネル LFO)
    dst.sw.LDM  = src.LDM;
    dst.sw.LDL  = src.LDL;
    dst.sw.LWF  = src.LWF;
    dst.sw.LFS  = src.LFS;
    dst.sw.LFM  = src.LFM;
    dst.sw.LFD  = src.LFD;
    dst.sw.LFR  = src.LFR;
    dst.sw.LFI  = src.LFI;

    // チップ拡張
    dst.ext.ALG_EXT = (src.AL >> 3) & 0x01;

    for (int i = 0; i < 4; ++i) {
        const auto& s = src.op[i];

        // HW (オペレータ)
        auto& h  = dst.hwOp[i];
        h.AR  = s.AR;   h.DR  = s.DR;   h.SL  = s.SL;
        h.SR  = s.SR;   h.RR  = s.RR;   h.TL  = s.TL;
        h.KSR = s.KSR;  h.KSL = s.KSL;
        h.MUL = s.MUL;  h.DT1 = s.DT1;  h.DT2 = s.DT2;
        h.AM  = s.AM;   h.VIB = s.VIB;
        h.EGT = s.EGT;  h.WS  = s.WS;

        // チップ拡張 (オペレータ単位)
        dst.ext.REV  = s.REV;  // OP0 で代表（OPZ は全OP共通）
        dst.ext.EGS  = s.EGS;
        dst.ext.DM0  = s.DM0;
        dst.ext.DT3  = s.DT3;

        // SW (オペレータ)
        auto& w  = dst.swOp[i];
        w.VTL = s.VTL;  w.VAR = s.VAR;  w.VDR = s.VDR;
        w.VSL = s.VSL;  w.VSR = s.VSR;  w.VRR = s.VRR;
        w.VLD = s.VLD;  w.VLR = s.VLR;
        w.SLW = s.SLW;  w.SLS = s.SLS;  w.SLM = s.SLM;  w.SLI = s.SLI;
        w.SLD = s.SLD;  w.SLY = s.SLY;  w.SLR = s.SLR;
    }
    return dst;
}

// 変換関数: 新 FmVoice → 旧 FMVOICE (GUI 後方互換用)
inline FMVOICE toLegacy(const FmVoice& src) noexcept {
    FMVOICE dst{};
    dst.ID = src.id;
    std::strncpy(dst.name, src.name, sizeof(dst.name) - 1);

    dst.FB  = src.hw.FB;
    dst.AL  = src.hw.ALG | (src.ext.ALG_EXT << 3);
    dst.AMS = src.hw.AMS;
    dst.PMS = src.hw.PMS;
    dst.NFQ = src.hw.NFQ;
    dst.LDM = src.sw.LDM;  dst.LDL = src.sw.LDL;
    dst.LWF = src.sw.LWF;  // LFO フィールド廃止 (HW LFO はパフォーマンスパラメータへ)
    dst.LFS = src.sw.LFS;  dst.LFD = src.sw.LFD;
    dst.LFR = src.sw.LFR;

    for (int i = 0; i < 4; ++i) {
        auto& d       = dst.op[i];
        const auto& h = src.hwOp[i];
        const auto& w = src.swOp[i];

        d.AR  = h.AR;   d.DR  = h.DR;   d.SL  = h.SL;
        d.SR  = h.SR;   d.RR  = h.RR;   d.TL  = h.TL;
        d.KSR = h.KSR;  d.KSL = h.KSL;
        d.MUL = h.MUL;  d.DT1 = h.DT1;  d.DT2 = h.DT2;
        d.AM  = h.AM;   d.VIB = h.VIB;
        d.EGT = h.EGT;  d.WS  = h.WS;

        d.REV = src.ext.REV;
        d.EGS = src.ext.EGS;
        d.DM0 = src.ext.DM0;
        d.DT3 = src.ext.DT3;

        d.VTL = w.VTL;  d.VAR = w.VAR;  d.VDR = w.VDR;
        d.VSL = w.VSL;  d.VSR = w.VSR;  d.VRR = w.VRR;
        d.VLD = w.VLD;  d.VLR = w.VLR;
        d.SLW = w.SLW;  d.SLS = w.SLS;  d.SLM = w.SLM;
        d.SLD = w.SLD;  d.SLY = w.SLY;  d.SLR = w.SLR;  d.SLI = w.SLI;
    }
    return dst;
}

} // namespace legacy

// ================================================================
//  チップドライバ用ヘルパーマクロ
//  UpdateVoice 内で FmVoice から各チップのビット幅に切り出す。
//  旧 GET_AR 等を置き換える。
// ================================================================

// OPN / OPM 系: 5bit → そのまま (OPM は上位3bit使用 = >> 2)
#define FV_AR_OPN(v, o)   ((v).hwOp[o].AR)
#define FV_DR_OPN(v, o)   ((v).hwOp[o].DR)
#define FV_SR_OPN(v, o)   ((v).hwOp[o].SR)
#define FV_RR_OPN(v, o)   ((v).hwOp[o].RR)
#define FV_SL_OPN(v, o)   ((v).hwOp[o].SL)
#define FV_TL_OPN(v, o)   ((v).hwOp[o].TL)

// OPL 系: AR/DR は上位4bit
#define FV_AR_OPL(v, o)   ((v).hwOp[o].AR >> 1)
#define FV_DR_OPL(v, o)   ((v).hwOp[o].DR >> 1)
#define FV_RR_OPL(v, o)   ((v).hwOp[o].RR)
#define FV_SL_OPL(v, o)   ((v).hwOp[o].SL)
#define FV_TL_OPL(v, o)   ((v).hwOp[o].TL >> 1)   // 6bit

// OPM 系: >> 2 でハードウェアの 5bit値に
#define FV_AR_OPM(v, o)   ((v).hwOp[o].AR >> 2)
#define FV_DR_OPM(v, o)   ((v).hwOp[o].DR >> 2)
#define FV_SR_OPM(v, o)   ((v).hwOp[o].SR >> 2)
#define FV_RR_OPM(v, o)   ((v).hwOp[o].RR >> 3)
#define FV_SL_OPM(v, o)   ((v).hwOp[o].SL >> 3)
#define FV_TL_OPM(v, o)   ((v).hwOp[o].TL)

} // namespace fitom
