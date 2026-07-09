// fitom/OPL_new.cpp
// COPL / COPL2 / C3801 / COPL3 チップドライバ — ISoundDevice ベース移行版
//
// OPL 系の特徴:
//   - 2OP (ops=2)
//   - ALG は 1bit (FM=0 / Add=1)、AL&1 がキャリア判定
//   - TL は 6bit (64段階)
//   - AR/DR/SR/RR は 4bit (元のビット幅 >>1 で使用)
//   - KSL あり / DT1・DT2 なし
//   - VIB あり / WS あり (OPL2 以降)
//   - UpdateKey でキーオン/オフを Sustain とともに制御
//   - パンポット: レジスタ 0xC0 の bit7/bit6 (OPL3 は 4bit)

#include "fitom/ISoundDevice.h"
#include "fitom/MultiDevice.h"
#include "fitom/IPort.h"
#include "fitom/FITOMdefine.h"
#include "fitom/Log.h"
#include <algorithm>

namespace fitom {

// ================================================================
//  COPL (OPL / OPL2 / Y8950 基底)
// ================================================================
class COPL : public CSoundDevice {
public:
    COPL(IPort* port, int sampleRate, uint8_t devId = DEVICE_OPL2)
        : CSoundDevice(devId, 9, port,
                       sampleRate, 72,   // fnum master/divide
                       FNUM_OFFSET,
                       FnumTableType::Fnumber,
                       0x100)
    {
        opCount_ = 2;
    }

    std::string getDescriptor() const override { return "OPL2 (YM3812) 9ch"; }

    void init() override {
        setReg(0x04, 0, true);
        setReg(0x08, 0, true);
    }

    void reset() override {
        CSoundDevice::reset();
        for (int i = 0x20; i < 0xF6; ++i) setReg(static_cast<uint16_t>(i), 0, true);
    }

protected:
    // チャンネル → オペレータスロットのオフセット
    static const uint8_t kMap[9]; // {0,1,2,8,9,10,16,17,18}

    // OPL 系キャリア判定: op1 は常にキャリア。ALG=1 なら op0 もキャリア
    bool isCarrier(uint8_t ch, int op) const {
        return (op == 1) || ((chState_[ch].hwPatch.hw.ALG & 1) != 0);
    }

    // ビット幅変換ヘルパー
    static uint8_t ar4(uint8_t v) { return v >> 1; }  // 5bit → 4bit (上位4bit)
    static uint8_t tl6(uint8_t v) { return v >> 1; }  // 7bit → 6bit

    void updateVoice(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        const bool sus = s.sustain;

        for (int i = 0; i < 2; ++i) {
            const FmHwOp& o = p.hwOp[i];
            uint8_t slot = kMap[ch];

            // AM / VIB / EG type / KSR / MUL
            setReg(static_cast<uint16_t>(0x20 + i * 3 + slot),
                   static_cast<uint8_t>(
                       ((o.AM  & 1) << 7) | ((o.VIB & 1) << 6) |
                       ((o.SR > 0) ? 0 : 0x20) |    // EG type (SR>0 → sustain mode)
                       ((o.KSR & 1) << 4) | (o.MUL & 0xF)));

            // KSL / TL (TL は updateVolExp で書くので、ここはモジュレータのみ)
            if (!isCarrier(ch, i)) {
                setReg(static_cast<uint16_t>(0x40 + i * 3 + slot),
                       static_cast<uint8_t>((o.KSL << 6) | tl6(o.TL)));
            }

            // AR / DR (キャリアはベロシティ補正値を使用)
            const bool car_opl = isCarrier(ch, i);
            const uint8_t ar_opl = car_opl ? s.proc.velAR(i) : (o.AR & 0x1F);
            const uint8_t dr_opl = car_opl ? s.proc.velDR(i) : (o.DR & 0x1F);
            setReg(static_cast<uint16_t>(0x60 + i * 3 + slot),
                   static_cast<uint8_t>((ar4(ar_opl) << 4) | ar4(dr_opl)));

            // SL / RR (サスティン中はフォールバックRR=4)
            static constexpr uint8_t kFallbackRR = 4;
            const uint8_t sl_opl = car_opl ? s.proc.velSL(i) : (o.SL & 0xF);
            const uint8_t rr_base = car_opl ? s.proc.velRR(i)
                                            : (o.SR ? ar4(o.SR) : o.RR);
            const uint8_t rr_opl = (sus && car_opl) ? kFallbackRR : rr_base;
            setReg(static_cast<uint16_t>(0x80 + i * 3 + slot),
                   static_cast<uint8_t>(((sl_opl & 0xF) << 4) | (rr_opl & 0xF)));

            // WS (OPL2 以降のみ有効)
            setReg(static_cast<uint16_t>(0xE0 + i * 3 + slot), o.WS & 0x3);
        }

        // FB / ALG / Pan (bit7-6 = L/R, bit3-1 = FB, bit0 = ALG)
        setReg(static_cast<uint16_t>(0xC0 + ch),
               static_cast<uint8_t>(0x30 | ((p.hw.FB & 7) << 1) | (p.hw.ALG & 1)));

        updateVolExp(ch);
        updatePanpot(ch);
    }

    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        for (int i = 0; i < 2; ++i) {
            if (!isCarrier(ch, i)) continue;
            uint8_t slot = kMap[ch];
            uint8_t tl = tl6(s.proc.effectiveTL(i));
            setReg(static_cast<uint16_t>(0x40 + i * 3 + slot),
                   static_cast<uint8_t>((p.hwOp[i].KSL << 6) | tl), false);
        }
    }

