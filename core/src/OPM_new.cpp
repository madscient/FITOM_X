// fitom/OPM_new.cpp
// COPM / COPP / COPZ チップドライバ — ISoundDevice ベース移行版
//
// 主な差異 (OPN との比較):
//   - 8ch
//   - 独自 GetFnumber (KeyCode テーブル使用)
//   - DT2 / NFQ / AMS / PMS (HW LFO)
//   - HW LFO は LFORESOURCE で管理 (チップ全体で1リソース)
//   - OPZ: WS / DT3 / REV / EGS / DM0 (FmChipExt)

#include "fitom/ISoundDevice.h"
#include "fitom/FITOMdefine.h"
#include "fitom/Log.h"
#include <cmath>

namespace fitom {

// ================================================================
//  COPM
// ================================================================
class COPM : public CSoundDevice {
public:
    explicit COPM(IPort* port, int sampleRate = 44100, uint8_t devId = DEVICE_OPM)
        : CSoundDevice(devId, 8, port,
                       0, 0,           // fnumMaster/Divide: OPM は独自計算
                       -61,            // origin note O4C+
                       FnumTableType::Fnumber,
                       0x100)
        , lfoOwner_(nullptr)
        , lfoUsed_(0)
        , lfoAmDepth_(0), lfoPmDepth_(0), lfoAmRate_(0), lfoPmRate_(0)
    {
        opCount_ = 4;
        masterTune_ = computeMasterTune(440.0); // デフォルト A4=440Hz
    }

    std::string getDescriptor() const override { return "OPM (YM2151) 8ch"; }

    void init() override {
        setReg(0x01, 0x00, true);
        setReg(0x14, 0x00, true);
    }

    void reset() override {
        CSoundDevice::reset();
        for (int i = 0x20; i < 0xFF; ++i) setReg(static_cast<uint16_t>(i), 0, true);
        lfoOwner_ = nullptr; lfoUsed_ = 0;
    }

protected:
    // OPM 専用 F-number 計算 (KeyCode / KeyFraction 方式)
    ChState::Fnum getFnumber(uint8_t ch, int16_t offset = 0) const override {
        ChState::Fnum ret;
        const auto& s = chState_[ch];
        if (s.lastNote >= 128) return ret;

        int16_t lfoOffset = s.proc.channelLfoActive() ? s.proc.channelLfoValue() : 0;
        int32_t idx = static_cast<int32_t>(s.lastNote) * 64
                    + (noteOffset_ * 64) / 12
                    + masterTune_
                    + offset + lfoOffset;

        // 除算でオクターブを直接求める (ループ方式はオーバーフロー時に誤る)
        int oct = idx / 768;
        idx %= 768;
        if (idx < 0) { --oct; idx += 768; }

        if (oct >= 0 && oct < 8) {
            ret.block = static_cast<uint8_t>(oct);
            ret.fnum  = static_cast<uint16_t>(
                (kKeyCode[idx >> 6] << 6) | (idx & 0x3F));
        }
        return ret;
    }

