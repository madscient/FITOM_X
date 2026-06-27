#include "fitom/SccWaveData.h"
// fitom/PSG_new.cpp
// CSSG / CDCSG / CSCC チップドライバ — ISoundDevice ベース移行版
//
// PSG 系の共通特性:
//   - FnumTableType::SSG (period テーブル) または TonePeriod
//   - 音量は線形 4bit (0=最大, 15=最小 ← OPM/OPN と逆)
//   - TL は 7bit で保持するが、writeReg 時に 4bit に変換
//   - ノイズチャンネルは ALG で選択 (ALG=0:トーン/1:ノイズ/2:両方/3:MIX)
//   - DCSG (SN76489): 非対称アドレス (writeRaw のみ)
//   - SCC: 波形テーブル書き込みが必要

#include "fitom/ISoundDevice.h"
#include "fitom/Log.h"
#include <cstring>
#include <algorithm>

namespace fitom {

// ================================================================
//  CPSGBase: SSG / PSG 共通基底
// ================================================================
class CPSGBase : public CSoundDevice {
public:
    CPSGBase(uint8_t devId, IPort* port, int regSize,
             uint8_t maxChs, int sampleRate,
             uint8_t numOps = 2,
             int noteOffset = FNUM_OFFSET,
             FnumTableType fnumType = FnumTableType::SSG)
        : CSoundDevice(devId, maxChs, port,
                       sampleRate, 1,
                       noteOffset, fnumType, regSize)
    {
        opCount_ = numOps;
        std::fill(lfoTL_, lfoTL_ + MAX_CHS, 64);
    }

protected:
    uint8_t lfoTL_[MAX_CHS]; // ソフトLFO の基準TL (振幅変調用)

    // PSG 系共通: キーオン/オフはノイズ/トーン enable の切り替え
    // チャンネルごとに MixReg (reg 0x07) の対応ビットを制御
    void updateKey(uint8_t ch, bool keyOn) override {
        if (keyOn) {
            // ミックスレジスタ: トーンは bit0-2、ノイズは bit3-5
            const HwPatch& p = chState_[ch].hwPatch;
            uint8_t mixBit = computeMixBit(ch, p);
            uint8_t cur = getReg(0x07) & ~((0x09u) << ch);
            setReg(0x07, static_cast<uint8_t>(cur | (mixBit << ch)), true);
        } else {
            // 両方 disable
            uint8_t cur = getReg(0x07) | ((0x09u) << ch);
            setReg(0x07, cur, true);
        }
    }

    void updateVolExp(uint8_t ch) override {
        if (chState_[ch].hwPatch.hwOp[0].EGT & 0x08) return; // HW EG 使用中
        uint8_t evol = chState_[ch].proc.effectiveTL(0);
        // PSG は音量が逆 (0=最大, 15=最小)。7bit → 4bit
        uint8_t vol = 15u - ((evol * 15u) / 127u);
        setReg(static_cast<uint16_t>(0x08 + ch), vol & 0x0F, false);
    }

    void updateTL(uint8_t ch, uint8_t op, uint8_t lev) override {
        switch (op) {
        case 0: // 振幅 LFO
            lfoTL_[ch] = lev;
            updateVolExp(ch); // lfoTL を加味して再計算
            break;
        case 1: // ノイズ周波数 LFO
        {
            const HwPatch& p = chState_[ch].hwPatch;
            if ((p.hw.ALG & 3) == 1 || (p.hw.ALG & 3) == 2) {
                int16_t frq = static_cast<int16_t>(lev) - 64
                            + static_cast<int16_t>(p.hw.NFQ);
                frq = std::clamp<int16_t>(frq, 0, 31);
                setReg(0x06, static_cast<uint8_t>(frq), false);
            }
            break;
        }
        }
    }

    void updateSustain(uint8_t /*ch*/) override {}
    void updatePanpot(uint8_t /*ch*/) override {}

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint8_t oct = fnum.block;
        uint16_t etp = fnum.fnum >> (oct + 3);
        setReg(static_cast<uint16_t>(ch * 2),     static_cast<uint8_t>(etp & 0xFF), false);
        setReg(static_cast<uint16_t>(ch * 2 + 1), static_cast<uint8_t>(etp >> 8),   false);
    }

