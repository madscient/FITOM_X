// fitom/OPLL_new.cpp
// COPLL / COPLL2 / COPLLP チップドライバ — ISoundDevice ベース移行版
//
// OPLL の特徴:
//   - 9ch / 2OP
//   - ユーザー音色 (reg 0x00-0x07) またはプリセット音色 (reg 0x30 bit7-4)
//   - ALG & 0x40 でプリセット選択フラグ (HwPatch では ext.ALG_EXT として管理)
//   - ボリュームはレジスタ 0x30 の下位 4bit (4bit = 16段階)
//   - リズムモード: ch 6-8 はリズムとして使用 (有効/無効を enableCh で管理)

#include "fitom/ISoundDevice.h"
#include "fitom/FITOMdefine.h"
#include "fitom/VolumeUtils.h"
#include "fitom/Log.h"
#include <algorithm>

namespace fitom {

class COPLL : public CSoundDevice {
public:
    // mode: 0=トーンのみ (9ch), 1=リズムモード (6ch + リズム)
    // maxChs: 物理チャンネル数。VRC7 (リズム回路なし) は 6 を渡す。
    //         CSoundDevice のコンストラクタが maxChs 以降のチャンネルを
    //         自動的に disable() するため、getChCount() も正しく反映される。
    COPLL(IPort* port, int sampleRate, uint8_t mode = 0,
          uint8_t devId = DEVICE_OPLL, uint8_t maxChs = 9)
        : CSoundDevice(devId, maxChs, port,
                       sampleRate, 72,
                       FNUM_OFFSET,
                       FnumTableType::Fnumber,
                       0x40)
        , rhythmMode_(mode)
    {
        opCount_ = 2;
        if (rhythmMode_) {
            // ch 6-8 をリズム専用として自動割り当て禁止
            // (maxChs=6 の場合は既に disable 済みのため範囲外アクセスにならない)
            for (int i = 6; i < 9 && i < MAX_CHS; ++i) chState_[i].disable();
        }
    }

    // getDescriptor: リズムモードの有無を反映する。
    // "OPLL (YM2413) 9ch" または "OPLL (YM2413) 6ch + Rhythm 5ch"
    // 派生クラスは chipLabel() だけをオーバーライドすればよい。
    std::string getDescriptor() const override {
        std::string label = chipLabel();
        if (rhythmMode_) {
            return label + " " + std::to_string(maxChs_ - 3) + "ch + Rhythm 5ch";
        }
        return label + " " + std::to_string(maxChs_) + "ch";
    }
    void init() override {}

    void reset() override {
        CSoundDevice::reset();
        if (rhythmMode_) {
            for (int i = 6; i < 9; ++i) chState_[i].disable();
        }
    }

protected:
    bool rhythmMode_;

    // 派生クラスがチップ名部分だけを差し替えるためのフック。
    virtual std::string chipLabel() const { return "OPLL (YM2413)"; }

