#include "fitom/PcmBankData.h"
// fitom/ADPCM_new.cpp
// ADPCM / PCM チップドライバ — ISoundDevice ベース移行版
//
// CAdPcmBase: YM2608 ADPCM-A/B, YMZ280B(PCMD8) の共通基底
// CYmDelta:   YM2608 ADPCM-B (Delta-T) / OPNA 内蔵
// CAdPcmZ280: YMZ280B (PCMD8) 8ch ADPCM

#include "fitom/ISoundDevice.h"
#include "fitom/FnumUtils.h"
#include "fitom/Log.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace fitom {

// ================================================================
//  ADPCM ボイス情報 (旧 ADPCMVOICE 相当)
// ================================================================
struct AdpcmVoice {
    char     name[32]   = {};
    uint32_t startAddr  = 0;   // ビット単位
    uint32_t length     = 0;   // ビット単位
    uint32_t loopStart  = 0;   // offset from start
    uint32_t loopEnd    = 0;   // offset from start
};

// ================================================================
//  CAdPcmBase: ADPCM デバイス基底クラス
// ================================================================
class CAdPcmBase : public CSoundDevice {
public:
    CAdPcmBase(uint32_t devId, IPort* port, size_t regSize,
               int sampleRate, int fnumDivide, int noteOffset,
               size_t memSize, uint8_t maxChs, uint8_t parentDevId)
        : CSoundDevice(static_cast<uint8_t>(devId), maxChs, port,
                       sampleRate, fnumDivide,
                       noteOffset, FnumTableType::DeltaN,
                       regSize)
        , maxMem_(memSize)
        , usedMem_(0)
        , parentDevId_(parentDevId)
        , pcmReg_(nullptr)
        , pcmBankNo_(0)
    {
        fnumTable_ = FnumRegistry::instance().getTable(
            FnumTableType::DeltaN, sampleRate, fnumDivide, noteOffset);
    }

    // B-3: PcmBankRegistry を注入する (CFITOM::initDevices() から呼ぶ)
    void setPcmRegistry(const PcmBankRegistry* reg, int bankNo = 0) {
        pcmReg_    = reg;
        pcmBankNo_ = bankNo;
    }

    // PcmBankRegistry からバイナリを取得してチップへ転送する
    // CFITOM::initDevices() でデバイス生成直後に呼ぶ
    void initPcmData() {
        if (!pcmReg_) return;
        const PcmBank* bank = pcmReg_->find(pcmBankNo_);
        if (!bank || !bank->hasBinData()) {
            FITOM_LOG_WARN("CAdPcmBase: PCM bank " << pcmBankNo_ << " not loaded");
            return;
        }
        // バイナリ全体をエントリごとに転送
        for (int i = 0; i < PCM_MAX_ENTRIES; ++i) {
            const auto& e = bank->getEntry(static_cast<uint8_t>(i));
            if (!e.isValid()) continue;
            if (e.startOffset + e.paddedSize > bank->binData.size()) continue;
            loadVoice(i,
                bank->binData.data() + e.startOffset,
                e.paddedSize);
        }
        FITOM_LOG_INFO("CAdPcmBase: PCM data loaded, "
            << usedMem_ << " bytes used");
    }

    // B-3: HwPatch.hwOp[0].WS = エントリ番号 → PcmEntry を解決
    const PcmEntry* resolvePcmEntry(const HwPatch& patch) const {
        if (!pcmReg_) return nullptr;
        uint8_t ws = patch.hwOp[0].WS & 0x7F;
        return pcmReg_->resolve(pcmBankNo_, ws);
    }

    // 後方互換: 直接データを転送する方式 (テスト用)
    virtual void loadVoice(int prog, const uint8_t* data, size_t length) = 0;

    uint8_t  getParentDevId() const { return parentDevId_; }
    size_t   getMaxMem()      const { return maxMem_; }
    size_t   getUsedMem()     const { return usedMem_; }

