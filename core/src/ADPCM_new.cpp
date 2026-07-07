#include "fitom/PcmBankData.h"
// fitom/ADPCM_new.cpp
// ADPCM / PCM チップドライバ — ISoundDevice ベース移行版
//
// CAdPcmBase: YM2608 ADPCM-A/B, YMZ280B(PCMD8) の共通基底
// CYmDelta:   YM2608 ADPCM-B (Delta-T) / OPNA 内蔵
// CAdPcmZ280: YMZ280B (PCMD8) 8ch ADPCM

#include "fitom/ISoundDevice.h"
#include "fitom/FITOMdefine.h"
#include "fitom/FnumUtils.h"
#include "fitom/VolumeUtils.h"
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
    void setPcmRegistry(const PcmBankRegistry* reg, int bankNo = 0) override {
        pcmReg_    = reg;
        pcmBankNo_ = bankNo;
    }

    // PcmBankRegistry からバイナリを取得してチップへ転送する
    // CFITOM::initDevices() でデバイス生成直後に呼ぶ
    void initPcmData() override {
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

    // ────────────────────────────────────────────────────────────
    // サンプルベース音源系 (VOICE_PATCH_ADPCMB/A/PCMD8) 共通ヘルパー。
    // ChState::samplePatchのzones[]から、現在のノート・ベロシティに
    // 一致するSampleZoneを検索する。OPL4AWMと同じ検索ロジック
    // (該当なしなら最初のゾーンにフォールバック)。
    // ────────────────────────────────────────────────────────────
    const SampleZone* resolveSampleZone(const ChState& s) const {
        if (!s.samplePatch || s.samplePatch->zones.empty()) return nullptr;
        for (const auto& z : s.samplePatch->zones) {
            if (s.lastNote >= z.keyMin && s.lastNote <= z.keyMax &&
                s.velocity  >= z.velMin && s.velocity  <= z.velMax) {
                return &z;
            }
        }
        return &s.samplePatch->zones[0]; // フォールバック: 最初のゾーン
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

    // DeltaN F-number 計算 (旧 CYmDelta::GetDeltaN 相当、完全に元の実装のまま)。
    // root_note対応は、この関数の計算結果に対して後段でスケーリングする
    // 方式にしたため(getFnumber参照)、この関数自体は一切変更していない。
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

    // root_note(MIDIノート番号)が69(A4)と異なる場合、DeltaN計算結果に
    // 直接、半音差の周波数比(2^(69-rootNote)/12)を掛けて補正する。
    // getDeltaN()自体(oct探索を含む複雑な既存ロジック)には一切手を
    // 加えず、後段でシンプルな比率補正を1回だけ適用することで、
    // rootNote=69(デフォルト、既存動作)の場合は完全に無変更のまま
    // 後方互換を保証しつつ、rootNoteが異なる場合のみ安全に対応する。
    static uint16_t applyRootNoteScale(uint16_t deltaN, uint8_t rootNote) noexcept {
        if (rootNote == 69 || deltaN == 0) return deltaN;
        double scale = std::pow(2.0, (69.0 - static_cast<double>(rootNote)) / 12.0);
        int scaled = static_cast<int>(std::round(deltaN * scale));
        return static_cast<uint16_t>(std::clamp(scaled, 0, 65535));
    }
};

// ================================================================
//  レジスタマップ (旧 REGMAP 完全移植。フィールド名・順序は
//  YMDeltaT.h の struct REGMAP に完全準拠する)
// ================================================================
struct RegMap {
    uint8_t control1;
    uint8_t control2;
    uint8_t startLSB;
    uint8_t startMSB;
    uint8_t endLSB;
    uint8_t endMSB;
    uint8_t limitLSB;   // 0xff = 未使用 (レジスタなし)
    uint8_t limitMSB;   // 0xff = 未使用
    uint8_t memory;     // PCMデータ転送レジスタ
    uint8_t deltanLSB;
    uint8_t deltanMSB;
    uint8_t volume;
    uint8_t flag;
    uint8_t ctrl1init;
    uint8_t ctrl2init;
    uint8_t panmask;
};

// YM2608 (OPNA) ADPCM-B レジスタマップ (旧FITOM CAdPcm2608 完全移植)
static const RegMap kOPNA_DeltaT = {
    /*control1*/0x00, /*control2*/0x01, /*startLSB*/0x02, /*startMSB*/0x03,
    /*endLSB*/0x04,   /*endMSB*/0x05,   /*limitLSB*/0x0c, /*limitMSB*/0x0d,
    /*memory*/0x08,   /*deltanLSB*/0x09,/*deltanMSB*/0x0a,/*volume*/0x0b,
    /*flag*/0x10,     /*ctrl1init*/0x01,/*ctrl2init*/0x00,/*panmask*/0xc0
};

// Y8950 (YM3801) ADPCM レジスタマップ (旧FITOM CAdPcm3801 完全移植)
static const RegMap kY8950_DeltaT = {
    /*control1*/0x07, /*control2*/0x08, /*startLSB*/0x09, /*startMSB*/0x0a,
    /*endLSB*/0x0b,   /*endMSB*/0x0c,   /*limitLSB*/0xff, /*limitMSB*/0xff,
    /*memory*/0x0f,   /*deltanLSB*/0x10,/*deltanMSB*/0x11,/*volume*/0x12,
    /*flag*/0x04,     /*ctrl1init*/0x01,/*ctrl2init*/0x00,/*panmask*/0x00
};

// YM2610 (OPNB) ADPCM-B レジスタマップ (旧FITOM CAdPcm2610B 完全移植)
static const RegMap kOPNB_DeltaT = {
    /*control1*/0x10, /*control2*/0x11, /*startLSB*/0x12, /*startMSB*/0x13,
    /*endLSB*/0x14,   /*endMSB*/0x15,   /*limitLSB*/0xff, /*limitMSB*/0xff,
    /*memory*/0xff,   /*deltanLSB*/0x19,/*deltanMSB*/0x1a,/*volume*/0x1b,
    /*flag*/0x1c,     /*ctrl1init*/0x01,/*ctrl2init*/0x00,/*panmask*/0xc0
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

    // 旧FITOM CYmDelta::LoadVoice 完全移植。
    // データ転送は regmap.memory レジスタを経由する。
    void loadVoice(int prog, const uint8_t* data, size_t length) override {
        if (prog < 0 || prog >= 128) return;

        uint32_t st = 0;
        if (prog > 0) {
            st = voices_[prog - 1].startAddr + voices_[prog - 1].length;
        }
        size_t blk = (length * 8) >> 5;
        if ((blk << 5) < length * 8) { ++blk; }
        blk <<= 5;
        // 256Kbit 境界をまたぐ場合は次の境界へ
        if ((st >> 18) != ((st + blk) >> 18)) {
            st = ((st >> 18) + 1) << 18;
        }
        uint32_t ed = static_cast<uint32_t>(st + blk - 1);

        voices_[prog].startAddr = st;
        voices_[prog].length    = static_cast<uint32_t>(blk);

        setReg(reg_.flag,     0);
        setReg(reg_.flag,     0x80);
        setReg(reg_.control1, reg_.ctrl1init);
        setReg(reg_.control1, 0x60);
        setReg(reg_.control2, reg_.ctrl2init);
        setReg(reg_.startLSB, static_cast<uint8_t>((st >> 5) & 0xFF));
        setReg(reg_.startMSB, static_cast<uint8_t>((st >> 13) & 0xFF));
        setReg(reg_.endLSB,   static_cast<uint8_t>((ed >> 5) & 0xFF));
        setReg(reg_.endMSB,   static_cast<uint8_t>((ed >> 13) & 0xFF));
        if (reg_.limitLSB != 0xFF) setReg(reg_.limitLSB, static_cast<uint8_t>((ed >> 5) & 0xFF));
        if (reg_.limitMSB != 0xFF) setReg(reg_.limitMSB, static_cast<uint8_t>((ed >> 13) & 0xFF));

        for (size_t i = 0; i < (blk >> 3); ++i) {
            setReg(reg_.memory, (i < length) ? data[i] : 0x80);
        }
        setReg(reg_.control1, reg_.ctrl1init);
        setReg(reg_.control1, 0);

        usedMem_ += blk;
        FITOM_LOG_DEBUG("YMDeltaT: loaded prog=" << prog
            << " start=0x" << std::hex << st << " len=" << length);
    }

protected:
    RegMap reg_;

    ChState::Fnum getFnumber(uint8_t ch, int16_t offset = 0) const override {
        ChState::Fnum ret;
        const auto& s = chState_[ch];
        if (s.lastNote >= 128) return ret;

        // 既存ロジックと全く同じ計算 (root_note=69固定相当、無変更)。
        int idx = static_cast<int>(s.lastNote) * 64
                + (noteOffset_ * 64 / 12)
                + s.fineFreq + offset;
        ret.block = 0;
        uint16_t deltaN = const_cast<CYmDelta*>(this)->getDeltaN(idx, kNoteOffset);

        // root_noteが69と異なる場合のみ、後段で周波数比を適用する。
        uint8_t rootNote = 69;
        if (const SampleZone* zone = resolveSampleZone(s)) rootNote = zone->rootNote;
        ret.fnum = applyRootNoteScale(deltaN, rootNote);
        return ret;
    }

    // Delta-N (再生ピッチ) のみを書く。Start/End/Volumeは各々別のフックが担当。
    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        setReg(reg_.deltanLSB, static_cast<uint8_t>(fnum.fnum & 0xFF));
        setReg(reg_.deltanMSB, static_cast<uint8_t>((fnum.fnum >> 8) & 0xFF));
    }

    // Start/Endアドレスのみを書く (旧FITOM CYmDelta::UpdateVoice 完全移植)。
    // プログラム番号は、samplePatchのゾーン検索結果のwaveIndexを使う
    // (B-3 resolvePcmEntryのHwPatch版から、SampleZone版に置き換え)。
    void updateVoice(uint8_t ch) override {
        const auto& s = chState_[ch];
        const SampleZone* zone = resolveSampleZone(s);
        if (!zone) {
            FITOM_LOG_WARN("CYmDelta::updateVoice: ch=" << (int)ch
                << " no SampleZone resolved (samplePatch not set?)");
            return;
        }
        int num = zone->waveIndex & 0x7F;
        if (num >= 0 && num < 128 && voices_[num].length) {
            uint32_t st = voices_[num].startAddr;
            uint32_t ed = st + voices_[num].length - 1;
            setReg(reg_.startLSB, static_cast<uint8_t>((st >> 5) & 0xFF));
            setReg(reg_.startMSB, static_cast<uint8_t>((st >> 13) & 0xFF));
            setReg(reg_.endLSB,   static_cast<uint8_t>((ed >> 5) & 0xFF));
            setReg(reg_.endMSB,   static_cast<uint8_t>((ed >> 13) & 0xFF));
            if (reg_.limitLSB != 0xFF) setReg(reg_.limitLSB, static_cast<uint8_t>((ed >> 5) & 0xFF));
            if (reg_.limitMSB != 0xFF) setReg(reg_.limitMSB, static_cast<uint8_t>((ed >> 13) & 0xFF));
        }
    }

    // Volumeのみを書く (旧FITOM CYmDelta::UpdateVolExp 完全移植)。
    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        uint8_t loudness = 127u - s.proc.effectiveTL(0);
        uint8_t vol = fitom::calcLinearLevel(loudness, 0); // 旧: CalcLinearLevel(evol, 0)
        setReg(reg_.volume, static_cast<uint8_t>(vol << 1));
    }

    void updatePanpot(uint8_t ch) override {
        int8_t pan = chState_[ch].panpot;
        uint8_t chena = (pan > 20) ? 0x40 : (pan < -20) ? 0x80 : 0xC0;
        setReg(reg_.control2, static_cast<uint8_t>(reg_.ctrl2init | (chena & reg_.panmask)));
    }

    // 再生トリガーのみ (Start/End/DeltaN/Volumeは既に別フックで設定済み)。
    // 旧FITOM CYmDelta::UpdateKey / StopPCM 完全移植。
    void updateKey(uint8_t ch, bool keyOn) override {
        if (ch >= maxChs_) return;
        stopPcm(); // 常に一旦停止
        if (keyOn) {
            setReg(reg_.flag,     0x1b);
            setReg(reg_.flag,     0x80);
            setReg(reg_.control1, 0xa0);
        }
    }