    void updateTL(uint8_t ch, uint8_t op, uint8_t lev) override {
        uint8_t slot = kMap[ch];
        lev = std::min<uint8_t>(lev, 63);
        setReg(static_cast<uint16_t>(0x40 + op * 3 + slot), lev, false);
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        // OPL: A0 = fnum[8:1], B0 = block[2:0] | fnum[9] | keyon
        uint8_t b0cur = getReg(static_cast<uint16_t>(0xB0 + ch)) & 0x20;
        setReg(static_cast<uint16_t>(0xA0 + ch),
               static_cast<uint8_t>((fnum.fnum >> 1) & 0xFF), true);
        setReg(static_cast<uint16_t>(0xB0 + ch),
               static_cast<uint8_t>(b0cur | ((fnum.block & 7) << 2) | ((fnum.fnum >> 9) & 1)), true);
    }

    void updatePanpot(uint8_t ch) override {
        int8_t pan = chState_[ch].panpot;
        uint8_t lr = (pan > 20) ? 0x10 : (pan < -20) ? 0x20 : 0x30;
        uint8_t cur = getReg(static_cast<uint16_t>(0xC0 + ch)) & 0x0F;
        setReg(static_cast<uint16_t>(0xC0 + ch), static_cast<uint8_t>(lr | cur));
    }

    // CC#120 (All Sound Off): キャリア OP の RR を最大値にして急速減衰。
    void forceDamp(uint8_t ch) override {
        if (ch >= maxChs_) return;
        const auto& s = chState_[ch];
        if (!s.isActive()) return;
        const HwPatch& p = s.hwPatch;
        const uint8_t slot = kMap[ch];
        for (int i = 0; i < 2; ++i) {
            if (!isCarrier(ch, i)) continue;
            const FmHwOp& o = p.hwOp[i];
            setReg(static_cast<uint16_t>(0x80 + i * 3 + slot),
                   static_cast<uint8_t>(((o.SL & 0xF) << 4) | 0xF)); // RR=15
        }
        noteOff(ch);
    }

    void updateSustain(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        for (int i = 0; i < 2; ++i) {
            if (!isCarrier(ch, i)) continue;
            uint8_t slot = kMap[ch];
            static constexpr uint8_t kFallbackRR = 4;
            const FmHwOp& o = p.hwOp[i];
            // サステインペダルON時にRR=4固定にするのは、実際にキーオフ中
            // (リリースフェーズ)の音のみ。キーオン中の音は EGT/RR による
            // サスティンレイト表現 (updateKey 参照) をそのまま維持する。
            uint8_t rr = (s.sustain && s.isReleasing())
                       ? kFallbackRR
                       : (o.SR ? ar4(o.SR) : o.RR);
            setReg(static_cast<uint16_t>(0x80 + i * 3 + slot),
                   static_cast<uint8_t>(((o.SL & 0xF) << 4) | (rr & 0xF)));
        }
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        uint8_t slot = kMap[ch];

        // リリース中の音が残った状態で同一chに新規ノートオンすると
        // アタック波形が不正になるため、事前に強制ダンプ(RR最大化)する。
        if (keyOn && s.wasReleasing) {
            for (int i = 0; i < 2; ++i) {
                if (!isCarrier(ch, i)) continue;
                const uint8_t sl = p.hwOp[i].SL & 0xF;
                setReg(static_cast<uint16_t>(0x80 + i * 3 + slot),
                       static_cast<uint8_t>((sl << 4) | 0xF));
            }
        }

        for (int i = 0; i < 2; ++i) {
            const FmHwOp& o = p.hwOp[i];
            // EG type bit (bit5): keyon=0 で SR モードに切り替え
            uint8_t cur = getReg(static_cast<uint16_t>(0x20 + i * 3 + slot)) & 0xDF;
            setReg(static_cast<uint16_t>(0x20 + i * 3 + slot),
                   static_cast<uint8_t>(cur | (keyOn ? 0 : 0x20)), true);

            // SL/RR: キーオン中は SR、オフ中は RR
            // サステインペダルON時のRR=4固定は、キーオフ(リリース)時のみ適用する。
            static constexpr uint8_t kFallbackRR = 4;
            bool carrier = isCarrier(ch, i);
            uint8_t rr = (s.sustain && carrier && !keyOn) ? kFallbackRR
                       : (keyOn ? ar4(o.SR) : o.RR);
            setReg(static_cast<uint16_t>(0x80 + i * 3 + slot),
                   static_cast<uint8_t>(((o.SL & 0xF) << 4) | (rr & 0xF)), true);
        }

        // B0 の bit5 = KeyOn
        uint8_t b0cur = getReg(static_cast<uint16_t>(0xB0 + ch)) & 0xDF;
        setReg(static_cast<uint16_t>(0xB0 + ch),
               static_cast<uint8_t>(b0cur | (keyOn ? 0x20 : 0)), true);
    }
};