    void updateVoice(uint8_t /*ch*/) override    {}
    void updateVolExp(uint8_t /*ch*/) override   {}
    void updatePanpot(uint8_t /*ch*/) override   {}
    void updateSustain(uint8_t /*ch*/) override  {}
    void updateTL(uint8_t, uint8_t, uint8_t) override {}

protected:
    size_t   maxMem_;
    size_t   usedMem_;
    uint8_t  parentDevId_;

    // B-3 追加
    const PcmBankRegistry* pcmReg_;
    int                    pcmBankNo_;

    AdpcmVoice voices_[128];  // 後方互換 (loadVoice で使用)

    // DeltaN F-number 計算 (旧 CYmDelta::GetDeltaN 相当)
    uint16_t getDeltaN(int offsetFromBase, int baseOffset) {
        int off = offsetFromBase;
        int oct = 0;
        while (off > 1216) off -= 768;
        while (!((baseOffset - oct * 768) <= off &&
                  off <= ((baseOffset + 768) - oct * 768))) {
            ++oct;
        }
        off += 768 * oct;
        if (!fnumTable_ || (off - baseOffset) >= FNUM_TABLE_SIZE) return 0;
        return static_cast<uint16_t>(fnumTable_[off - baseOffset] >> oct);
    }
};

// ================================================================
//  レジスタマップ (旧 REGMAP 相当)
// ================================================================
struct RegMap {
    uint16_t flag;
    uint16_t control1;
    uint16_t control2;
    uint16_t startLSB;
    uint16_t startMSB;
    uint16_t endLSB;
    uint16_t endMSB;
    uint16_t limitLSB;
    uint16_t limitMSB;
    uint16_t volume;
    uint16_t delta;
    uint8_t  ctrl1init;
    uint8_t  ctrl2init;
};

// YM2608 (OPNA) ADPCM-B レジスタマップ
static const RegMap kOPNA_DeltaT = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
    0x1d, 0x1e, 0x1b, 0x19, 0x00, 0x00
};

// ================================================================
//  CYmDelta: YM2608 ADPCM-B (Delta-T)
// ================================================================
class CYmDelta : public CAdPcmBase {
public:
    static constexpr int kNoteOffset = 448; // YMDELTA_OFFSET

    CYmDelta(uint32_t devId, IPort* port, size_t regSize,
             int sampleRate, int fnumDivide, size_t memSize,
             uint8_t parentDevId, const RegMap& regmap)
        : CAdPcmBase(devId, port, regSize, sampleRate, fnumDivide,
                     kNoteOffset, memSize, 1, parentDevId)
        , reg_(regmap)
    {}

    std::string getDescriptor() const override { return "YMDeltaT (ADPCM-B) 1ch"; }
    void init() override {}

    void loadVoice(int prog, const uint8_t* data, size_t length) override {
        if (prog < 0 || prog >= 128) return;

        uint32_t st = 0;
        if (prog > 0) {
            st = voices_[prog - 1].startAddr + voices_[prog - 1].length;
        }
        // 32byte アライン
        size_t blk = (length * 8 + 31) / 32 * 32;
        // 256Kbit 境界をまたぐ場合は次の境界へ
        if ((st >> 18) != ((st + blk) >> 18)) {
            st = ((st >> 18) + 1) << 18;
        }
        uint32_t ed = static_cast<uint32_t>(st + blk - 1);

        voices_[prog].startAddr = st;
        voices_[prog].length    = static_cast<uint32_t>(blk);

        // ADPCM-B に書き込み
        setReg(reg_.flag,     0x00);
        setReg(reg_.flag,     0x80);
        setReg(reg_.control1, reg_.ctrl1init);
        setReg(reg_.control1, 0x60); // メモリ書き込みモード
        setReg(reg_.control2, reg_.ctrl2init);
        setReg(reg_.startLSB, static_cast<uint8_t>((st >> 5) & 0xFF));
        setReg(reg_.startMSB, static_cast<uint8_t>((st >> 13) & 0xFF));
        setReg(reg_.endLSB,   static_cast<uint8_t>((ed >> 5) & 0xFF));
        setReg(reg_.endMSB,   static_cast<uint8_t>((ed >> 13) & 0xFF));

        // データ転送
        for (size_t i = 0; i < length; ++i) {
            port_->write(0, data[i]); // 旧 SetReg(regmap.data, ...)
        }
        setReg(reg_.flag, 0x00);
        usedMem_ += blk;
        FITOM_LOG_DEBUG("YMDeltaT: loaded prog=" << prog
            << " start=0x" << std::hex << st << " len=" << length);
    }

protected:
    RegMap reg_;