private:
    void stopPcm() {
        setReg(reg_.control1, static_cast<uint8_t>(getReg(reg_.control1) & 0x7F));
        setReg(reg_.control1, 0);
        setReg(reg_.control1, reg_.ctrl1init);
        setReg(reg_.control1, 0);
    }
};

// ================================================================
//  CAdPcm2610A: YM2610 ADPCM-A (6ch, Delta-Tとは異なる多チャンネルPCM方式)
//
//  ADPCM-B(CYmDelta)とは全く異なるレジスタ体系:
//    0x00: TEST/リセット、キーオン(bit0-5 = 1<<ch)
//    0x01: 総合音量 (全ch共通、6bit)
//    0x08+ch: パン(bit6-7)/チャンネル音量(bit0-4, 5bit)
//    0x10/0x18+ch, 0x20/0x28+ch: 各chの開始/終了アドレス
//    0x200-0x204: PCM ROM/RAM書き込み (loadVoice)
//
//  旧FITOM同様、チャンネル毎のMIDI Volume(CC#7)は反映せず、
//  velocity×expressionのみをチャンネルレジスタに反映し、
//  MIDI Volumeは総合音量レジスタ(0x01)に集約する
//  (6ch全体で1つの総合音量しか持てないハードウェア制約のため、
//   最後に更新されたチャンネルのVolumeが全体に適用される)。
// ================================================================
class CAdPcm2610A : public CAdPcmBase {
public:
    CAdPcm2610A(IPort* port, int sampleRate, size_t memSize,
                uint8_t parentDevId = DEVICE_OPNB)
        : CAdPcmBase(DEVICE_ADPCMA, port, 0x30, sampleRate, 0,
                     448 /* YMDELTA_OFFSET、CYmDelta::kNoteOffsetと同値 */,
                     memSize, 6, parentDevId)
    {
        boundary_ = 0x100000; // 1MB境界
    }