    void updateVoice(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;

        // FB / ALG (L/R は updatePanpot で設定するため上位2bit は保持)
        uint8_t r20 = (getReg(static_cast<uint16_t>(0x20 + ch)) & 0xC0)
                    | ((p.hw.FB & 7) << 3) | (p.hw.ALG & 7);
        setReg(static_cast<uint16_t>(0x20 + ch), r20);
        setReg(static_cast<uint16_t>(0x38 + ch), 0); // HW LFO disable

        for (int i = 0; i < 4; ++i) {
            const FmHwOp& o = p.hwOp[kMap[i]];
            const FmChipExt& ex = p.ext;

            // DT1 / MUL
            setReg(static_cast<uint16_t>(0x40 + i * 8 + ch),
                   ((o.DT1 & 7) << 4) | (o.MUL & 0xF));

            // TL (キャリアは effectiveTL、モジュレータは固定)
            const bool car_opm = isCarrier(ch, kMap[i]);
            const uint8_t tl = car_opm
                ? s.proc.effectiveTL(kMap[i])
                : o.TL;
            setReg(static_cast<uint16_t>(0x60 + i * 8 + ch), tl);

            // KSR / AR / DM0
            const bool dm0 = (ex.DM0 != 0);
            const uint8_t ar_opm = car_opm ? s.proc.velAR(kMap[i]) : (o.AR & 0x1F);
            setReg(static_cast<uint16_t>(0x80 + i * 8 + ch),
                   ((o.KSR & 3) << 6) | (ar_opm >> 2) | (dm0 ? 0x20 : 0));

            // AM / DR
            const uint8_t dr_opm = car_opm ? s.proc.velDR(kMap[i]) : (o.DR & 0x1F);
            setReg(static_cast<uint16_t>(0xA0 + i * 8 + ch),
                   ((o.AM & 1) << 7) | (dr_opm >> 2));

            // DT2 / SR (FmHwOp では SR = OPM の "D2R")
            const uint8_t sr_opm = car_opm ? s.proc.velSR(kMap[i]) : (o.SR & 0x1F);
            setReg(static_cast<uint16_t>(0xC0 + i * 8 + ch),
                   (dm0 ? 0 : ((o.DT2 & 3) << 6)) | (sr_opm >> 2));

            // SL / RR
            const uint8_t sl_opm = car_opm ? s.proc.velSL(kMap[i]) : (o.SL & 0xF);
            const uint8_t rr_opm = car_opm ? s.proc.velRR(kMap[i]) : (o.RR & 0xF);
            setReg(static_cast<uint16_t>(0xE0 + i * 8 + ch),
                   ((sl_opm >> 3) << 4) | (rr_opm >> 3));
        }

        // ch7: ノイズ有効チェック (ALG_EXT bit = 1 → NFQ を設定)
        if (ch == 7) {
            if (p.ext.ALG_EXT & 1) {
                setReg(0x0F, static_cast<uint8_t>(0x80 | p.hw.NFQ));
            } else {
                setReg(0x0F, 0);
            }
        }

        updatePanpot(ch);
        updateVolExp(ch);
    }

    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        for (int i = 0; i < 4; ++i) {
            if (!isCarrier(ch, kMap[i])) continue;
            setReg(static_cast<uint16_t>(0x60 + i * 8 + ch),
                   s.proc.effectiveTL(kMap[i]));
        }
    }

    void updateTL(uint8_t ch, uint8_t op, uint8_t tl) override {
        setReg(static_cast<uint16_t>(0x60 + kMap[op] * 8 + ch), tl);
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint8_t kc = static_cast<uint8_t>((fnum.block << 4) | (fnum.fnum >> 6));
        uint8_t kf = static_cast<uint8_t>(fnum.fnum << 2);
        setReg(static_cast<uint16_t>(0x28 + ch), kc, false);
        setReg(static_cast<uint16_t>(0x30 + ch),
               static_cast<uint8_t>(kf | (getReg(static_cast<uint16_t>(0x30 + ch)) & 0x03)),
               false);
    }