    void updateVoice(uint8_t ch) override {
        lfoTL_[ch] = 64;
        const HwPatch& p = chState_[ch].hwPatch;
        uint8_t mixBit = computeMixBit(ch, p);
        uint8_t cur = getReg(0x07) & ~((0x09u) << ch);
        setReg(0x07, static_cast<uint8_t>(cur | (mixBit << ch)), true);

        // ノイズ周波数
        if ((p.hw.ALG & 3) == 1 || (p.hw.ALG & 3) == 2) {
            setReg(0x06, p.hw.NFQ & 0x1F, true);
        }

        // HW エンベロープ (EGT bit3 = 使用フラグ)
        // AY-3-8910 / YM2149: HW EG 使用時はすべてのベロシティ感度を無効化する。
        // HW EG はチップ固有のエンベロープ形状 (0xD = attack+decay 等) で制御され、
        // ソフトウェア側からの EG レート補正は意味をなさないためレジスタ値そのまま。
        if (p.hwOp[0].EGT & 0x08) {
            setReg(static_cast<uint16_t>(0x08 + ch),
                   static_cast<uint8_t>((getReg(static_cast<uint16_t>(0x08 + ch)) & 0xE0)
                   | 0x10 | (p.hwOp[0].EGT & 0xF)), true);
            setReg(0x0B, static_cast<uint8_t>(((p.hwOp[0].SL << 4) & 0xF0) | (p.hwOp[0].RR & 0xF)), true);
            setReg(0x0C, static_cast<uint8_t>(((p.hwOp[0].DR << 4) & 0xF0) | (p.hwOp[0].SR & 0xF)), true);
            setReg(0x0D, static_cast<uint8_t>(p.hwOp[0].EGT & 0xF), true);
        }
    }

    // ノイズ/トーン ALG に基づくミックスビット計算
    // ミックスレジスタ bit: トーン=bit[ch], ノイズ=bit[ch+3]
    // 0=有効, 1=無効 (Active Low)
    static uint8_t computeMixBit(uint8_t ch, const HwPatch& p) {
        switch (p.hw.ALG & 3) {
        case 0: return 0x01; // トーンのみ (noise disable, tone enable → bit=0b001)
        case 1: return 0x08; // ノイズのみ (ビット: 0b1000)
        case 2: return 0x00; // 両方有効
        case 3: return 0x09; // 両方無効 (実質消音)
        default: return 0x09;
        }
    }

    // ノイズ有効時は ch2 を優先割り当て
    uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode) override {
        if (patch && (patch->hw.ALG & 3) != 0) {
            const auto& s2 = chState_[2];
            bool avail = mode ? s2.isEmpty() : s2.isEnabled();
            return avail ? 2 : 0xFF;
        }
        return CSoundDevice::queryCh(owner, patch, mode);
    }
};

// ================================================================
//  CSSG (YM2149 SSG / AY-3-8910)
// ================================================================
class CSSG : public CPSGBase {
public:
    CSSG(IPort* port, int sampleRate, uint8_t devId = DEVICE_SSG)
        : CPSGBase(devId, port, 0x20, 3, sampleRate) {}

    std::string getDescriptor() const override { return "SSG (YM2149) 3ch"; }
    void init() override { setReg(0x07, 0x3F, true); } // 全チャンネル無効化

    void updateKey(uint8_t ch, bool keyOn) override {
        const HwPatch& p = chState_[ch].hwPatch;
        if (p.hwOp[0].EGT & 0x08) {
            // HW EG: キーオンはエンベロープ開始で代用
            if (keyOn) updateVoice(ch);
            else       setReg(static_cast<uint16_t>(0x08 + ch),
                              static_cast<uint8_t>(getReg(static_cast<uint16_t>(0x08 + ch)) & 0xE0), true);
        } else {
            CPSGBase::updateKey(ch, keyOn);
        }
    }
};