    std::string getDescriptor() const override { return "ADPCM-A (YM2610) 6ch"; }

    void init() override {
        setReg(0, 0xbf, true);
        setReg(0, 0, true);
    }

    void loadVoice(int prog, const uint8_t* data, size_t length) override {
        if (prog < 0 || prog >= 128) return;
        uint32_t st = 0;
        if (prog > 0) {
            st = voices_[prog - 1].startAddr + voices_[prog - 1].length;
        }
        // 256byte 境界に切り上げ
        size_t blk = (length + 0xFF) & ~static_cast<size_t>(0xFF);
        if ((st >> 20) < ((st + blk) >> 20)) { // 1MB境界をまたぐ場合
            st = (st + boundary_) & ~static_cast<uint32_t>(boundary_ - 1);
        }
        voices_[prog].startAddr = st;
        voices_[prog].length    = static_cast<uint32_t>(blk);

        port_->writeRaw(0x200, static_cast<uint16_t>(st & 0xFF));
        port_->writeRaw(0x201, static_cast<uint16_t>((st >> 8) & 0xFF));
        port_->writeRaw(0x202, static_cast<uint16_t>((st >> 16) & 0xFF));
        port_->writeRaw(0x203, 1);
        for (size_t i = 0; i < blk; ++i) {
            port_->writeRaw(0x204, (i < length) ? data[i] : 0x80);
        }
        usedMem_ += blk;
        FITOM_LOG_DEBUG("ADPCM-A: loaded prog=" << prog
            << " start=0x" << std::hex << st << " len=" << length);
    }

protected:
    uint32_t boundary_ = 0x100000;