const uint8_t COPL::kMap[9] = {0, 1, 2, 8, 9, 10, 16, 17, 18};

// ================================================================
//  COPL2 (OPL2 = YM3812) — WS を有効化
// ================================================================
class COPL2 : public COPL {
public:
    COPL2(IPort* port, int sampleRate)
        : COPL(port, sampleRate, DEVICE_OPL2) {}
    std::string getDescriptor() const override { return "OPL2 (YM3812) 9ch"; }
    void init() override {
        COPL::init();
        setReg(0x01, 0x20, true); // Wave Select Enable
    }
};

// ================================================================
//  C3801 (Y8950) — OPL ベース
// ================================================================
class C3801 : public COPL {
public:
    C3801(IPort* port, int sampleRate)
        : COPL(port, sampleRate, DEVICE_Y8950) {}
    std::string getDescriptor() const override { return "Y8950 9ch"; }
};

// ================================================================
//  COPL3 (OPL3 = YMF262) — 4OPモード専用、6ch
//
//  1ポートあたり最大3ch(ch0-2)を4OPとして使用 (前半opペア+後半opペア結合)。
//  2ポート合計で6ch。残りのch6-8(3chずつ)はCOPL3_2(2OP)が別途担当する。
//
//  4OPの構成は hw.ALG(3bit)にそのまま収まる:
//    bit0: 前半ペアのCON (0xC0+rop+dch のbit0)
//    bit1: 後半ペアのCON (0xC0+rop+pairCh のbit0)
//    bit2: ConnectionSEL (CONNECTIONSEL(0x104)で4OP結合を有効にするか)
//    carmsk[8]: hw.ALG(3bit)値ごとのキャリアOPビットマスク
// ================================================================
class COPL3 : public CSoundDevice {
public:
    COPL3(IPort* port, int sampleRate)
        : CSoundDevice(DEVICE_OPL3, 6, port,
                       sampleRate, 288,
                       FNUM_OFFSET,
                       FnumTableType::Fnumber,
                       0x200)
    {
        opCount_ = 4;
    }

    std::string getDescriptor() const override { return "OPL3 (YMF262) 4OP 6ch"; }

    void init() override {
        setReg(0x01, 0x20, true);  // Wave Select Enable (port1)
        setReg(0x105, 0x01, true); // OPL3 NEW1 (port2経由、OPL3モード有効)
    }

    void reset() override {
        CSoundDevice::reset();
        for (int i = 0x20; i < 0xF6; ++i) {
            setReg(static_cast<uint16_t>(i), 0, true);
            setReg(static_cast<uint16_t>(0x100 + i), 0, true);
        }
    }

protected:
    static const uint8_t opmap[4];  // 4オペレータ分のスロットオフセット
    static const uint8_t carmsk[8]; // hw.ALG(3bit)値ごとのキャリアOPビットマスク

    uint16_t portBase(uint8_t ch) const { return (ch >= 3) ? 0x100 : 0; }
    uint8_t  localCh(uint8_t ch)  const { return ch % 3; }
    // 後半opペアのローカルch (常に前半+3、旧FITOM同様)
    uint8_t  pairCh(uint8_t ch)   const { return static_cast<uint8_t>(localCh(ch) + 3); }

    uint8_t alValue(uint8_t ch) const {
        return chState_[ch].hwPatch.hw.ALG & 0x7;
    }
    bool isCarrier(uint8_t ch, int op) const {
        return (carmsk[alValue(ch)] & (1 << op)) != 0;
    }
    static uint8_t ar4(uint8_t v) { return v >> 1; } // 5bit → 4bit
    static uint8_t tl6(uint8_t v) { return v >> 1; } // 7bit → 6bit