    // OPLL 専用: プリセットか否かで UpdateVoice 挙動が変わる
    void updateVoice(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;

        // ext.ALG_EXT: bit0 = プリセット選択フラグ (旧 AL & 0x40)
        bool preset = (p.ext.ALG_EXT & 1) != 0;
        uint8_t instNo = preset ? (p.hw.ALG & 0xF) : 0;

        if (!preset) {
            // ユーザー音色レジスタへ書き込み (0x00-0x07)
            for (int i = 0; i < 2; ++i) {
                const FmHwOp& o = p.hwOp[i];
                // AM/VIB/EG/KSR/MUL
                setReg(static_cast<uint16_t>(i),
                       static_cast<uint8_t>(
                           ((o.AM & 1) << 7) | ((o.VIB & 1) << 6) |
                           ((o.SR > 0) ? 0 : 0x20) |
                           ((o.KSR & 1) << 4) | (o.MUL & 0xF)));
                // AR / DR (キャリア=i:1 はベロシティ補正)
                const bool car_opll = (i == 1);
                const uint8_t ar_opll = car_opll ? s.proc.velAR(i) : (o.AR & 0x1F);
                const uint8_t dr_opll = car_opll ? s.proc.velDR(i) : (o.DR & 0x1F);
                setReg(static_cast<uint16_t>(4 + i),
                       static_cast<uint8_t>(((ar_opll >> 1) << 4) | (dr_opll >> 1)));
                // SL / RR (Sustain は SUS bit で制御するため RR をそのまま書く)
                const uint8_t sl_opll = car_opll ? s.proc.velSL(i) : (o.SL & 0xF);
                const uint8_t rr_opll = car_opll ? s.proc.velRR(i) : (o.RR & 0xF);
                setReg(static_cast<uint16_t>(6 + i),
                       static_cast<uint8_t>(((sl_opll & 0xF) << 4) | (rr_opll & 0xF)));
            }
            // TL / KSL (op0)
            setReg(0x02, static_cast<uint8_t>(((p.hwOp[0].KSL & 3) << 6) | (p.hwOp[0].TL >> 1)));
            // KSL / WS (op0/op1) / FB
            setReg(0x03, static_cast<uint8_t>(
                ((p.hwOp[1].KSL & 3) << 6) |
                ((p.hwOp[0].WS & 1) << 3) |
                ((p.hwOp[1].WS & 1) << 4) |
                (p.hw.FB & 7)));
        }

        // inst / vol (inst: bit7-4, vol: bit3-0)
        uint8_t cur = getReg(static_cast<uint16_t>(0x30 + ch)) & 0x0F;
        setReg(static_cast<uint16_t>(0x30 + ch),
               static_cast<uint8_t>((instNo << 4) | cur));
    }

    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        // OPLL はキャリア (op1) の effectiveTL (TL空間、0=最大音量) をラウドネスに反転してから
        // 正しいdB変換 (48dB/1.5dBステップ、4bit) を行う。
        uint8_t loudness = 127u - s.proc.effectiveTL(1);
        uint8_t vol = fitom::linear2dB(loudness, RANGE48DB, STEP150DB, 4);
        uint8_t cur = getReg(static_cast<uint16_t>(0x30 + ch)) & 0xF0;
        setReg(static_cast<uint16_t>(0x30 + ch), static_cast<uint8_t>(cur | (vol & 0xF)), false);
    }