    // ADPCM-A は各chが固定音程のサンプル再生のみ (音程制御レジスタなし)
    void updateFreq(uint8_t /*ch*/, const ChState::Fnum* /*fn*/) override {}

    void updateKey(uint8_t ch, bool keyOn) override {
        if (keyOn) setReg(0x00, static_cast<uint8_t>(1u << ch), true);
    }

    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        // チャンネルレジスタ: velocity×expression のみ (vol=127固定相当)
        uint8_t vev = fitom::calcVolExpVel(127, s.expression, s.velocity);
        uint8_t evol = 31u - fitom::linear2dB(vev, RANGE24DB, STEP075DB, 5);
        setReg(static_cast<uint16_t>(0x08 + ch),
               static_cast<uint8_t>((getReg(static_cast<uint16_t>(0x08 + ch)) & 0xC0) | evol), true);

        // 総合音量レジスタ: MIDI Volume(CC#7) のみ反映 (全ch共通)
        uint8_t mvol = 63u - fitom::linear2dB(s.volume, RANGE48DB, STEP075DB, 6);
        if (getReg(0x01) != mvol) setReg(0x01, mvol, true);
    }

    void updatePanpot(uint8_t ch) override {
        int8_t pan = chState_[ch].panpot;
        uint8_t chena = (pan > 20) ? 0x40 : (pan < -20) ? 0x80 : 0xC0;
        setReg(static_cast<uint16_t>(0x08 + ch),
               static_cast<uint8_t>((getReg(static_cast<uint16_t>(0x08 + ch)) & 0x3F) | chena), true);
    }

    void updateVoice(uint8_t ch) override {
        const auto& s = chState_[ch];
        const SampleZone* zone = resolveSampleZone(s);
        if (!zone) {
            FITOM_LOG_WARN("CAdPcm2610A::updateVoice: ch=" << (int)ch
                << " no SampleZone resolved (samplePatch not set?)");
            updateVolExp(ch);
            updatePanpot(ch);
            return;
        }
        int num = zone->waveIndex & 0x7F;
        if (num >= 0 && num < 128 && voices_[num].length) {
            uint32_t st = voices_[num].startAddr;
            uint32_t ed = st + voices_[num].length - 1;
            setReg(static_cast<uint16_t>(0x10 + ch), static_cast<uint8_t>((st >> 8) & 0xFF), true);
            setReg(static_cast<uint16_t>(0x18 + ch), static_cast<uint8_t>((st >> 16) & 0xFF), true);
            setReg(static_cast<uint16_t>(0x20 + ch), static_cast<uint8_t>((ed >> 8) & 0xFF), true);
            setReg(static_cast<uint16_t>(0x28 + ch), static_cast<uint8_t>((ed >> 16) & 0xFF), true);
        }
        updateVolExp(ch);
        updatePanpot(ch);
    }

    uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode) override {
        return CSoundDevice::queryCh(owner, patch, 0);
    }
};
// ================================================================
// ================================================================
//  CAdPcmZ280 (YMZ280B / PCMD8) — 8ch、4bit ADPCM固定
//
//  実機データシート (bitsavers YMZ280B_199610.pdf) 確認済み。
//  旧FITOM(PCMD8.cpp)はStart/Endアドレスの複雑な4ブロック分散配置
//  (H/M/Lがそれぞれ$20/$40/$60系に分かれる) を正確に再現していたが、
//  Pitchレジスタの書き込みだけ旧FITOM独自の内部スケール前提
//  (fnum>>8/fnum>>16という17bit幅を仮定したシフト) に依存しており、
//  新FITOMの9bit精度Fnumberとは噛み合わないため、その部分のみ
//  データシートに忠実な形で書き直す。
//
//  レジスタマップ (実機確定、chはオフセット0-7):
//    $00+ch*4: FN7-FN0 (Pitch下位8bit)
//    $01+ch*4: KON(bit7)|MD1(bit6)|MD0(bit5)|LOOP(bit3)|FN8(bit0)
//    $02+ch*4: TL7-TL0 (Total Level, 8bit, 256段階)
//    $03+ch*4: PAN3-PAN0 (Panpot, 4bit, 16段階)
//    $20/$40/$60+ch*4: Start Address (H/M/L)
//    $21/$41/$61+ch*4: Loop Start Address (H/M/L) ※未使用のためゼロ固定
//    $22/$42/$62+ch*4: Loop End Address (H/M/L)   ※未使用のためゼロ固定
//    $23/$43/$63+ch*4: End Address (H/M/L)
//    $80: DSP出力chセレクタ (今回はDSP非接続のため無効化のみ)
//    $FF: KENS(bit0)|MENS(bit1)|IENS(bit2) ※ビット位置はデータシート
//         記載レイアウトが崩れており確定できず、旧FITOMの実測値(0xc0)
//         を踏襲する
//
//  Pitch精度は9bit(FN8-FN0)。4bit ADPCMモード固定 (MD1MD0=01) のため
//  0.172〜44.1kHzを256刻みで表現する。
// ================================================================
class CAdPcmZ280 : public CAdPcmBase {
public:
    static constexpr int kNoteOffset = 356; // YMZ280_OFFSET (旧FITOM準拠)