    void updatePanpot(uint8_t ch) override {
        int8_t pan = chState_[ch].panpot;
        uint8_t chena = (pan > 20) ? 0x80 : (pan < -20) ? 0x40 : 0xC0;
        uint8_t cur = getReg(static_cast<uint16_t>(0x20 + ch)) & 0x3F;
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>(cur | chena), true);
    }

    void updateSustain(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        for (int i = 0; i < 4; ++i) {
            if (!isCarrier(ch, kMap[i])) continue; // モジュレータは対象外
            const FmHwOp& o = p.hwOp[kMap[i]];
            uint8_t rr = s.sustain ? 4 : (o.RR >> 3);
            setReg(static_cast<uint16_t>(0xE0 + i * 8 + ch),
                   static_cast<uint8_t>(((o.SL >> 3) << 4) | (rr & 0xF)));
        }
    }

    // CC#120 (All Sound Off): 全 OP の RR を最大値にして急速減衰させてから noteOff。
    // (updateKey 内の sustain 再トリガー時ダンプとは別の独立した処理)
    void forceDamp(uint8_t ch) override {
        if (ch >= maxChs_) return;
        const auto& s = chState_[ch];
        if (!s.isActive()) return;
        const HwPatch& p = s.hwPatch;
        for (int i = 0; i < 4; ++i) {
            if (!isCarrier(ch, kMap[i])) continue; // モジュレータは対象外
            const FmHwOp& o = p.hwOp[kMap[i]];
            setReg(static_cast<uint16_t>(0xE0 + i * 8 + ch),
                   static_cast<uint8_t>(((o.SL >> 3) << 4) | 0xF)); // RR=15
        }
        noteOff(ch);
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        if (keyOn && s.sustain) {
            // ForceDamp: RR を最大に
            for (int i = 0; i < 4; ++i) {
                const FmHwOp& o = p.hwOp[kMap[i]];
                setReg(static_cast<uint16_t>(0xE0 + i * 8 + ch),
                       static_cast<uint8_t>(((o.SL >> 3) << 4) | 0xF));
            }
        }
        if (!keyOn) updateSustain(ch);
        setReg(0x08, static_cast<uint8_t>((keyOn ? 0x78 : 0) | ch), true);
    }

    // ノイズ専用チャンネル (ch7) の優先割り当て
    uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode) override {
        if (patch && (patch->ext.ALG_EXT & 1)) {
            // ノイズ有効 → ch7 専用
            const auto& s7 = chState_[7];
            bool avail = mode ? s7.isEmpty() : s7.isEnabled();
            return avail ? 7 : 0xFF;
        }
        return CSoundDevice::queryCh(owner, patch, mode);
    }

    // HW LFO (PM) — チップ全体で1リソース
    void enablePM(uint8_t ch, bool on) override {
        const auto& s = chState_[ch];
        if (on && s.owner) {
            if (lfoOwner_ != s.owner) {
                clearLfoUsers();
                lfoOwner_ = s.owner;
            }
            lfoUsed_ |= (1u << ch);
            setReg(static_cast<uint16_t>(0x38 + ch),
                   static_cast<uint8_t>((getReg(static_cast<uint16_t>(0x38 + ch)) & 0x07)
                   | ((s.hwPatch.hw.PMS & 7) << 4)));
        } else {
            setReg(static_cast<uint16_t>(0x38 + ch),
                   static_cast<uint8_t>(getReg(static_cast<uint16_t>(0x38 + ch)) & 0x07));
            lfoUsed_ &= ~(1u << ch);
            if (!lfoUsed_) lfoOwner_ = nullptr;
        }
    }

    void enableAM(uint8_t ch, bool on) override {
        const auto& s = chState_[ch];
        if (on && s.owner) {
            if (lfoOwner_ != s.owner) {
                clearLfoUsers();
                lfoOwner_ = s.owner;
            }
            lfoUsed_ |= (1u << ch);
            setReg(static_cast<uint16_t>(0x38 + ch),
                   static_cast<uint8_t>((getReg(static_cast<uint16_t>(0x38 + ch)) & 0x70)
                   | (s.hwPatch.hw.AMS & 3)));
        } else {
            setReg(static_cast<uint16_t>(0x38 + ch),
                   static_cast<uint8_t>(getReg(static_cast<uint16_t>(0x38 + ch)) & 0x70));
            lfoUsed_ &= ~(1u << ch);
            if (!lfoUsed_) lfoOwner_ = nullptr;
        }
    }

    void setPMDepth(uint8_t, uint8_t dep) override {
        if (lfoPmDepth_ != dep) { lfoPmDepth_ = dep; setReg(0x19, 0x80 | dep); }
    }
    void setAMDepth(uint8_t, uint8_t dep) override {
        if (lfoAmDepth_ != dep) { lfoAmDepth_ = dep; setReg(0x19, dep & 0x7F); }
    }
    void setPMRate(uint8_t, uint8_t rate) override {
        if (lfoPmRate_ != rate) { lfoPmRate_ = rate; setReg(0x18, static_cast<uint8_t>(rate << 1)); }
    }
    void setAMRate(uint8_t, uint8_t rate) override {
        if (lfoAmRate_ != rate) { lfoAmRate_ = rate; setReg(0x18, static_cast<uint8_t>(rate << 1)); }
    }

protected:
    static const uint8_t kMap[4];