    void updateTL(uint8_t ch, uint8_t /*op*/, uint8_t lev) override {
        // OPLL はボリュームレジスタ (0x30 下位 4bit) のみ
        uint8_t loudness = 127u - lev;
        uint8_t vol = fitom::linear2dB(loudness, RANGE48DB, STEP150DB, 4);
        uint8_t cur = getReg(static_cast<uint16_t>(0x30 + ch)) & 0xF0;
        setReg(static_cast<uint16_t>(0x30 + ch), static_cast<uint8_t>(cur | (vol & 0xF)), false);
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        // getFnumber() は11bit精度で値を返すが、実機OPLLのFnumberは9bit。
        // 旧FITOMの ">>2" (11bit→9bit変換) が新実装で欠落していたため復元する。
        uint16_t fnum9 = static_cast<uint16_t>((fnum.fnum >> 2) & 0x1FF);
        uint8_t b0cur = getReg(static_cast<uint16_t>(0x20 + ch)) & 0x30; // KEY/SUSビット保持
        setReg(static_cast<uint16_t>(0x10 + ch),
               static_cast<uint8_t>(fnum9 & 0xFF), false);
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>(b0cur | ((fnum.block & 7) << 1) | ((fnum9 >> 8) & 1)), false);
    }

    void updatePanpot(uint8_t /*ch*/) override {
        // OPLL はパンポット非対応
    }

    // CC#120 (All Sound Off): SUS bit を強制的にクリアしてから noteOff。
    // OPLL は RR を直接操作できないため（ROM音色はEG変更不可）、
    // SUS bit を外すことで通常のリリース動作に戻してから noteOff する。
    // これにより sustain 中の音がすぐにリリースフェーズへ移行する。
    void forceDamp(uint8_t ch) override {
        if (ch >= maxChs_) return;
        const auto& s = chState_[ch];
        if (!s.isActive()) return;
        const uint8_t cur = getReg(static_cast<uint16_t>(0x20 + ch));
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>(cur & 0xDFu)); // SUS bit クリア
        noteOff(ch);
    }

    void updateSustain(uint8_t ch) override {
        // OPLL: 0x20+ch レジスタの bit5 = SUS フラグを操作する。
        // ROM 音色はエンベロープパラメータ変更不可のため、
        // ユーザー音色・ROM 音色を問わず常に SUS bit で制御する。
        // SUS=1: NoteOff 後もチップが無限サスティンレートで引き延ばす。
        // SUS=0: 通常のリリース動作に戻す。
        // 旧 FITOM COPLL::UpdateSustain と同等。
        const auto& s = chState_[ch];
        const uint8_t sus_bit = s.sustain ? 0x20u : 0x00u;
        const uint8_t cur     = getReg(static_cast<uint16_t>(0x20 + ch));
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>((cur & 0xDFu) | sus_bit));
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        bool preset = (p.ext.ALG_EXT & 1) != 0;

        // EGT/RR動的書き換え (OPN/OPL3と同じ技法)。
        // ユーザー音色のみ対象 (プリセット音色はROMのためEGパラメータ変更不可、
        // 旧FITOM COPLL::UpdateKey と同様)。
        // キーオン中はSRをRR位置に書きEGT=0 (サスティンレイトとして機能)、
        // キーオフ時はEGT=1に切り替えてRRレジスタをリリースレイトとして使う。
        if (!preset) {
            for (int i = 0; i < 2; ++i) {
                const FmHwOp& o = p.hwOp[i];
                bool car = (i == 1); // OPLLはOP1がキャリア固定
                uint8_t sr = car ? s.proc.velSR(i) : (o.SR & 0x1F);
                uint8_t rr = car ? s.proc.velRR(i) : (o.RR & 0xF);
                uint8_t sl = car ? s.proc.velSL(i) : (o.SL & 0xF);
                bool useSR = keyOn && (sr != 0);
                uint8_t rrReg = useSR ? ((sr >> 1) & 0xF) : (rr & 0xF); // 5bit→4bit

                uint8_t egtCur = getReg(static_cast<uint16_t>(i)) & 0xDF;
                setReg(static_cast<uint16_t>(i),
                       static_cast<uint8_t>(egtCur | (useSR ? 0 : 0x20)), true);
                setReg(static_cast<uint16_t>(6 + i),
                       static_cast<uint8_t>(((sl & 0xF) << 4) | rrReg), true);
            }
        }

        uint8_t cur = getReg(static_cast<uint16_t>(0x20 + ch)) & 0xEF;
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>(cur | (keyOn ? 0x10 : 0)), true);
    }
};

// ================================================================
//  COPLL2 / COPLLP / COPLLX — デバイス ID 違いのみ (制御ロジックは共通)
//  内蔵プリセット音色がそれぞれ異なるため、独立したチップとして扱う。
//  リズムモードは OPLL と同じレジスタ構造のためそのまま利用できる。
// ================================================================
class COPLL2 : public COPLL {
public:
    COPLL2(IPort* port, int sampleRate, uint8_t mode = 0)
        : COPLL(port, sampleRate, mode, DEVICE_OPLL2) {}
protected:
    std::string chipLabel() const override { return "OPLL2 (YM2420)"; }

    // YM2420 (OPLL2) は Fnumber のレジスタ配置が YM2413 と異なる。
    // 生の11bit fnum値を直接使用 (COPLL側のような9bit変換は行わない)。
    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        setReg(static_cast<uint16_t>(0x10 + ch),
               static_cast<uint8_t>(((fnum.block & 7) << 5) | ((fnum.fnum >> 6) & 0xFF)), false);
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>((getReg(static_cast<uint16_t>(0x20 + ch)) & 0xF0)
                   | ((fnum.fnum >> 2) & 0xF)), false);
    }
};

