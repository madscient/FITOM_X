// fitom/OPN2_new.cpp
// COPNA / COPN2: OPN を 2 ポート (SplitPort) でまとめた 6ch ドライバ
//
// B-2 対応:
//   チップドライバは SplitPort を受け取り、
//   addr < 0x100  → port1 (ch0-2)
//   addr >= 0x100 → port2 (ch3-5)
//   と書くだけでよい。HW/エミュレーターの差異は SplitPort が吸収する。
//
// エミュレーター (FmEnginePort):
//   FmEnginePort は addr 上位バイトを port 引数として渡すため、
//   SplitPort を経由しない 1 ポートで 6ch を処理できる。
//   DeviceFactory がエミュレーター時は SplitPort を作らず FmEnginePort を直接渡す。
//
// HW (SPFM 等):
//   port1: HWPort(slot=N)   → OPN の CS0 (ch0-2)
//   port2: HWPort(slot=N+1) → OPN の CS1 (ch3-5)
//   を SplitPort でまとめて COPNA に渡す。

#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/Log.h"
#include <memory>

namespace fitom {

// ================================================================
//  COPNA: OPN2/OPNA 6ch ドライバ
//  単一の IPort (FmEnginePort または SplitPort) を受け取る
// ================================================================
class COPNA : public CSoundDevice {
public:
    explicit COPNA(IPort* port, int sampleRate = 44100,
                   uint8_t devId = DEVICE_OPNA)
        : CSoundDevice(devId, 6, port,
                       sampleRate, 144,   // fnumMaster/divide: OPN 系標準
                       -576,
                       FnumTableType::Fnumber,
                       0x200)             // 0x000〜0x0FF + 0x100〜0x1FF
        , lfoUsed_(0), lfoAmDepth_(0), lfoPmDepth_(0)
        , lfoAmRate_(0), lfoPmRate_(0)
        , sampleRate_(sampleRate)
    {
        opCount_ = 4;
    }

    std::string getDescriptor() const override {
        return std::string(getDeviceType() == DEVICE_OPN2 ? "OPN2 (YM2612)" : "OPNA (YM2608)")
               + " 6ch via SplitPort";
    }

    void init() override {
        // 全チャンネルサイレント
        port_->write(0x28, 0x00, true);  // all key off
        port_->write(0x27, 0x30, true);  // ch3 normal mode, timer stop
        // LFO 無効
        port_->write(0x22, 0x00, true);
    }

    void reset() override {
        CSoundDevice::reset();
        for (int i = 0x30; i < 0xB0; ++i) {
            port_->write(static_cast<uint16_t>(i),       0, true);
            port_->write(static_cast<uint16_t>(0x100+i), 0, true);
        }
        for (int ch = 0; ch < 3; ++ch) {
            port_->write(static_cast<uint16_t>(0xB4 + ch),       0xC0, true);
            port_->write(static_cast<uint16_t>(0x100 + 0xB4 + ch), 0xC0, true);
        }
        lfoUsed_ = 0;
    }

protected:
    // ch0-2: 0x000〜0x0FF  / ch3-5: 0x100〜0x1FF
    // SplitPort がアドレスに従って port1/port2 に振り分ける
    uint16_t portBase(uint8_t ch) const {
        return (ch < 3) ? 0x000 : 0x100;
    }
    uint8_t localCh(uint8_t ch) const { return ch % 3; }

    static const uint8_t kOpMap[4];  // {0, 8, 4, 12}

    bool isCarrier(uint8_t ch, int op) const {
        return (kCarrierMask[chState_[ch].hwPatch.hw.ALG & 7] >> op) & 1;
    }