private:
    static const uint8_t kKeyCode[12];

    IMidiCh* lfoOwner_;
    uint32_t lfoUsed_;
    uint8_t  lfoAmDepth_, lfoPmDepth_, lfoAmRate_, lfoPmRate_;
    int16_t  masterTune_ = 0;

    bool isCarrier(uint8_t ch, int op) const {
        uint8_t alg = chState_[ch].hwPatch.hw.ALG & 7;
        return (kCarrierMask[alg] >> op) & 1;
    }
    void clearLfoUsers() {
        for (int i = 0; i < 8; ++i) {
            if (lfoUsed_ & (1u << i))
                setReg(static_cast<uint16_t>(0x38 + i),
                       static_cast<uint8_t>(getReg(static_cast<uint16_t>(0x38 + i)) & 0x03));
        }
        lfoUsed_ = 0;
    }

    // masterTune_: note=69(A4) → KC=0x4C(oct4,A), KF=0 になる基準オフセット
    // 検証: note=69, noteOffset=-61 のとき
    //   idx = 69*64 + (-61*64)/12 + (-442) = 4416 - 326 - 442 = 3648
    //   oct = 3648/768 = 4, idx_norm = 576 = 9*64+0
    //   KC = (4<<4)|kKeyCode[9] = (4<<4)|12 = 0x4C = A4 ✓
    static constexpr int16_t kMasterTuneBase = -442;

    // pitchHz から masterTune_ を計算する
    // 440Hz 基準で ±20Hz (約 ±77cent) の可変範囲
    static int16_t computeMasterTune(double pitchHz) {
        return static_cast<int16_t>(kMasterTuneBase
            + std::round(768.0 * std::log2(pitchHz / 440.0)));
    }

    // masterTune_ を更新し、発音中チャンネルの F-number を再計算する
    void onMasterPitchChanged(double pitchHz) {
        masterTune_ = computeMasterTune(pitchHz);
        for (int ch = 0; ch < maxChs_; ++ch)
            if (chState_[ch].isActive())
                updateFnumber(static_cast<uint8_t>(ch), true);
    }
};

const uint8_t COPM::kMap[4]     = {0, 2, 1, 3};
const uint8_t COPM::kKeyCode[12]= {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};

// ================================================================
//  COPP (OPP = YM2164) — OPM のデバイス ID 違いのみ
// ================================================================
class COPP : public COPM {
public:
    explicit COPP(IPort* port, int sampleRate = 44100)
        : COPM(port, sampleRate, DEVICE_OPP) {}
    std::string getDescriptor() const override { return "OPP (YM2164) 8ch"; }
    void init() override {
        for (int i = 0; i < 8; ++i) setReg(static_cast<uint16_t>(i), 0x10, true);
    }
};

// ================================================================
//  COPZ (OPZ = YM2414) — WS / DT3 / REV / EGS 拡張あり
// ================================================================
class COPZ : public COPM {
public:
    explicit COPZ(IPort* port, int sampleRate = 44100)
        : COPM(port, sampleRate, DEVICE_OPZ) {}
    std::string getDescriptor() const override { return "OPZ (YM2414) 8ch"; }
    void init() override {
        for (int i = 0; i < 8; ++i) setReg(static_cast<uint16_t>(i), 0x10, true);
        setReg(0x09, 0x00, true);
        setReg(0x0A, 0x04, true);
        setReg(0x0F, 0x00, true);
        setReg(0x14, 0x70, true);
        setReg(0x15, 0x01, true);
    }

    void updatePanpot(uint8_t ch) override {
        int8_t pan = chState_[ch].panpot;
        uint8_t chena = (pan > 20) ? 0x80 : (pan < -20) ? 0x40 : 0xC0;
        uint8_t mono  = (pan <= 20 && pan >= -20) ? 0x01 : 0x00;
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>((getReg(static_cast<uint16_t>(0x20 + ch)) & 0x3F) | chena), false);
        setReg(static_cast<uint16_t>(0x30 + ch),
               static_cast<uint8_t>((getReg(static_cast<uint16_t>(0x30 + ch)) & 0xFE) | mono), false);
    }

    void updateVoice(uint8_t ch) override {
        const HwPatch& p = chState_[ch].hwPatch;
        const FmChipExt& ex = p.ext;
        // OPZ 固有: WS / DT3 を最初に書く
        for (int i = 0; i < 4; ++i) {
            const FmHwOp& o = p.hwOp[kMap[i]];
            setReg(static_cast<uint16_t>(0x40 + i * 8 + ch),
                   static_cast<uint8_t>(0x80 | ((o.WS & 7) << 4) | (ex.DT3 & 0xF)));
            setReg(static_cast<uint16_t>(0xC0 + i * 8 + ch), 0x20);
        }
        COPM::updateVoice(ch); // 共通部分を呼ぶ
    }
    // kMap は COPM::kMap (protected) をそのまま継承して使う
};

} // namespace fitom

namespace fitom {
std::unique_ptr<ISoundDevice> createCOPM(IPort* p, int sr) { return std::make_unique<COPM>(p, sr); }
std::unique_ptr<ISoundDevice> createCOPP(IPort* p, int sr) { return std::make_unique<COPP>(p, sr); }
std::unique_ptr<ISoundDevice> createCOPZ(IPort* p, int sr) { return std::make_unique<COPZ>(p, sr); }
} // namespace fitom