    void updateVoice(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        const bool sus = s.sustain;
        uint16_t rop = portBase(ch);
        uint8_t  dch = localCh(ch);
        uint8_t  al  = alValue(ch);

        for (int i = 0; i < 4; ++i) {
            const FmHwOp& o = p.hwOp[i];
            uint16_t slot = static_cast<uint16_t>(rop + opmap[i] + dch);
            bool car = isCarrier(ch, i);

            // AM/VIB/EGT/KSR/MUL
            setReg(static_cast<uint16_t>(0x20 + slot),
                   static_cast<uint8_t>(
                       ((o.AM & 1) << 7) | ((o.VIB & 1) << 6) |
                       ((o.SR > 0) ? 0 : 0x20) |
                       ((o.KSR & 1) << 4) | (o.MUL & 0xF)), true);

            // KSL/TL (モジュレータのみ。キャリアはupdateVolExpで書く)
            if (!car) {
                setReg(static_cast<uint16_t>(0x40 + slot),
                       static_cast<uint8_t>((o.KSL << 6) | tl6(o.TL)), true);
            }

            // AR/DR (キャリアはベロシティ補正値)
            const uint8_t ar_ = car ? s.proc.velAR(i) : (o.AR & 0x1F);
            const uint8_t dr_ = car ? s.proc.velDR(i) : (o.DR & 0x1F);
            setReg(static_cast<uint16_t>(0x60 + slot),
                   static_cast<uint8_t>((ar4(ar_) << 4) | ar4(dr_)), true);

            // SL/RR (サスティン中はフォールバックRR=4、キーオフ時のみ)
            static constexpr uint8_t kFallbackRR = 4;
            const uint8_t sl_ = car ? s.proc.velSL(i) : (o.SL & 0xF);
            const uint8_t rr_base = car ? s.proc.velRR(i) : o.RR;
            const uint8_t rr_ = (sus && car) ? kFallbackRR : rr_base;
            setReg(static_cast<uint16_t>(0x80 + slot),
                   static_cast<uint8_t>(((sl_ & 0xF) << 4) | (rr_ & 0xF)), true);

            // WS (OPL3は3bit)
            setReg(static_cast<uint16_t>(0xE0 + slot), static_cast<uint8_t>(o.WS & 0x7), true);
        }

        // FB/CON: 前半ペア・後半ペアそれぞれの0xC0レジスタに書く
        // (実機OPL3は前半・後半ペアそれぞれ独立したFBレジスタを持つため、
        //  前半はhw.FB、後半はhw.FB2を使う)
        uint16_t reg1 = static_cast<uint16_t>(0xC0 + rop + dch);
        uint16_t reg2 = static_cast<uint16_t>(0xC0 + rop + pairCh(ch));
        setReg(reg1, static_cast<uint8_t>((getReg(reg1) & 0xF0) | ((p.hw.FB & 7) << 1) | (al & 0x1)), true);
        setReg(reg2, static_cast<uint8_t>((getReg(reg2) & 0xF0) | ((p.hw.FB2 & 7) << 1) | ((al >> 1) & 0x1)), true);

        // CONNECTIONSEL(0x104): 4OPペア結合ビット
        uint8_t con = getReg(0x104) & static_cast<uint8_t>(~(1u << ch));
        bool con4op = (al & 0x04) != 0; // bit2 = ConnectionSEL
        setReg(0x104, static_cast<uint8_t>(con | (con4op ? (1u << ch) : 0)), true);

        updateVolExp(ch);
        updatePanpot(ch);
    }

    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        uint16_t rop = portBase(ch);
        uint8_t  dch = localCh(ch);
        for (int i = 0; i < 4; ++i) {
            if (!isCarrier(ch, i)) continue;
            uint16_t slot = static_cast<uint16_t>(rop + opmap[i] + dch);
            uint8_t tl = tl6(s.proc.effectiveTL(i));
            setReg(static_cast<uint16_t>(0x40 + slot),
                   static_cast<uint8_t>((p.hwOp[i].KSL << 6) | tl), false);
        }
    }

    void updateTL(uint8_t ch, uint8_t op, uint8_t lev) override {
        uint16_t rop = portBase(ch);
        uint8_t  dch = localCh(ch);
        uint16_t slot = static_cast<uint16_t>(rop + opmap[op] + dch);
        lev = std::min<uint8_t>(lev, 63);
        setReg(static_cast<uint16_t>(0x40 + slot), lev, false);
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        // 疑似デチューン: op[0]/op[2] (各2OPペアの先頭オペレータ) の
        // DT2 フィールドを符号付き8bit (int8_t、100/64セント単位) として
        // 再解釈し、前半/後半ペアで別々のFnumberを計算する。
        // getFnumber() の offset 引数は index 単位 = 100/64セントであり、
        // DT2 の単位と一致するため変換不要でそのまま渡せる。
        const HwPatch& p = chState_[ch].hwPatch;
        int16_t pdt1 = static_cast<int16_t>(static_cast<int8_t>(p.hwOp[0].DT2));
        int16_t pdt2 = static_cast<int16_t>(static_cast<int8_t>(p.hwOp[2].DT2));

        ChState::Fnum fnum1 = fn ? *fn : getFnumber(ch, pdt1);
        ChState::Fnum fnum2 = fn ? *fn : getFnumber(ch, pdt2);
        // fn (外部から明示指定、例: ポルタメント時) が渡された場合は
        // 疑似デチューンを適用せず両ペアとも同じ fnum を使う。

        uint16_t rop = portBase(ch);
        uint8_t  dch = localCh(ch);

        uint16_t reg_a1 = static_cast<uint16_t>(rop + 0xA0 + dch);
        uint16_t reg_b1 = static_cast<uint16_t>(rop + 0xB0 + dch);
        uint16_t reg_a2 = static_cast<uint16_t>(rop + 0xA0 + pairCh(ch));
        uint16_t reg_b2 = static_cast<uint16_t>(rop + 0xB0 + pairCh(ch));

        uint8_t b1cur = getReg(reg_b1) & 0x20;
        setReg(reg_a1, static_cast<uint8_t>((fnum1.fnum >> 1) & 0xFF), true);
        setReg(reg_b1, static_cast<uint8_t>(b1cur | ((fnum1.block & 7) << 2) | ((fnum1.fnum >> 9) & 1)), true);

        uint8_t b2cur = getReg(reg_b2) & 0x20;
        setReg(reg_a2, static_cast<uint8_t>((fnum2.fnum >> 1) & 0xFF), true);
        setReg(reg_b2, static_cast<uint8_t>(b2cur | ((fnum2.block & 7) << 2) | ((fnum2.fnum >> 9) & 1)), true);
    }

    void updatePanpot(uint8_t ch) override {
        int8_t pan = chState_[ch].panpot;
        uint8_t lr = (pan > 20) ? 0x10 : (pan < -20) ? 0x20 : 0x30;
        uint16_t rop = portBase(ch);
        uint8_t  dch = localCh(ch);
        uint16_t reg1 = static_cast<uint16_t>(0xC0 + rop + dch);
        uint16_t reg2 = static_cast<uint16_t>(0xC0 + rop + pairCh(ch));
        setReg(reg1, static_cast<uint8_t>(lr | (getReg(reg1) & 0x0F)));
        setReg(reg2, static_cast<uint8_t>(lr | (getReg(reg2) & 0x0F)));
    }

    void forceDamp(uint8_t ch) override {
        if (ch >= maxChs_) return;
        const auto& s = chState_[ch];
        if (!s.isActive()) return;
        const HwPatch& p = s.hwPatch;
        uint16_t rop = portBase(ch);
        uint8_t  dch = localCh(ch);
        for (int i = 0; i < 4; ++i) {
            if (!isCarrier(ch, i)) continue;
            uint16_t slot = static_cast<uint16_t>(rop + opmap[i] + dch);
            const FmHwOp& o = p.hwOp[i];
            setReg(static_cast<uint16_t>(0x80 + slot),
                   static_cast<uint8_t>(((o.SL & 0xF) << 4) | 0xF)); // RR=15
        }
        noteOff(ch);
    }

    void updateSustain(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        uint16_t rop = portBase(ch);
        uint8_t  dch = localCh(ch);
        for (int i = 0; i < 4; ++i) {
            if (!isCarrier(ch, i)) continue;
            uint16_t slot = static_cast<uint16_t>(rop + opmap[i] + dch);
            static constexpr uint8_t kFallbackRR = 4;
            const FmHwOp& o = p.hwOp[i];
            uint8_t rr = (s.sustain && s.isReleasing()) ? kFallbackRR : o.RR;
            setReg(static_cast<uint16_t>(0x80 + slot),
                   static_cast<uint8_t>(((o.SL & 0xF) << 4) | (rr & 0xF)));
        }
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        uint16_t rop = portBase(ch);
        uint8_t  dch = localCh(ch);

        // リリース中の音が残った状態で同一chに新規ノートオンすると
        // アタック波形が不正になるため、事前に強制ダンプ(RR最大化)する。
        if (keyOn && s.wasReleasing) {
            for (int i = 0; i < 4; ++i) {
                if (!isCarrier(ch, i)) continue;
                uint16_t slot = static_cast<uint16_t>(rop + opmap[i] + dch);
                const uint8_t sl = p.hwOp[i].SL & 0xF;
                setReg(static_cast<uint16_t>(0x80 + slot),
                       static_cast<uint8_t>((sl << 4) | 0xF));
            }
        }

        for (int i = 0; i < 4; ++i) {
            const FmHwOp& o = p.hwOp[i];
            uint16_t slot = static_cast<uint16_t>(rop + opmap[i] + dch);
            // EGT: キーオフ時はRRレジスタをリリースレイトとして機能させるため
            // EGT=1に切り替える (キーオン中は音色データのSR有無で決定済み)
            uint8_t cur = getReg(static_cast<uint16_t>(0x20 + slot)) & 0xDF;
            setReg(static_cast<uint16_t>(0x20 + slot),
                   static_cast<uint8_t>(cur | (keyOn ? 0 : 0x20)), true);

            static constexpr uint8_t kFallbackRR = 4;
            bool carrier = isCarrier(ch, i);
            uint8_t rr = (s.sustain && carrier && !keyOn) ? kFallbackRR
                       : (keyOn ? ar4(o.SR) : o.RR);
            setReg(static_cast<uint16_t>(0x80 + slot),
                   static_cast<uint8_t>(((o.SL & 0xF) << 4) | (rr & 0xF)), true);
        }

        // B0/B3 の bit5 = KeyOn (前半・後半両方)
        uint16_t reg_b1 = static_cast<uint16_t>(rop + 0xB0 + dch);
        uint16_t reg_b2 = static_cast<uint16_t>(rop + 0xB0 + pairCh(ch));
        uint8_t b1cur = getReg(reg_b1) & 0xDF;
        setReg(reg_b1, static_cast<uint8_t>(b1cur | (keyOn ? 0x20 : 0)), true);
        // COPL3 は常に4OP専用チャンネルとして使うデバイスのため、
        // 前半・後半ペアのキーオン/オフは常に同時に行う
        // (hw.ALG bit2=ConnectionSELの値に関わらず)。
        uint8_t b2cur = getReg(reg_b2) & 0xDF;
        setReg(reg_b2, static_cast<uint8_t>(b2cur | (keyOn ? 0x20 : 0)), true);
    }
};