    ChState::Fnum getFnumber(uint8_t ch, int16_t offset) const override {
        ChState::Fnum ret;
        const auto& s = chState_[ch];
        if (s.lastNote >= 128) return ret;
        int idx = static_cast<int>(s.lastNote) * 64
                + (noteOffset_ * 64 / 12)
                + s.fineFreq + offset;
        ret.block = 0;
        ret.fnum  = const_cast<CYmDelta*>(this)->getDeltaN(idx, kNoteOffset);
        return ret;
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        setReg(reg_.delta, static_cast<uint8_t>(fnum.fnum & 0xFF));
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        const auto& s = chState_[ch];
        int prog = static_cast<int>(s.lastNote); // prog 番号として使う
        if (prog < 0 || prog >= 128) return;

        if (keyOn) {
            uint32_t st = voices_[prog].startAddr;
            uint32_t ed = st + voices_[prog].length - 1;
            setReg(reg_.flag,     0x00);
            setReg(reg_.control1, reg_.ctrl1init);
            setReg(reg_.control2, reg_.ctrl2init);
            setReg(reg_.startLSB, static_cast<uint8_t>((st >> 5) & 0xFF));
            setReg(reg_.startMSB, static_cast<uint8_t>((st >> 13) & 0xFF));
            setReg(reg_.endLSB,   static_cast<uint8_t>((ed >> 5) & 0xFF));
            setReg(reg_.endMSB,   static_cast<uint8_t>((ed >> 13) & 0xFF));
            // ループ設定
            setReg(reg_.limitLSB, static_cast<uint8_t>((ed >> 5) & 0xFF));
            setReg(reg_.limitMSB, static_cast<uint8_t>((ed >> 13) & 0xFF));
            updateFreq(ch, nullptr);
            // ボリューム
            uint8_t vol = s.proc.effectiveTL(0);
            setReg(reg_.volume, static_cast<uint8_t>(255 - (vol * 255 / 127)));
            // 再生開始
            setReg(reg_.flag, 0x80);
            setReg(reg_.control1, static_cast<uint8_t>(reg_.ctrl1init | 0x01)); // START
        } else {
            setReg(reg_.flag, 0x00); // STOP
        }
    }
};

// ================================================================
//  CAdPcmZ280: YMZ280B (PCMD8) — 8ch ADPCM
// ================================================================
class CAdPcmZ280 : public CAdPcmBase {
public:
    static constexpr int kNoteOffset = 356; // YMZ280_OFFSET

    CAdPcmZ280(IPort* port, int sampleRate, size_t memSize)
        : CAdPcmBase(DEVICE_PCMD8, port, 0x100, sampleRate, 384,
                     kNoteOffset, memSize, 8, DEVICE_PCMD8)
    {}

    std::string getDescriptor() const override { return "YMZ280B (PCMD8) 8ch"; }

    void init() override {
        setReg(0xFF, 0xC0, true); // KON enable / Memory enable
        setReg(0x81, 0x00, true); // DSP disable
        for (int i = 0; i < 8; ++i) {
            setReg(0x80, static_cast<uint8_t>(0x88 | (i * 17)), true);
        }
    }