// ================================================================
//  CDCSG (SN76489 DCSG)
//  レジスタアクセスは writeRaw (アドレスなし、常にポート0)
//  周波数はトーン期間 (TonePeriod)、4ch (tone*3 + noise)
// ================================================================
class CDCSG : public CPSGBase {
public:
    CDCSG(IPort* port, int sampleRate)
        : CPSGBase(DEVICE_DCSG, port, 0x10, 4, sampleRate,
                   2, -576, FnumTableType::TonePeriod)
        , prevNoise_(0)
    {
        std::fill(prevVol_,  prevVol_  + 4, 0u);
        std::fill(prevFreq_, prevFreq_ + 4, 0u);
        // ノイズチャンネル (ch3) は手動割り当てのみ
        chState_[3].autoAssign = false;
    }

    std::string getDescriptor() const override { return "DCSG (SN76489) 4ch"; }

    void init() override {
        for (int ch = 0; ch < 3; ++ch) {
            port_->writeRaw(0, static_cast<uint16_t>(0x80 | (ch * 32)));
            port_->writeRaw(0, 0x00);
            port_->writeRaw(0, static_cast<uint16_t>(0x90 | (ch * 32) | 0xF));
        }
        port_->writeRaw(0, 0xE0);
        port_->writeRaw(0, 0xFF);
    }

protected:
    uint8_t  prevNoise_;
    uint8_t  prevVol_[4];
    uint16_t prevFreq_[4];

    void updateVolExp(uint8_t ch) override {
        uint8_t evol = chState_[ch].proc.effectiveTL(0);
        // DCSG も逆: 0=最大, 15=最小
        uint8_t vol = 15u - ((evol * 15u) / 127u);
        if (prevVol_[ch] == vol) return;
        prevVol_[ch] = vol;
        if (ch < 3) port_->writeRaw(0, static_cast<uint16_t>(0x90 | (ch * 32) | (vol & 0xF)));
        else        port_->writeRaw(0, static_cast<uint16_t>(0xF0 | (vol & 0xF)));
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        if (ch >= 3) return;
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint8_t oct = fnum.block;
        uint16_t etp = fnum.fnum >> (oct + 3);
        if (prevFreq_[ch] == etp) return;
        prevFreq_[ch] = etp;
        port_->writeRaw(0, static_cast<uint16_t>(0x80 | (ch * 32) | (etp & 0xF)));
        port_->writeRaw(0, static_cast<uint16_t>((etp >> 4) & 0x3F));
    }

    void updateVoice(uint8_t ch) override {
        if (ch == 3) {
            const HwPatch& p = chState_[3].hwPatch;
            if (p.hw.ALG == 1) {
                prevNoise_ = static_cast<uint8_t>(0xE0 | ((p.hw.FB & 1) << 2) | (p.hw.NFQ & 3));
                port_->writeRaw(0, prevNoise_);
            }
        }
        // 音量は updateVolExp で書く
    }

    void updateKey(uint8_t /*ch*/, bool /*keyOn*/) override {
        // DCSG はキーオン/オフなし (音量 0 で代用)
    }

    uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode) override {
        if (patch && patch->hw.ALG == 1) {
            const auto& s3 = chState_[3];
            return (mode ? s3.isEmpty() : s3.isEnabled()) ? 3 : 0xFF;
        }
        return CSoundDevice::queryCh(owner, patch, mode);
    }
};

// ================================================================
//  CSCC (SCC / SCC+)
//  SCC は 5ch、各チャンネルに 32 バイトの波形テーブルがある
//
//  B-1 対応:
//    FmHwOp::WS = 波形番号 (0〜127) → SccWaveRegistry から波形を引く
//    updateVoice(ch) が WS を見て setWaveform(ch, data) を呼ぶ
//    SccWaveRegistry のポインタを setWaveRegistry() で注入する
// ================================================================
class CSCC : public CPSGBase {
public:
    CSCC(IPort* port, int sampleRate, uint8_t devId = DEVICE_SCC)
        : CPSGBase(devId, port, 0xB0, 5, sampleRate,
                   2, FNUM_OFFSET, FnumTableType::TonePeriod)
        , waveReg_(nullptr)
    {}

    std::string getDescriptor() const override { return "SCC (K051649) 5ch"; }