const uint8_t COPL3::opmap[4] = {0x0, 0x3, 0x8, 0xb};
const uint8_t COPL3::carmsk[8] = {0x2, 0x3, 0x8, 0xc, 0x8, 0x9, 0xa, 0xd};


//
//  実機OPL3は1ポートあたり4OPモード最大3ch(ch0-2)を使うと、
//  残りのch6-8(3ch)は2OPのまま使える。2ポート合計で
//  4OP×6ch(COPL3が担当) + 2OP×6ch(本クラスが担当) という構成になる。
//
//  ch0-8 → chip1_ (port1), ch9-17 → chip2_ (port2) の各COPL2のうち、
//  ch0-5 (4OPペア用スロット) は EnableCh で無効化し、
//  実際に使えるのは各ポートch6-8の3chずつ、計6chのみ。
//
//  ch  6- 8 → chip1_ (port1: アドレス 0x000-0x0FF)
//  ch 15-17 → chip2_ (port2: OffsetPort(port, 0x100) → 0x100-0x1FF)
//  (CSpanDeviceの通しch番号としては前半9ch+後半9ch=18のうち、
//   有効なのはch6-8とch15-17の6chのみ)
//
//  OPL3 固有の初期化:
//    0x001: Wave Select Enable (port1)     → chip1_->setReg(0x01, 0x20)
//    0x101: Wave Select Enable (port2)     → chip2_->setReg(0x01, 0x20)
//    0x105: OPL3 NEW1 = 1 (OPL3 モード有効) → chip2_->setReg(0x05, 0x01)
//           (OffsetPort により 0x100+0x05 = 0x105 に書かれる)
//
//  OPL のキーオン/オフは B0+ch レジスタ (bit5) で完結しており、
//  OPN2 の 0x28 のようなグローバルレジスタではないため、
//  特殊なポートラッパーは不要。OffsetPort だけで正しく動作する。
// ================================================================
class COPL3_2 : public CSpanDevice {
public:
    COPL3_2(IPort* port, int sampleRate)
    {
        offsetPort_ = std::make_unique<OffsetPort>(port, 0x100);
        chip1_ = std::make_unique<COPL2>(port,              sampleRate);
        chip2_ = std::make_unique<COPL2>(offsetPort_.get(), sampleRate);
        // 各ポートch0-5は4OPモード(COPL3)が使用するため無効化する。
        // 残りch6-8(3ch)ずつ、計6chのみ2OPとして使用する。
        for (int i = 0; i < 6; ++i) {
            chip1_->enableCh(static_cast<uint8_t>(i), false);
            chip2_->enableCh(static_cast<uint8_t>(i), false);
        }
        addDevice(chip1_.get()); // ch  0-8 (有効なのはch6-8のみ)
        addDevice(chip2_.get()); // ch 9-17 (有効なのはch15-17のみ)
    }

