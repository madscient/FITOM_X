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
    uint8_t SR;   // Sustain Rate: 5bit。SR>0でパーカッシブモード
                  // (実機EGTビット=0、キーオン中もこのレートで減衰
                  // し続ける)、SR==0でサステインモード(実機EGTビット=1、
                  // 通常のADSR。RRはキーオフ時にのみ適用)を表す。
                  // OPL/OPL2/OPL3/OPLL共通: パーカッシブモード時、
                  // 実機RRレジスタにはRRの代わりにSRの値(4bit変換)を
                  // 書く。OPLLはこの反映を updateVoice(初期値、常にRR)
                  // ではなく updateKey(キーオン/キーオフ毎に動的に
                  // 再書き込み)で行う点がOPL系と異なるが、最終的な
                  // 変換規則自体は共通(2026年7月に確認・訂正)。詳細は
                  // docs/voice-parameter-reference.md参照。
    uint8_t RR;   // Release Rate:   4bit
    uint8_t TL;   // Total Level:    7bit (OPL は6bit / 上位1bit無視)

    // ─── キースケール ─────────────────────────────────────────────
    uint8_t KSR;  // Key Scale Rate:  2bit (OPL は1bit)
    uint8_t KSL;  // Key Scale Level: 2bit (OPL のみ / OPN/OPM: 0固定)

    // ─── 発振器 ───────────────────────────────────────────────────
    uint8_t MUL;  // Multiple:  4bit
    uint8_t DT1;  // Detune 1:  3bit (OPN/OPM のみ / OPL: 0固定)
    // Detune 2: OPM/OPZ (HW): 2bit (0-3)。
    // OPN/OPL3(4OPモード)では未使用のため、疑似デチューン値として転用する。
    // op[0]/op[2] (各2OPペアの先頭オペレータ) の値を、符号付き8bit
    // (int8_t、-128〜127、単位=100/64セント) として再解釈して使う。
    // フィールド幅は変更せず、解釈のみで対応する。0=デチューンなし。
    uint8_t DT2;

    // OPN ch2 FXモード (3rd channel special mode) 専用パラメータ。
    // ext.DM0 (チャンネル単位、0=通常/1=疑似デチューン/2=非整数倍率/3=固定周波数)
    // でモードを選択し、本フィールドの解釈が変わる。オペレータ単位。
    //   モード1/2: 100/64セント単位の符号付きオフセット (getFnumber(ch,FXV))
    //   モード3  : 0.1Hz単位の絶対周波数 (getFnumberFromHz(FXV/10.0))
    // OPN以外・ch2以外では無視される。0=無効(通常のch2共有Fnumberを使用)。
    int16_t FXV;

    // ─── AM・VIB ──────────────────────────────────────────────────
    uint8_t AM;   // AM enable:   1bit
    uint8_t VIB;  // VIB enable:  1bit (OPL のみ)

    // ─── EG 拡張 ─────────────────────────────────────────────────
    uint8_t EGT;  // SSG-EG type: 4bit (OPN: SSG-EGタイプ0-7としてそのまま
                  // 使用 / SSG: bit3=HWエンベロープ使用フラグ、下位4bit=
                  // 波形シェイプとしてビットフィールド的に共用 / 他: 0固定)
    uint8_t WS;   // Wave Select: 3bit (OPL2: 2bit / OPL3/OPZ: 3bit / OPM: 0固定)

    // ─── OPZ (YM2414) 固有、オペレータ単位 ─────────────────────────
    // 2026年7月、FmChipExt(チャンネル単位)から移設。実機OPZは各オペレータ
    // に個別のREV/EGS/DT3を設定できるため、チャンネル単位の共通値に
    // 強制すると表現力が失われる(旧実装は4オペレータの値を1つに
    // 上書きして潰していた、既存の不具合だった)。
    uint8_t REV;  // Reverberation: 4bit (OPZ のみ)
    uint8_t EGS;  // EG bias:       7bit (OPZ のみ)
    uint8_t DT3;  // Fine frequency: 4bit OPZ ratio mode (OPZ のみ)

    constexpr FmHwOp() noexcept
        : AR(31), DR(0), SL(0), SR(0), RR(7), TL(0)
        , KSR(0), KSL(0), MUL(1), DT1(0), DT2(0), FXV(0)
        , AM(0), VIB(0), EGT(0), WS(0)
        , REV(0), EGS(0), DT3(0) {}
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
    // OPL3(COPL3) 4OPモード専用: 後半ペア(M2/C2)独立のフィードバック値 (3bit)。
    // 実機OPL3は前半・後半ペアそれぞれ独立したFBレジスタを持つため、
    // FBを前半ペア用のみに限定せず、後半ペア用に別フィールドを設ける。
    // OPL3以外のチップでは未使用 (0固定)。
    uint8_t FB2;

    constexpr FmHwVoice() noexcept
        : FB(0), ALG(0), AMS(0), PMS(0), NFQ(0), FB2(0) {}
};

