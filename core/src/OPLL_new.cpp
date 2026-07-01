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
#include "fitom/Log.h"
#include <algorithm>

namespace fitom {

class COPLL : public CSoundDevice {
public:
    // mode: 0=トーンのみ (9ch), 1=リズムモード (6ch + リズム)
    COPLL(IPort* port, int sampleRate, uint8_t mode = 0,
          uint8_t devId = DEVICE_OPLL)
        : CSoundDevice(devId, 9, port,
                       sampleRate, 72,
                       FNUM_OFFSET,
                       FnumTableType::Fnumber,
                       0x40)
        , rhythmMode_(mode)
    {
        opCount_ = 2;
        if (rhythmMode_) {
            // ch 6-8 をリズム専用として自動割り当て禁止
            for (int i = 6; i < 9; ++i) chState_[i].disable();
        }
    }

    std::string getDescriptor() const override { return "OPLL (YM2413) 9ch"; }
    void init() override {}

    void reset() override {
        CSoundDevice::reset();
        if (rhythmMode_) {
            for (int i = 6; i < 9; ++i) chState_[i].disable();
        }
    }

protected:
    bool rhythmMode_;

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
        const HwPatch& p = s.hwPatch;
        // OPLL はキャリア (op1) の TL がボリューム (4bit)
        uint8_t tl = s.proc.effectiveTL(1);
        // 7bit → 4bit (OPLL は RANGE48DB / STEP150DB / 4bit)
        // 簡易変換: 上位 4bit を使う
        uint8_t vol = (tl >> 3) & 0xF;
        uint8_t cur = getReg(static_cast<uint16_t>(0x30 + ch)) & 0xF0;
        setReg(static_cast<uint16_t>(0x30 + ch), static_cast<uint8_t>(cur | vol), false);
    }

    void updateTL(uint8_t ch, uint8_t /*op*/, uint8_t lev) override {
        // OPLL はボリュームレジスタ (0x30 下位 4bit) のみ
        uint8_t vol = (lev >> 3) & 0xF;
        uint8_t cur = getReg(static_cast<uint16_t>(0x30 + ch)) & 0xF0;
        setReg(static_cast<uint16_t>(0x30 + ch), static_cast<uint8_t>(cur | vol), false);
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint8_t b0cur = getReg(static_cast<uint16_t>(0x20 + ch)) & 0xF0;
        // OPLL: 0x10+ch = fnum LSB, 0x20+ch = block/fnum MSB/keyon
        setReg(static_cast<uint16_t>(0x10 + ch),
               static_cast<uint8_t>(fnum.fnum & 0xFF), false);
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>(b0cur | ((fnum.block & 7) << 1) | ((fnum.fnum >> 8) & 1)), false);
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
        uint8_t cur = getReg(static_cast<uint16_t>(0x20 + ch)) & 0xEF;
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>(cur | (keyOn ? 0x10 : 0)), true);
    }
};

// ================================================================
//  COPLL2 / COPLLP / COPLLX — デバイス ID 違いのみ
// ================================================================
class COPLL2 : public COPLL {
public:
    COPLL2(IPort* port, int sampleRate)
        : COPLL(port, sampleRate, 0, DEVICE_OPLL2) {}
    std::string getDescriptor() const override { return "OPLL2 9ch"; }
};

class COPLLP : public COPLL {
public:
    COPLLP(IPort* port, int sampleRate)
        : COPLL(port, sampleRate, 0, DEVICE_OPLLP) {}
    std::string getDescriptor() const override { return "VRC7 (OPLLP) 9ch"; }
};

} // namespace fitom

namespace fitom {
std::unique_ptr<ISoundDevice> createCOPLL(IPort* p, int sr, uint8_t m)  { return std::make_unique<COPLL>(p, sr, m); }
std::unique_ptr<ISoundDevice> createCOPLL2(IPort* p, int sr) { return std::make_unique<COPLL2>(p, sr); }
std::unique_ptr<ISoundDevice> createCOPLLP(IPort* p, int sr) { return std::make_unique<COPLLP>(p, sr); }
} // namespace fitom