class COPLLP : public COPLL {
public:
    COPLLP(IPort* port, int sampleRate, uint8_t mode = 0)
        : COPLL(port, sampleRate, mode, DEVICE_OPLLP) {}
protected:
    std::string chipLabel() const override { return "OPLLP (YMF281B)"; }
};

class COPLLX : public COPLL {
public:
    COPLLX(IPort* port, int sampleRate, uint8_t mode = 0)
        : COPLL(port, sampleRate, mode, DEVICE_OPLLX) {}
protected:
    std::string chipLabel() const override { return "OPLLX (YM2423-X)"; }
};

// ================================================================
//  CVRC7 — OPLL からリズムチャンネルを削除した派生 (FS1001)
//  制御ロジックは OPLL と同一だが、リズム音源回路自体が存在しないため
//  楽音 6ch のみが有効。CSoundDevice に maxChs=6 を渡すことで
//  getChCount() も正しく 6 を返し、ch 6-8 は自動的に disable される。
//  リズム回路自体が存在しないため、rhythmMode は常に無効に固定する
//  (呼び出し元から true が渡されても無視する)。
// ================================================================
class CVRC7 : public COPLL {
public:
    CVRC7(IPort* port, int sampleRate)
        : COPLL(port, sampleRate, /*mode=*/0, DEVICE_VRC7, 6) {}
protected:
    std::string chipLabel() const override { return "VRC7 (FS1001)"; }
};

// ================================================================
//  COPLLRhythm: YM2413 (OPLL) 内蔵リズム音源 (5パート: HH/CYM/TOM/SD/BD)
//
//  FM本体とは独立したレジスタ体系:
//    0x0E: bit5=リズムモード有効固定、bit0-4=各パートのキーオン
//    0x36/0x37/0x38: パート音量 (2パートずつ上位/下位4bitに packing)
//    パート→物理ch対応 (旧FITOM RhythmMapCh): {7,8,8,7,6}
//
//  sub-device自動生成により、OPLL本体と同一の物理ポートを共有する
//  独立デバイスとして生成される。
// ================================================================
class COPLLRhythm : public CSoundDevice {
public:
    COPLLRhythm(IPort* port, int sampleRate)
        : CSoundDevice(DEVICE_OPLL_RHY, 5, port,
                       sampleRate, 72,
                       FNUM_OFFSET,
                       FnumTableType::Fnumber,
                       0x40)
    {}

    std::string getDescriptor() const override { return "OPLL Rhythm 5ch"; }
    void init() override { setReg(0x0E, 0x20, true); }

protected:
    // パート(0-4: HH,CYM,TOM,SD,BD) → 音量レジスタ / 物理ch対応 (旧FITOM完全移植)
    static constexpr uint8_t kRhythmReg[5]   = {0x37, 0x38, 0x38, 0x37, 0x36};
    static constexpr uint8_t kRhythmMapCh[5] = {7, 8, 8, 7, 6};

    // 物理ch6/7/8固定のFnumber/Block (旧FITOM RhythmFnum完全移植)。
    // リズム音はROM内蔵の固定音色だが、Fnum/Blockレジスタの値自体は
    // 実機マニュアル記載の固定値を設定する必要がある
    // (ノイズ/エンベロープ生成に影響するため無意味な値ではない)。
    // インデックスは (物理ch - 6) : ch6→0, ch7→1, ch8→2
    struct FixedFnum { uint8_t block; uint16_t fnum; };
    static constexpr FixedFnum kRhythmFnum[3] = {
        {2, 0x480}, // ch6 (BD)
        {2, 0x540}, // ch7 (HH/SD 共用)
        {0, 0x700}, // ch8 (CYM/TOM 共用)
    };