// ================================================================
//  チップ固有拡張パラメータ
//  特定チップのドライバのみが参照する。不要なチップでは全フィールド0。
// ================================================================
struct FmChipExt {
    // OPZ (YM2414) / OPN (ch2 FXモード) 共用。REV/EGS/DT3は2026年7月に
    // FmHwOp(オペレータ単位)へ移設した(この4番目のフィールドのみ
    // チャンネル単位で正しいため、こちらは残す)。
    // OPZ: Osc fixed freq flag (ノイズORフラグ、1bit)。
    // OPN: ch2 FXモード種別 (0=通常/1=疑似デチューン/2=非整数倍率/
    //      3=固定周波数)。
    uint8_t DM0;

    // OPZ: 2OP 拡張アルゴリズムフラグ (AL の bit3)
    uint8_t ALG_EXT;  // ALG 拡張ビット (OPM noise / OPLL preset選択)

    // AY-3-8910 / YM2149 (PSG) 固有
    // HW Envelope Period: レジスタ 0x0B(Fine)+0x0C(Coarse) に対応する
    // 16bit 値をそのまま指定する。実機は Fine/Coarse の2レジスタに
    // 分割されているが、意味的には不可分の単一の16bit周期値であるため
    // ここでは1つの16bitフィールドとして保持する。
    uint16_t HWEP;

    constexpr FmChipExt() noexcept
        : DM0(0), ALG_EXT(0), HWEP(0) {}
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
    uint8_t SLR;  // Soft LFO rate:  0〜127 (0=LFO無効。1-127は対数カーブで0.5Hz〜50Hzに変換、rateToTicks()参照)
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
    uint8_t LFR;  // LFO rate:  0〜127 (0=LFO無効。1-127は対数カーブで0.5Hz〜50Hzに変換、rateToTicks()参照)
    uint8_t LFI;  // LFO fade-in: 0=即フルデプス / 1〜127=フェードイン速度

    // LFO深さ [セント、-1200〜+1200]。旧実装はLDM/LDL(16bit値を2バイトに
    // 分割)だったが、これはMIDI CCによる制御を想定した設計だった。
    // 新実装ではパラメータのMIDI CC制御可否は未定(コア実装確定後に別途
    // 検討)のため、1フィールドに統合し、直感的なセント単位で直接指定
    // する形に変更した(2026年7月)。ピッチLFOがドラム音の装飾に使われる
    // ケースを考慮し、±200セント(旧LDM/LDLの実質上限)より広い、
    // ±1オクターブ(±1200セント)までのレンジを確保している。
    int16_t depthCents = 0;

    constexpr FmSwVoice() noexcept
        : LWF(0), LFS(0), LFM(0), LFD(0), LFR(0), LFI(0), depthCents(0) {}
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
    // 旧LDM/LDL(16bit分割、kfs相当)を新しいdepthCents(セント単位、
    // 1フィールド)に変換する。新レンジ(±1200セント)は旧の実質上限
    // (±8192kfs≈±200セント相当だった想定)より大幅に広いため、
    // クランプは実質的に効かない。
    {
        int16_t rawDepth = static_cast<int16_t>((static_cast<uint16_t>(src.LDM) << 8) | src.LDL);
        if (rawDepth >= 8192) rawDepth -= 16384;
        int cents = rawDepth * 100 / 64;
        dst.sw.depthCents = static_cast<int16_t>(cents < -1200 ? -1200 : (cents > 1200 ? 1200 : cents));
    }
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

        // チップ拡張 (オペレータ単位、2026年7月にFmHwOpへ移設)
        h.REV = s.REV;
        h.EGS = s.EGS;
        h.DT3 = s.DT3;
        // DM0のみチャンネル単位のまま(ext.DM0)。旧形式もOP単位で
        // DM0を持っていたが、意味的にチャンネル共通の値のため
        // op[0]の値を代表として使う。
        if (i == 0) dst.ext.DM0 = s.DM0;

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
    {
        int kfs = static_cast<int>(src.sw.depthCents) * 64 / 100;
        kfs = kfs < -8192 ? -8192 : (kfs > 8191 ? 8191 : kfs);
        uint16_t raw16 = static_cast<uint16_t>(kfs < 0 ? kfs + 16384 : kfs);
        dst.LDM = static_cast<uint8_t>(raw16 >> 8);
        dst.LDL = static_cast<uint8_t>(raw16 & 0xFF);
    }
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

        d.REV = h.REV;
        d.EGS = h.EGS;
        d.DM0 = src.ext.DM0;
        d.DT3 = h.DT3;

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