    uint8_t     getDeviceType() const override { return DEVICE_OPL3_2; }
    std::string getDescriptor() const override { return "OPL3 (YMF262) 2OP residual 6ch"; }

    void init() override {
        chip1_->reset();
        chip2_->reset();
        for (int i = 0; i < 6; ++i) {
            chip1_->enableCh(static_cast<uint8_t>(i), false);
            chip2_->enableCh(static_cast<uint8_t>(i), false);
        }
        // Wave Select Enable: 両ポートに書く
        chip1_->setReg(0x01, 0x20, true); // 0x001
        chip2_->setReg(0x01, 0x20, true); // 0x101 (OffsetPort 経由)
        // OPL3 NEW1: port2 の 0x05 → OffsetPort で 0x105 に書かれる
        chip2_->setReg(0x05, 0x01, true); // 0x105: OPL3 モード有効
    }

    void reset() override { CMultiDevice::reset(); }

private:
    std::unique_ptr<COPL2>      chip1_;
    std::unique_ptr<COPL2>      chip2_;
    std::unique_ptr<OffsetPort> offsetPort_;
};

// ================================================================
//  COPLRhythm: OPL系(OPL/OPL2/OPL3)内蔵リズムチャンネル (5ch)
//  独立デバイスとして生成され、OPLメイン(COPL/COPL2)と物理ポートを
//  共有する。COPNARhythm/COPLLRhythmと異なり、リズム音がROM固定では
//  なく実際のFMオペレータパラメータ(HwPatch)を要求するため、
//  VOICE_PATCH_OPL_RHYという通常のVoicePatchTypeを持ち、パッチ解決は
//  他の直接モードチップと全く同じ経路(resolveTriple→HwBankRegistry)
//  で行われる(2026年7月)。
//
//  実機レジスタ配置(0xBD): bit5=リズムモード有効、bit4=BD/bit3=SD/
//  bit2=TOM/bit1=CYM/bit0=HHのキーオン。楽器番号(0-4)をそのまま
//  ビット位置として使えるよう、論理ch順序を0=HH,1=CYM,2=TOM,3=SD,4=BD
//  に統一している(COPLLRhythmと同じ並び)。
//
//  OPLLと異なり、HH/SD(ch7共有)・TOM/CYM(ch8共有)は「チャンネル全体の
//  後着優先上書き」ではなく、各々が独立したオペレータスロット
//  (モジュレータ/キャリア)を持つため、独立したFM音色パラメータを
//  同時に保持できる。BDのみch6の両オペレータを使う通常の2opボイス。
// ================================================================
class COPLRhythm : public CSoundDevice {
public:
    COPLRhythm(IPort* port, int sampleRate)
        : CSoundDevice(DEVICE_OPL_RHY, 5, port,
                       sampleRate, 72,
                       FNUM_OFFSET,
                       FnumTableType::Fnumber,
                       0x100)
    {}