    // リズムパートは固定音程のため、ノート番号は無視し物理ch6-8の
    // 固定Fnum/Blockを直接書き込む (旧FITOM COPLLRhythm::UpdateFreq 相当)。
    // COPLLRhythm は OPLL本体と同一の物理ポートを共有しているため、
    // 親インスタンスを介さず直接 setReg してよい。
    void updateVoice(uint8_t ch) override {
        updateFreq(ch, nullptr); // 固定Fnumberを毎回反映 (パッチロード時にも保証)
        updateVolExp(ch);
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* /*fn*/) override {
        uint8_t vch = kRhythmMapCh[ch];
        const FixedFnum& f = kRhythmFnum[vch - 6];
        // getFnumberFromHz/getFnumber は使わず、実機マニュアル記載の
        // 生のFnum/Block値をそのまま書き込む (COPLL::updateFreqと同じ
        // 9bit変換は不要、既に9bit相当の生値)。
        uint8_t b0cur = getReg(static_cast<uint16_t>(0x20 + vch)) & 0x30;
        setReg(static_cast<uint16_t>(0x10 + vch),
               static_cast<uint8_t>(f.fnum & 0xFF), true);
        setReg(static_cast<uint16_t>(0x20 + vch),
               static_cast<uint8_t>(b0cur | ((f.block & 7) << 1) | ((f.fnum >> 8) & 1)), true);
    }
    void updateSustain(uint8_t /*ch*/) override {}
    void updatePanpot(uint8_t /*ch*/) override {}
    void updateTL(uint8_t, uint8_t, uint8_t) override {}

    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        // 旧FITOM: CalcLinearLevel(GetVolume(), 127-velocity) という
        // MIDI Volume(CC#7)とベロシティの組み合わせ (Expressionは含まない)。
        uint8_t loudness = fitom::calcVolExpVel(s.volume, 127u, 127u - s.velocity);
        uint8_t vol = fitom::linear2dB(loudness, RANGE48DB, STEP150DB, 4);
        uint16_t addr = kRhythmReg[ch];
        // ch&5: ch=1,3,4 は上位nibble、ch=0,2 は下位nibble (旧FITOM完全移植)
        bool highNibble = (ch & 5) != 0;
        uint8_t mask = highNibble ? 0x0F : 0xF0;
        uint8_t shifted = highNibble ? static_cast<uint8_t>(vol << 4) : vol;
        setReg(addr, static_cast<uint8_t>((getReg(addr) & mask) | shifted));
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        uint8_t keymask = static_cast<uint8_t>(~(1u << ch));
        uint8_t cur = getReg(0x0E) & keymask;
        setReg(0x0E, static_cast<uint8_t>(cur | 0x20 | (keyOn ? (1u << ch) : 0)), true);
    }

    // パート番号は音色データの hw.ALG (下位3bit) で直接指定する。
    // 該当パートが既に使用中なら 0xFF (旧FITOM COPLLRhythm::QueryCh 完全移植)。
    uint8_t queryCh(IMidiCh* /*owner*/, const HwPatch* patch, int mode) override {
        if (!patch) return 0xFF;
        uint8_t num = patch->hw.ALG & 0x7;
        if (num >= 5) return 0xFF;
        bool inuse = (getReg(0x0E) & (1u << num)) != 0;
        return mode ? num : (inuse ? 0xFF : num);
    }
};

} // namespace fitom

namespace fitom {
std::unique_ptr<ISoundDevice> createCOPLL(IPort* p, int sr, uint8_t m)  { return std::make_unique<COPLL>(p, sr, m); }
std::unique_ptr<ISoundDevice> createCOPLL2(IPort* p, int sr, uint8_t m) { return std::make_unique<COPLL2>(p, sr, m); }
std::unique_ptr<ISoundDevice> createCOPLLP(IPort* p, int sr, uint8_t m) { return std::make_unique<COPLLP>(p, sr, m); }
std::unique_ptr<ISoundDevice> createCOPLLX(IPort* p, int sr, uint8_t m) { return std::make_unique<COPLLX>(p, sr, m); }
std::unique_ptr<ISoundDevice> createCVRC7(IPort* p, int sr)  { return std::make_unique<CVRC7>(p, sr); }
std::unique_ptr<ISoundDevice> createCOPLLRhythm(IPort* p, int sr) { return std::make_unique<COPLLRhythm>(p, sr); }
} // namespace fitom