    void updateVoice(uint8_t ch) override {
        const auto& s   = chState_[ch];
        const HwPatch& p = s.hwPatch;
        uint16_t base    = portBase(ch);
        uint8_t  lch     = localCh(ch);

        // FB / ALG
        port_->write(static_cast<uint16_t>(base + 0xB0 + lch),
                     static_cast<uint16_t>(((p.hw.FB & 7) << 3) | (p.hw.ALG & 7)));
        // L/R / AMS / PMS (デフォルト: 両方有効)
        port_->write(static_cast<uint16_t>(base + 0xB4 + lch), 0xC0);

        for (int i = 0; i < 4; ++i) {
            const FmHwOp& o = p.hwOp[kOpMap[i]];
            bool carrier = isCarrier(ch, kOpMap[i]);

            port_->write(static_cast<uint16_t>(base + 0x30 + kOpMap[i] + lch),
                         static_cast<uint16_t>(((o.DT1 & 7) << 4) | (o.MUL & 0xF)));

            uint8_t tl = carrier ? s.proc.effectiveTL(kOpMap[i]) : o.TL;
            port_->write(static_cast<uint16_t>(base + 0x40 + kOpMap[i] + lch),
                         static_cast<uint16_t>(tl & 0x7F));

            port_->write(static_cast<uint16_t>(base + 0x50 + kOpMap[i] + lch),
                         static_cast<uint16_t>(((o.KSR & 3) << 6) | (o.AR & 0x1F)));
            port_->write(static_cast<uint16_t>(base + 0x60 + kOpMap[i] + lch),
                         static_cast<uint16_t>(((o.AM & 1) << 7) | (o.DR & 0x1F)));
            port_->write(static_cast<uint16_t>(base + 0x70 + kOpMap[i] + lch),
                         static_cast<uint16_t>(o.SR & 0x1F));
            port_->write(static_cast<uint16_t>(base + 0x80 + kOpMap[i] + lch),
                         static_cast<uint16_t>(((o.SL & 0xF) << 4) | (o.RR & 0xF)));
            port_->write(static_cast<uint16_t>(base + 0x90 + kOpMap[i] + lch),
                         static_cast<uint16_t>(o.EGT & 0xF));
        }
        updatePanpot(ch);
    }

    void updateVolExp(uint8_t ch) override {
        uint16_t base = portBase(ch);
        uint8_t  lch  = localCh(ch);
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        for (int i = 0; i < 4; ++i) {
            if (!isCarrier(ch, kOpMap[i])) continue;
            port_->write(static_cast<uint16_t>(base + 0x40 + kOpMap[i] + lch),
                         static_cast<uint16_t>(s.proc.effectiveTL(kOpMap[i]) & 0x7F));
        }
    }

    void updateTL(uint8_t ch, uint8_t op, uint8_t tl) override {
        port_->write(static_cast<uint16_t>(portBase(ch) + 0x40 + kOpMap[op] + localCh(ch)),
                     static_cast<uint16_t>(tl & 0x7F));
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint16_t base = portBase(ch);
        uint8_t  lch  = localCh(ch);
        // MSB 先書き
        port_->write(static_cast<uint16_t>(base + 0xA4 + lch),
                     static_cast<uint16_t>((fnum.block << 3) | ((fnum.fnum >> 8) & 7)));
        port_->write(static_cast<uint16_t>(base + 0xA0 + lch),
                     static_cast<uint16_t>(fnum.fnum & 0xFF));
    }

    void updatePanpot(uint8_t ch) override {
        int8_t  pan   = chState_[ch].panpot;
        uint8_t lr    = (pan > 20) ? 0x40 : (pan < -20) ? 0x80 : 0xC0;
        port_->write(static_cast<uint16_t>(portBase(ch) + 0xB4 + localCh(ch)),
                     static_cast<uint16_t>(lr));
    }

    void updateSustain(uint8_t ch) override {
        uint16_t base = portBase(ch);
        uint8_t  lch  = localCh(ch);
        bool     sus  = chState_[ch].sustain;
        const HwPatch& p = chState_[ch].hwPatch;
        for (int i = 0; i < 4; ++i) {
            const FmHwOp& o = p.hwOp[kOpMap[i]];
            uint8_t rr = sus ? 4u : o.RR;
            port_->write(static_cast<uint16_t>(base + 0x80 + kOpMap[i] + lch),
                         static_cast<uint16_t>(((o.SL & 0xF) << 4) | (rr & 0xF)));
        }
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        // OPN2/OPNA: 0x28 は常に port1 (0x000 側) に書く
        // ch0-2 → value & 0x03、ch3-5 → value | 0x04
        uint8_t slotMask = keyOn ? 0xF0u : 0x00u;
        uint8_t chBits   = static_cast<uint8_t>((ch % 3) | ((ch >= 3) ? 0x04 : 0x00));
        port_->write(0x28, static_cast<uint16_t>(slotMask | chBits));
    }