    std::string getDescriptor() const override { return "OPL Rhythm 5ch"; }
    void init() override { setReg(0xBD, 0x20, true); }

protected:
    // 論理ch(0-4: HH,CYM,TOM,SD,BD) → 物理ch(6-8)対応。
    // 実機レジスタ0xBDのビット位置(bit0=HH..bit4=BD)と論理ch番号が
    // 一致するよう並べてある。
    static constexpr uint8_t kRhythmMapCh[5] = {7, 8, 8, 7, 6};
    // 単一オペレータ楽器(HH/CYM/TOM/SD)が使う、物理ch内のオペレータ
    // index(0=モジュレータスロット/1=キャリアスロット)。BD(index4)は
    // 両方使うためこの表は参照しない。
    static constexpr uint8_t kRhythmOpIndex[5] = {0, 1, 0, 1, 0};
    // COPL::kMap と同じ、物理ch(0-8) → オペレータレジスタオフセット。
    static constexpr uint8_t kSlot[9] = {0, 1, 2, 8, 9, 10, 16, 17, 18};

    static uint8_t ar4(uint8_t v) { return v >> 1; }  // 5bit → 4bit
    static uint8_t tl6(uint8_t v) { return v >> 1; }  // 7bit → 6bit

    // 物理ch+オペレータindex1個分のレジスタを書き込む。carrier=trueなら
    // ベロシティ補正値(VoiceProcessor経由)を使う。単一オペレータ楽器は
    // 常にcarrier=true(単独の発振器として出力されるため)。
    void writeOperatorRegs(uint8_t physCh, uint8_t opIdx, const FmHwOp& o,
                            const VoiceProcessor& proc, bool carrier) {
        uint8_t slot = kSlot[physCh];

        setReg(static_cast<uint16_t>(0x20 + opIdx * 3 + slot),
               static_cast<uint8_t>(
                   ((o.AM & 1) << 7) | ((o.VIB & 1) << 6) |
                   ((o.SR > 0) ? 0 : 0x20) |
                   ((o.KSR & 1) << 4) | (o.MUL & 0xF)));

        if (!carrier) {
            setReg(static_cast<uint16_t>(0x40 + opIdx * 3 + slot),
                   static_cast<uint8_t>((o.KSL << 6) | tl6(o.TL)));
        }

        const uint8_t ar = carrier ? proc.velAR(0) : (o.AR & 0x1F);
        const uint8_t dr = carrier ? proc.velDR(0) : (o.DR & 0x1F);
        setReg(static_cast<uint16_t>(0x60 + opIdx * 3 + slot),
               static_cast<uint8_t>((ar4(ar) << 4) | ar4(dr)));

        const uint8_t sl = carrier ? proc.velSL(0) : (o.SL & 0xF);
        const uint8_t rr = carrier ? proc.velRR(0) : (o.SR ? ar4(o.SR) : o.RR);
        setReg(static_cast<uint16_t>(0x80 + opIdx * 3 + slot),
               static_cast<uint8_t>(((sl & 0xF) << 4) | (rr & 0xF)));

        setReg(static_cast<uint16_t>(0xE0 + opIdx * 3 + slot), o.WS & 0x3);
    }

    void updateVoice(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        uint8_t physCh = kRhythmMapCh[ch];

        if (ch == 4) {
            // BD: 通常の2opボイスとして両オペレータを書く(COPLと同じ
            // isCarrier規則: op1は常にキャリア、ALG=1(並列)ならop0も)。
            for (int i = 0; i < 2; ++i) {
                bool carrier = (i == 1) || ((p.hw.ALG & 1) != 0);
                writeOperatorRegs(physCh, static_cast<uint8_t>(i), p.hwOp[i], s.proc, carrier);
            }
        } else {
            // HH/CYM/TOM/SD: 単一オペレータ(hwOp[0]のみ使用)、常にキャリア
            // 扱い(単独の発振器として出力されるため、isCarrier規則は
            // 適用しない)。
            writeOperatorRegs(physCh, kRhythmOpIndex[ch], p.hwOp[0], s.proc, true);
        }

        // FB/ALG/Panは物理ch単位のレジスタ(0xC0+physCh)。ch7(HH/SD)・
        // ch8(TOM/CYM)は共有するため、後着優先で上書きされる(仕様として
        // 許容、docs/terminology.md参照)。
        setReg(static_cast<uint16_t>(0xC0 + physCh),
               static_cast<uint8_t>(0x30 | ((p.hw.FB & 7) << 1) | (p.hw.ALG & 1)));

        updateVolExp(ch);
    }

    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        uint8_t physCh = kRhythmMapCh[ch];
        uint8_t slot = kSlot[physCh];