    CAdPcmZ280(IPort* port, int sampleRate, size_t memSize)
        : CAdPcmBase(DEVICE_PCMD8, port, 0x100, sampleRate, 384,
                     kNoteOffset, memSize, 8, DEVICE_PCMD8)
    {}

    std::string getDescriptor() const override { return "YMZ280B (PCMD8) 8ch"; }

    void init() override {
        // KENS/MENS 有効化。ビット位置はデータシートのレイアウト崩れで
        // 確定できないため、旧FITOMの実測値をそのまま踏襲する。
        setReg(0xFF, 0xC0, true);
        // DSP出力は使わないため無効化 (Lch/Rch enable bit = 1:Disable)
        setReg(0x80, 0x88, true);
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

        for (size_t i = 0; i < blk; ++i) {
            uint32_t addr = st + i;
            setReg(0x84, static_cast<uint8_t>(addr >> 16));
            setReg(0x85, static_cast<uint8_t>((addr >> 8) & 0xFF));
            setReg(0x86, static_cast<uint8_t>(addr & 0xFF));
            setReg(0x87, (i < length) ? data[i] : uint8_t{0});
        }
        usedMem_ += blk;
    }

protected:
    // 9bit精度 (FN8-FN0)。実機式は正確な変換係数がデータシートに
    // 明記されていないため、既存のgetDeltaN基盤 (DeltaN方式テーブル、
    // 384分周) をそのまま用いる。
    ChState::Fnum getFnumber(uint8_t ch, int16_t offset = 0) const override {
        ChState::Fnum ret;
        const auto& s = chState_[ch];
        if (s.lastNote >= 128) return ret;

        int idx = static_cast<int>(s.lastNote) * 64
                + (noteOffset_ * 64 / 12)
                + s.fineFreq + offset;
        ret.block = 0;
        uint16_t deltaN = const_cast<CAdPcmZ280*>(this)->getDeltaN(idx, kNoteOffset);

        uint8_t rootNote = 69;
        if (const SampleZone* zone = resolveSampleZone(s)) rootNote = zone->rootNote;
        ret.fnum = applyRootNoteScale(deltaN, rootNote) & 0x1FF; // 9bit
        return ret;
    }

