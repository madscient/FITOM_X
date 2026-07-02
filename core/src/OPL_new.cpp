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
//  COPL3 (OPL3 = YMF262) — CSpanDevice + 2×COPL2 構成
//
//  旧FITOMと同様に CSpanDevice を継承し、内部に2つの COPL2 サブチップを持つ。
//
//  ch  0- 8 → chip1_ (port1: アドレス 0x000-0x0FF)
//  ch  9-17 → chip2_ (port2: OffsetPort(port, 0x100) → 0x100-0x1FF)
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
class COPL3 : public CSpanDevice {
public:
    COPL3(IPort* port, int sampleRate)
    {
        offsetPort_ = std::make_unique<OffsetPort>(port, 0x100);
        chip1_ = std::make_unique<COPL2>(port,              sampleRate);
        chip2_ = std::make_unique<COPL2>(offsetPort_.get(), sampleRate);
        addDevice(chip1_.get()); // ch  0-8
        addDevice(chip2_.get()); // ch 9-17
    }

    uint8_t     getDeviceType() const override { return DEVICE_OPL3; }
    std::string getDescriptor() const override { return "OPL3 (YMF262) 18ch"; }

    void init() override {
        chip1_->reset();
        chip2_->reset();
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

} // namespace fitom

namespace fitom {
std::unique_ptr<ISoundDevice> createCOPL(IPort* p, int sr)  { return std::make_unique<COPL>(p, sr); }
std::unique_ptr<ISoundDevice> createCOPL2(IPort* p, int sr) { return std::make_unique<COPL2>(p, sr); }
std::unique_ptr<ISoundDevice> createCOPL3(IPort* p, int sr) { return std::make_unique<COPL3>(p, sr); }
} // namespace fitom