        if (ch == 4) {
            for (int i = 0; i < 2; ++i) {
                bool carrier = (i == 1) || ((p.hw.ALG & 1) != 0);
                if (!carrier) continue;
                uint8_t tl = tl6(s.proc.effectiveTL(i));
                setReg(static_cast<uint16_t>(0x40 + i * 3 + slot),
                       static_cast<uint8_t>((p.hwOp[i].KSL << 6) | tl), false);
            }
        } else {
            uint8_t opIdx = kRhythmOpIndex[ch];
            uint8_t tl = tl6(s.proc.effectiveTL(0));
            setReg(static_cast<uint16_t>(0x40 + opIdx * 3 + slot),
                   static_cast<uint8_t>((p.hwOp[0].KSL << 6) | tl), false);
        }
    }

    // リズムパートはノート番号に応じてピッチシフトできる(COPLLRhythmと
    // 同じ設計)。ch7(HH/SD)・ch8(TOM/CYM)は物理チャンネルを共有する
    // ため、2つの楽器が異なるノート番号で発音すると後着優先で上書き
    // される(仕様として許容)。
    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        uint8_t physCh = kRhythmMapCh[ch];
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint8_t b0cur = getReg(static_cast<uint16_t>(0xB0 + physCh)) & 0x20;
        setReg(static_cast<uint16_t>(0xA0 + physCh),
               static_cast<uint8_t>((fnum.fnum >> 1) & 0xFF), true);
        setReg(static_cast<uint16_t>(0xB0 + physCh),
               static_cast<uint8_t>(b0cur | ((fnum.block & 7) << 2) | ((fnum.fnum >> 9) & 1)), true);
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        uint8_t keymask = static_cast<uint8_t>(~(1u << ch));
        uint8_t cur = getReg(0xBD) & keymask;
        setReg(0xBD, static_cast<uint8_t>(cur | 0x20 | (keyOn ? (1u << ch) : 0)), true);
    }

    // パート番号は音色データの hw.ALG (下位3bit) で直接指定する
    // (COPLLRhythmと同じ設計)。該当パートが既に使用中なら0xFF。
    uint8_t queryCh(IMidiCh* /*owner*/, const HwPatch* patch, int mode) override {
        if (!patch) return 0xFF;
        uint8_t num = patch->hw.ALG & 0x7;
        if (num >= 5) return 0xFF;
        bool inuse = (getReg(0xBD) & (1u << num)) != 0;
        return mode ? num : (inuse ? 0xFF : num);
    }
    // リズムパートではCC#7/パン/サステインペダルによるリアルタイム
    // 制御を無視する(COPLLRhythmと同じ判断、重要度が低いため)。
    void updateSustain(uint8_t /*ch*/) override {}
    void updatePanpot(uint8_t /*ch*/) override {}
    void updateTL(uint8_t, uint8_t, uint8_t) override {}
};

} // namespace fitom

namespace fitom {
std::unique_ptr<ISoundDevice> createCOPL(IPort* p, int sr)  { return std::make_unique<COPL>(p, sr); }
std::unique_ptr<ISoundDevice> createCOPL2(IPort* p, int sr) { return std::make_unique<COPL2>(p, sr); }
std::unique_ptr<ISoundDevice> createCOPL3(IPort* p, int sr) { return std::make_unique<COPL3>(p, sr); }
std::unique_ptr<ISoundDevice> createCOPL3_2(IPort* p, int sr) { return std::make_unique<COPL3_2>(p, sr); }
std::unique_ptr<ISoundDevice> createCOPLRhythm(IPort* p, int sr) { return std::make_unique<COPLRhythm>(p, sr); }

// ================================================================
//  フォールバック受け入れ判定
// ================================================================
// COPL (VOICE_PATCH_OPL, YM3526): OPL2形式の音色データを再生できるか。
// 実機OPLは波形選択ハードウェア自体を持たないため、OPL2音色が
// hwOp[].WS!=0 (非サイン波) を使っている場合、その波形情報が失われ
// 音が変わってしまう。WSが全オペレータで0(サイン波)の場合のみ許可する。
// COPL (VOICE_PATCH_OPL, YM3526): OPL2/OPL3(2op)形式の音色データを
// 再生できるか。実機OPLは波形選択ハードウェア自体を持たないため、
// hwOp[].WS!=0 (非サイン波) を使っている場合、その波形情報が失われ
// 音が変わってしまう。WSが全オペレータで0(サイン波)の場合のみ許可する
// (OPL3(2op)のWSは3bit(8波形)まで取りうるが、OPLにとってはOPL2の
//  2bit(4波形)と同様、0以外は全て非サイン波なので同じ判定でよい)。
bool coplAcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& patch) {
    if (sourceVoicePatchType != VOICE_PATCH_OPL2 && sourceVoicePatchType != VOICE_PATCH_OPL3_2)
        return false;
    for (int i = 0; i < 2; ++i) {
        if (patch.hwOp[i].WS != 0) return false;
    }
    return true;
}

// COPL2 (VOICE_PATCH_OPL2): OPL形式は常に安全に再生できる (OPL由来の
// 音色データはWSフィールドを使わない=0のままのため、単なる上位互換)。
// OPL3(2OP)形式(VOICE_PATCH_OPL3_2)は、WSが3bit(8波形)まで使える
// 実機OPL3と異なりOPL2は2bit(4波形)までしか対応しないため、
// 全オペレータでWS<4の場合のみ許可する。
bool copl2AcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& patch) {
    if (sourceVoicePatchType == VOICE_PATCH_OPL) return true;
    if (sourceVoicePatchType == VOICE_PATCH_OPL3_2) {
        for (int i = 0; i < 2; ++i) {
            if (patch.hwOp[i].WS >= 4) return false;
        }
        return true;
    }
    return false;
}

} // namespace fitom