    void loadVoice(int prog, const uint8_t* data, size_t length) override {
        if (prog < 0 || prog >= 128) return;

        uint32_t st = 0;
        if (prog > 0) {
            st = voices_[prog - 1].startAddr + voices_[prog - 1].length;
        }
        size_t blk = (length + 3) / 4 * 4;
        uint32_t ed = static_cast<uint32_t>(st + blk - 1);

        voices_[prog].startAddr = st;
        voices_[prog].length    = static_cast<uint32_t>(blk);

        // アドレス / データ書き込み
        for (size_t i = 0; i < length; ++i) {
            uint32_t addr = st + i;
            setReg(0x84, static_cast<uint8_t>(addr >> 16));
            setReg(0x85, static_cast<uint8_t>(addr >> 8));
            setReg(0x86, static_cast<uint8_t>(addr));
            setReg(0x87, data[i]);
        }
        usedMem_ += blk;
    }

protected:
    ChState::Fnum getFnumber(uint8_t ch, int16_t offset) const override {
        ChState::Fnum ret;
        const auto& s = chState_[ch];
        if (s.lastNote >= 128) return ret;
        int idx = static_cast<int>(s.lastNote) * 64
                + (noteOffset_ * 64 / 12)
                + s.fineFreq + offset;
        ret.block = 0;
        ret.fnum  = const_cast<CAdPcmZ280*>(this)->getDeltaN(idx, kNoteOffset);
        return ret;
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        // YMZ280B: 各チャンネルのピッチレジスタ (0x00 + ch*8 = Pitch)
        uint8_t base = static_cast<uint8_t>(ch * 8);
        setReg(static_cast<uint16_t>(base + 2), static_cast<uint8_t>(fnum.fnum >> 8));
        setReg(static_cast<uint16_t>(base + 3), static_cast<uint8_t>(fnum.fnum & 0xFF));
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        const auto& s = chState_[ch];
        int prog = static_cast<int>(s.lastNote);
        if (prog < 0 || prog >= 128) return;

        uint8_t base = static_cast<uint8_t>(ch * 8);
        if (keyOn) {
            uint32_t st = voices_[prog].startAddr;
            uint32_t ed = st + voices_[prog].length - 1;

            // Start / End アドレス
            setReg(static_cast<uint16_t>(base + 4), static_cast<uint8_t>(st >> 16));
            setReg(static_cast<uint16_t>(base + 5), static_cast<uint8_t>(st >> 8));
            setReg(static_cast<uint16_t>(base + 6), static_cast<uint8_t>(st));
            setReg(static_cast<uint16_t>(base + 7), static_cast<uint8_t>(ed >> 16));

            // ボリューム
            uint8_t vol = s.proc.effectiveTL(0);
            uint8_t lvol = static_cast<uint8_t>(vol * 255 / 127);
            setReg(static_cast<uint16_t>(base + 0), lvol); // L
            setReg(static_cast<uint16_t>(base + 1), lvol); // R

            updateFreq(ch, nullptr);

            // KEY ON (0x80 | ch)
            setReg(static_cast<uint16_t>(0x80 + ch),
                   static_cast<uint8_t>(0xC0 | ch), true); // KON
        } else {
            setReg(static_cast<uint16_t>(0x80 + ch),
                   static_cast<uint8_t>(0x80 | ch), true); // KOFF
        }
    }
};

// ================================================================
//  ファクトリ関数
// ================================================================
std::unique_ptr<ISoundDevice> createCAdPcm(IPort* p, int sr, uint32_t deviceType)
{
    switch (deviceType) {
    case DEVICE_PCMD8:
    case DEVICE_MA2:
        return std::make_unique<CAdPcmZ280>(p, sr, 4 * 1024 * 1024); // 4MB
    case DEVICE_ADPCM:
    case DEVICE_ADPCMA:
    case DEVICE_ADPCMB:
    case DEVICE_MA1:
        return std::make_unique<CYmDelta>(
            deviceType, p, 0x100, sr, 1, 256 * 1024, DEVICE_OPNA,
            kOPNA_DeltaT);
    default:
        return nullptr;
    }
}

} // namespace fitom