    // PatchManager から SccWaveRegistry を注入する
    void setWaveRegistry(const SccWaveRegistry* reg) { waveReg_ = reg; }
    // SCC Wave Bank 番号 (デフォルト 0)
    void setWaveBankNo(int bankNo) { waveBankNo_ = bankNo; }

    void init() override {
        // 全チャンネル無効・波形テーブルをデフォルト(矩形波)で初期化
        setReg(0xAA, 0x00, true);
        // WS=0 の矩形波を全チャンネルに書き込む
        for (uint8_t ch = 0; ch < 5; ++ch) {
            const int8_t* data = waveReg_
                ? waveReg_->getWaveData(waveBankNo_, 0)
                : SccWaveBank{}.getWaveData(0);
            writeWaveform(ch, data);
        }
    }

protected:
    const SccWaveRegistry* waveReg_     = nullptr;
    int                    waveBankNo_  = 0;
    // 直前に書き込んだ WS 番号（同じ波形の再書き込みをスキップ）
    uint8_t prevWS_[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    // チップへの波形書き込み（内部使用）
    void writeWaveform(uint8_t ch, const int8_t* data) {
        uint16_t base = static_cast<uint16_t>(ch * 0x20);
        for (int i = 0; i < SCC_WAVE_SIZE; ++i) {
            setReg(static_cast<uint16_t>(base + i),
                   static_cast<uint8_t>(data[i]), true);
        }
    }

    void updateVoice(uint8_t ch) override {
        if (ch >= 5) return;
        const auto& p  = chState_[ch].hwPatch;
        // ch に対応するオペレータの WS を波形番号として使う
        // SCC は ch ごとに独立した波形を持つので hwOp[0].WS を使う
        uint8_t ws = p.hwOp[0].WS & 0x7F;  // 7bit (0〜127)

        if (ws != prevWS_[ch]) {
            const int8_t* data = waveReg_
                ? waveReg_->getWaveData(waveBankNo_, ws)
                : SccWaveBank{}.getWaveData(ws);
            writeWaveform(ch, data);
            prevWS_[ch] = ws;
        }
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        // SCC: 0xA0+ch*2 = period LSB, 0xA1+ch*2 = period MSB (12bit)
        uint16_t period = fnum.fnum >> (fnum.block + 3);
        setReg(static_cast<uint16_t>(0xA0 + ch * 2),
               static_cast<uint8_t>(period & 0xFF), false);
        setReg(static_cast<uint16_t>(0xA1 + ch * 2),
               static_cast<uint8_t>((period >> 8) & 0xF), false);
    }

    void updateVolExp(uint8_t ch) override {
        uint8_t evol = chState_[ch].proc.effectiveTL(0);
        uint8_t vol  = (evol * 15u) / 127u;  // SCC: 0=最小, 15=最大
        setReg(static_cast<uint16_t>(0xA8 + ch), vol & 0xF, false);
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        uint8_t cur = getReg(0xAA);
        if (keyOn) cur |=  (1u << ch);
        else       cur &= ~(1u << ch);
        setReg(0xAA, cur, true);
    }
};

} // namespace fitom

namespace fitom {
std::unique_ptr<ISoundDevice> createCSSG(IPort* p, int sr)  { return std::make_unique<CSSG>(p, sr); }

// SAA1099 は旧実装 (SAA.cpp) のみ。新実装 (SAA_new.cpp) 未作成。
// SAA1099 の HW EG 使用時の仕様:
//   - 音量制御 (effectiveTL→VTL) のみ有効
//   - EG レートのベロシティ感度 (VAR〜VRR) は無効 (SAA はソフトEGなし)
// → 新実装時には SAA::updateVolExp で EGT フラグを確認し、
//   EGT 有効時は effectiveTL のみ参照してレジスタへ書くこと。
std::unique_ptr<ISoundDevice> createCDCSG(IPort* p, int sr) { return std::make_unique<CDCSG>(p, sr); }
std::unique_ptr<ISoundDevice> createCSCC(IPort* p, int sr)  { return std::make_unique<CSCC>(p, sr); }
} // namespace fitom