    // HW LFO (PM) — チップ全体で 1 リソース
    void enablePM(uint8_t ch, bool on) override {
        if (on) {
            lfoUsed_ |= (1u << ch);
        } else {
            lfoUsed_ &= ~(1u << ch);
        }
        // LFO enable/disable を 0x22 に書く
        port_->write(0x22,
            static_cast<uint16_t>(lfoUsed_ ? (0x08 | (lfoPmRate_ >> 4)) : 0x00));
    }
    void setPMDepth(uint8_t, uint8_t dep) override {
        lfoPmDepth_ = dep;
        // AMS/PMS を B4 レジスタに反映（要: 各 ch の updatePanpot を再呼び出し）
    }
    void setPMRate(uint8_t, uint8_t rate) override {
        lfoPmRate_ = rate;
        port_->write(0x22, static_cast<uint16_t>(lfoUsed_ ? (0x08 | (rate >> 4)) : 0x00));
    }

private:
    uint32_t lfoUsed_;
    uint8_t  lfoAmDepth_, lfoPmDepth_, lfoAmRate_, lfoPmRate_;
    int      sampleRate_;
};

const uint8_t COPNA::kOpMap[4] = {0, 8, 4, 12};

// ================================================================
//  COPN2: OPN2 (YM2612) — devId 違いのみ
// ================================================================
class COPN2 : public COPNA {
public:
    explicit COPN2(IPort* port, int sampleRate = 44100)
        : COPNA(port, sampleRate, DEVICE_OPN2) {}
    std::string getDescriptor() const override {
        return "OPN2 (YM2612) 6ch via SplitPort";
    }
};

// ================================================================
//  ファクトリ関数 (DeviceFactory.cpp から呼ばれる)
//
//  エミュレーター: extraPort == port → SplitPort 不要 (FmEnginePort が port 引数で解決)
//  HW:             port=port1, extraPort=port2 → SplitPort を生成して COPNA に渡す
//
//  SplitPort の寿命は CFITOM が管理する (splitPorts_ ベクタで保持)
// ================================================================
// createCOPNA / createCOPN2:
//   p2 == nullptr → 1ポートモード (エミュレーター: FmEnginePort が port 引数で解決)
//   p2 != nullptr → CFITOM::initDevices が SplitPort を生成して p1 として渡してくる
//                   (CFITOM::splitPorts_ が寿命を管理; ここでは raw new しない)
std::unique_ptr<ISoundDevice> createCOPNA(IPort* p1, IPort* p2, int sr)
{
    if (p2 == nullptr) {
        // エミュレーター or 1ポートHW: p1 = FmEnginePort or HWPort(slot0)
        FITOM_LOG_INFO("createCOPNA: single-port mode");
        return std::make_unique<COPNA>(p1, sr, DEVICE_OPNA);
    }
    // p2 は CFITOM が生成した SplitPort.get() が渡ってくる
    // ここでは p2 を使わず p1 (= SplitPort*) をそのまま使う
    // (CFITOM::initDevices で SplitPort(port1, port2) を生成し
    //  extraPort = splitPorts_.back().get() として渡している)
    FITOM_LOG_INFO("createCOPNA: SplitPort mode (HW) via CFITOM");
    return std::make_unique<COPNA>(p1, sr, DEVICE_OPNA);
}

std::unique_ptr<ISoundDevice> createCOPN2(IPort* p1, IPort* p2, int sr)
{
    if (p2 == nullptr) {
        return std::make_unique<COPN2>(p1, sr);
    }
    return std::make_unique<COPN2>(p1, sr);
}

} // namespace fitom