    // Start/Loop/End アドレス (H/M/L の3ブロックに分散配置)。
    // ループ再生は使わないため Loop Start/End はゼロ固定 (旧FITOM準拠)。
    void updateVoice(uint8_t ch) override {
        const auto& s = chState_[ch];
        const SampleZone* zone = resolveSampleZone(s);
        if (!zone) {
            FITOM_LOG_WARN("CAdPcmZ280::updateVoice: ch=" << (int)ch
                << " no SampleZone resolved (samplePatch not set?)");
            return;
        }
        int prog = zone->waveIndex & 0x7F;
        if (prog < 0 || prog >= 128 || !voices_[prog].length) return;

        uint32_t st = voices_[prog].startAddr;
        uint32_t ed = st + voices_[prog].length - 1;
        uint8_t base = static_cast<uint8_t>(ch * 4);

        setReg(static_cast<uint16_t>(0x20 + base), static_cast<uint8_t>((st >> 16) & 0xFF), true);
        setReg(static_cast<uint16_t>(0x40 + base), static_cast<uint8_t>((st >> 8) & 0xFF), true);
        setReg(static_cast<uint16_t>(0x60 + base), static_cast<uint8_t>(st & 0xFF), true);
        setReg(static_cast<uint16_t>(0x23 + base), static_cast<uint8_t>((ed >> 16) & 0xFF), true);
        setReg(static_cast<uint16_t>(0x43 + base), static_cast<uint8_t>((ed >> 8) & 0xFF), true);
        setReg(static_cast<uint16_t>(0x63 + base), static_cast<uint8_t>(ed & 0xFF), true);

        // Loop Start/End (未使用、ゼロ固定)
        setReg(static_cast<uint16_t>(0x21 + base), 0, true);
        setReg(static_cast<uint16_t>(0x41 + base), 0, true);
        setReg(static_cast<uint16_t>(0x61 + base), 0, true);
        setReg(static_cast<uint16_t>(0x22 + base), 0, true);
        setReg(static_cast<uint16_t>(0x42 + base), 0, true);
        setReg(static_cast<uint16_t>(0x62 + base), 0, true);

        updateVolExp(ch);
        updatePanpot(ch);
    }

    // Pitch (9bit): FN7-FN0を$00+ch*4、FN8を$01+ch*4のbit0に書く。
    // (旧FITOMの`>>8`/`>>16`シフトは、旧FITOM独自の内部Fnumber表現を
    //  前提にした変換であり、新FITOMの9bit精度Fnumberとは異なるため使わない)
    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint8_t base = static_cast<uint8_t>(ch * 4);
        setReg(static_cast<uint16_t>(0x00 + base),
               static_cast<uint8_t>(fnum.fnum & 0xFF), false);
        uint8_t cur = getReg(static_cast<uint16_t>(0x01 + base)) & 0xFE; // FN8以外を保持
        setReg(static_cast<uint16_t>(0x01 + base),
               static_cast<uint8_t>(cur | ((fnum.fnum >> 8) & 1)), false);
    }

    // Total Level (8bit)。7bit音量を8bit空間へビット拡張 (旧FITOM準拠、
    // MSB複製による拡張: (v<<1)|(v>>6))。
    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        uint8_t vol = s.proc.effectiveTL(0);
        uint8_t loudness = 127u - vol;
        setReg(static_cast<uint16_t>(0x02 + ch * 4),
               static_cast<uint8_t>((loudness << 1) | (loudness >> 6)), false);
    }

    // Panpot (4bit、16段階)。中央=8相当 (データシートに中央値の明記は
    // ないため、16段階の中央=8と仮定)。
    void updatePanpot(uint8_t ch) override {
        int8_t pan = chState_[ch].panpot; // -64..63
        int pan4 = std::clamp((static_cast<int>(pan) + 64) * 15 / 127, 0, 15);
        setReg(static_cast<uint16_t>(0x03 + ch * 4),
               static_cast<uint8_t>(pan4 & 0xF), false);
    }

    void updateSustain(uint8_t /*ch*/) override {}
    void updateTL(uint8_t, uint8_t, uint8_t) override {}

    // KON(bit7)。MD1MD0=01(4bit ADPCM)固定。FN8ビットは保持する。
    void updateKey(uint8_t ch, bool keyOn) override {
        uint8_t base = static_cast<uint8_t>(0x01 + ch * 4);
        uint8_t fn8 = getReg(base) & 0x01;
        setReg(base, static_cast<uint8_t>((keyOn ? 0xA0 : 0x20) | fn8), true);
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
    case DEVICE_MA1:
        // Y8950 (YM3801) 内蔵ADPCM
        return std::make_unique<CYmDelta>(
            deviceType, p, 0x20, sr, 72, 256 * 1024, DEVICE_Y8950,
            kY8950_DeltaT);
    case DEVICE_ADPCMB_OPNA:
        // OPNA(YM2608)内蔵ADPCM-B。fnumDivide=144はOPNA用マスタークロック分周比。
        return std::make_unique<CYmDelta>(
            deviceType, p, 0x20, sr, 144, 256 * 1024, DEVICE_OPNA,
            kOPNA_DeltaT);
    case DEVICE_ADPCMB:
        // OPNB(YM2610/YM2610B)内蔵ADPCM-B。OPNAとはレジスタマップが異なる。
        return std::make_unique<CYmDelta>(
            deviceType, p, 0x20, sr, 144, 256 * 1024, DEVICE_OPNB,
            kOPNB_DeltaT);
    case DEVICE_ADPCMA:
        // ADPCM-A (YM2610) は ADPCM-B (Delta-T) とは全く異なるレジスタ体系。
        return std::make_unique<CAdPcm2610A>(p, sr, 1024 * 1024, DEVICE_OPNB); // 1MB
    default:
        return nullptr;
    }
}

} // namespace fitom
