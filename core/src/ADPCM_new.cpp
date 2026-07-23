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
    uint32_t startAddr  = 0;   // バイト単位 (offset from start of PCM image)
    uint32_t length     = 0;   // バイト単位
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

    // PcmBankRegistry からエントリのオフセット/サイズ情報を取得し、
    // Start/Endアドレス計算用のボイステーブル(voices_)へ登録する。
    // CFITOM::initDevices() でデバイス生成直後に呼ぶ。
    //
    // 波形バイナリ自体をチップのPCMメモリへ配置する処理はここでは
    // 行わない(2026年7月、設計を訂正)。実チップのADPCM-A/B用メモリは
    // 多くの場合ROM/事前フラッシュ済みRAMであり、その配置はhwif側
    // (実機ボード・fitom_fmhwif側の設定)の責務である。FITOM_X本体は
    // 波形データが既にそこへ配置されている前提で、entries[]のオフセット/
    // サイズ情報だけを使ってStart/Endアドレスレジスタを設定する
    // (以前はFITOM_X側でPCM RAM相当のレジスタへ波形バイナリを逐次
    // 転送する実装になっていたが、この転送責務自体を持つべきではなかった)。
    void initPcmData() override {
        if (!pcmReg_) return;
        const PcmBank* bank = pcmReg_->find(pcmBankNo_);
        if (!bank) {
            FITOM_LOG_WARN("CAdPcmBase: PCM bank " << pcmBankNo_ << " not loaded");
            return;
        }
        for (int i = 0; i < PCM_MAX_ENTRIES; ++i) {
            const auto& e = bank->getEntry(static_cast<uint8_t>(i));
            if (!e.isValid()) continue;
            registerVoice(i, e.startOffset, e.paddedSize);
        }
        FITOM_LOG_INFO("CAdPcmBase: PCM voice table registered, "
            << usedMem_ << " bytes (bank " << pcmBankNo_ << ")");
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

    // entries[]のoffset(バイト単位、adpcm_packer出力のstart_offset
    // そのもの)/length(padded_size)を、このチップのアドレス表現単位
    // (バイト or ビット)に変換してvoices_[prog]へ登録するだけ。
    // チップメモリへのデータ転送は行わない(波形配置はhwif側の責務)。
    virtual void registerVoice(int prog, uint32_t offset, uint32_t length) = 0;

    uint8_t  getParentDevId() const { return parentDevId_; }
    size_t   getMaxMem()      const { return maxMem_; }
    size_t   getUsedMem()     const { return usedMem_; }

    void updateVoice(uint8_t /*ch*/) override    {}
    void updateVolExp(uint8_t /*ch*/) override   {}
    void updatePanpot(uint8_t /*ch*/) override   {}
    void updateSustain(uint8_t /*ch*/) override  {}
    void updateTL(uint8_t, uint8_t, uint8_t) override {}

protected:
    // VoiceProcessor へ渡すキャリアマスク計算用 (2026年7月追加)。
    // ADPCM系はFM合成のアルゴリズム概念を持たず、単一の"声"(op0相当)を
    // 常にキャリア扱いする(updateVolExpのeffectiveTL(0)参照)。hw.ALGは
    // ADPCM系パッチでは意味を持たないため、OPN/OPM用のデフォルト実装
    // (hw.ALGを3bit8アルゴリズムとして解釈)に頼らずオーバーライドする。
    bool isCarrierOp(uint8_t /*ch*/, int op) const override {
        return op == 0;
    }

    // 旧FITOM CAdPcmBase::CAdPcmBase() が設定していた NoteOffset(-57,
    // "origin note: O3A") 相当。noteOffset_ (= kNoteOffset 448/356) は
    // fnumTable_ 生成用のテーブルオフセットであり別物なので、これと
    // 混同して再利用してはならない。
    static constexpr int kPitchOrigin = -57;

    size_t   maxMem_;
    size_t   usedMem_;
    uint8_t  parentDevId_;

    // B-3 追加
    const PcmBankRegistry* pcmReg_;
    int                    pcmBankNo_;

    AdpcmVoice voices_[128];  // WS番号(entry_no) → Start/Endアドレス・長さ (registerVoice で登録)

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
    // Start/Endアドレスレジスタのアドレッシング境界(バイトオフセットを
    // 何bit右シフトしてレジスタ値にするか)。実チップのアドレス指定単位
    // そのもの(ymfm adpcm_b_channel::address_shift()参照)。
    // YM2608(OPNA)/Y8950は外部メモリのcontrol2依存(本FITOM_Xの実装は
    // control2のROM/RAM・8bit DRAMビットを常に0にしか設定しないため
    // 実質固定で2=4byte境界)。YM2610/YM2610B(OPNB)はチップ側の実装が
    // 常に8=256byte境界固定(control2の値に関係なく固定)であり、OPNAと
    // 混同すると再生アドレスが64倍ズレる(2026-07-24、実機ログで確認・
    // 修正)。
    uint8_t addrShift;
};

// YM2608 (OPNA) ADPCM-B レジスタマップ (旧FITOM CAdPcm2608 完全移植)
static const RegMap kOPNA_DeltaT = {
    /*control1*/0x00, /*control2*/0x01, /*startLSB*/0x02, /*startMSB*/0x03,
    /*endLSB*/0x04,   /*endMSB*/0x05,   /*limitLSB*/0x0c, /*limitMSB*/0x0d,
    /*memory*/0x08,   /*deltanLSB*/0x09,/*deltanMSB*/0x0a,/*volume*/0x0b,
    /*flag*/0x10,     /*ctrl1init*/0x01,/*ctrl2init*/0x00,/*panmask*/0xc0,
    /*addrShift*/2
};

// Y8950 (YM3801) ADPCM レジスタマップ (旧FITOM CAdPcm3801 完全移植)
static const RegMap kY8950_DeltaT = {
    /*control1*/0x07, /*control2*/0x08, /*startLSB*/0x09, /*startMSB*/0x0a,
    /*endLSB*/0x0b,   /*endMSB*/0x0c,   /*limitLSB*/0xff, /*limitMSB*/0xff,
    /*memory*/0x0f,   /*deltanLSB*/0x10,/*deltanMSB*/0x11,/*volume*/0x12,
    /*flag*/0x04,     /*ctrl1init*/0x01,/*ctrl2init*/0x00,/*panmask*/0x00,
    /*addrShift*/2
};

// YM2610 (OPNB) ADPCM-B レジスタマップ (旧FITOM CAdPcm2610B 完全移植)
// 2026-07-24: addrShiftを8(256byte境界)へ修正したが、OPNA ADPCM-Bと同一の
// パッチのはずが同じ音にならないとの報告があり、原因切り分けのため
// いったん旧値の2(4byte境界)に差し戻す(診断用の一時的な戻し。
// 上のRegMap::addrShiftコメントの8=256byte境界という判断自体を撤回した
// わけではない点に注意)。
static const RegMap kOPNB_DeltaT = {
    /*control1*/0x10, /*control2*/0x11, /*startLSB*/0x12, /*startMSB*/0x13,
    /*endLSB*/0x14,   /*endMSB*/0x15,   /*limitLSB*/0xff, /*limitMSB*/0xff,
    /*memory*/0xff,   /*deltanLSB*/0x19,/*deltanMSB*/0x1a,/*volume*/0x1b,
    /*flag*/0x1c,     /*ctrl1init*/0x01,/*ctrl2init*/0x00,/*panmask*/0xc0,
    /*addrShift*/2  // 2026-07-24: 8から一時的に差し戻し中(上記コメント参照)
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

    // offset/length(バイト単位、adpcm_packer出力のstart_offset/padded_size
    // そのもの)をそのままvoices_[prog]へ登録するだけ。チップメモリへの
    // 書き込みは行わない(波形データは既にhwif側で配置済みという前提)。
    // 実際のStart/Endアドレスレジスタへの反映はupdateVoice()がNoteOnの
    // たびに行う(reg_.addrShiftビット右シフトしてレジスタ値にする。
    // OPNA/Y8950=2=4byte境界、OPNB/OPNBB=8=256byte境界。旧実装は全チップに
    // 4byte境界のシフト量を固定で使っており、OPNB/OPNBBの実際のシフト量
    // (ymfm adpcm_b_channel側がチップ種別で固定8を使う)と食い違って
    // 再生アドレスが64倍ズレていた。2026-07-24、実機ログで確認・修正)。
    //
    // adpcm_packer側がboundary(pcmbank.json、32/256byte)に整列済みの
    // オフセットを出力していれば自動的に満たされるはずだが、万一
    // reg_.addrShift境界に整列していない場合は無警告で下位ビットが
    // 切り捨てられ、再生アドレスが実際の配置位置とずれてしまう。ここで
    // 検知して警告する(FITOM_X側で値を丸めて誤魔化すことはしない。
    // 丸めるとhwif側が実際に配置したアドレスと一致しなくなり、波形データの
    // 配置はhwif側の責務という設計方針に反するため)。
    void registerVoice(int prog, uint32_t offset, uint32_t length) override {
        if (prog < 0 || prog >= 128) return;
        uint32_t mask = (1u << reg_.addrShift) - 1;
        if ((offset & mask) != 0 || (length & mask) != 0) {
            FITOM_LOG_WARN("CYmDelta: voice[" << prog << "] offset=" << offset
                << " length=" << length << " is not aligned to a "
                << (1u << reg_.addrShift) << "-byte boundary; "
                "Start/End address register will be truncated (adpcm_packer boundary設定を確認してください)");
        }
        voices_[prog].startAddr = offset;
        voices_[prog].length    = length;
        usedMem_ += length;
    }

protected:
    RegMap reg_;

    ChState::Fnum getFnumber(uint8_t ch, int16_t offset = 0) const override {
        ChState::Fnum ret;
        const auto& s = chState_[ch];
        if (s.lastNote >= 128) return ret;

        // 既存ロジックと全く同じ計算 (root_note=69固定相当、無変更)。
        int idx = static_cast<int>(s.lastNote) * 64
                + (kPitchOrigin * 64)
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
            // st/edはバイト単位(registerVoice参照)。reg_.addrShiftビット
            // 右シフトして、このチップの実際のアドレス指定単位に変換する
            // (OPNA/Y8950=4byte単位、OPNB/OPNBB=256byte単位。旧実装は
            // 全チップに4byte単位を固定で使っていたため、OPNB/OPNBBの
            // 再生アドレスが64倍ズレていた。2026-07-24、実機ログで確認・修正)。
            uint32_t st = voices_[num].startAddr;
            uint32_t ed = st + voices_[num].length - 1;
            uint32_t stReg = st >> reg_.addrShift;
            uint32_t edReg = ed >> reg_.addrShift;
            uint8_t startLSB = static_cast<uint8_t>(stReg & 0xFF);
            uint8_t startMSB = static_cast<uint8_t>((stReg >> 8) & 0xFF);
            uint8_t endLSB   = static_cast<uint8_t>(edReg & 0xFF);
            uint8_t endMSB   = static_cast<uint8_t>((edReg >> 8) & 0xFF);
            setReg(reg_.startLSB, startLSB);
            setReg(reg_.startMSB, startMSB);
            setReg(reg_.endLSB,   endLSB);
            setReg(reg_.endMSB,   endMSB);
            if (reg_.limitLSB != 0xFF) setReg(reg_.limitLSB, endLSB);
            if (reg_.limitMSB != 0xFF) setReg(reg_.limitMSB, endMSB);
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
            // 2026-07-24修正: control2のbit7/6(pan_left/pan_right)は
            // ステレオ定位ではなく実質的な「出力有効化ビット」を兼ねる
            // (ymfm adpcm_b_channel::output()参照: 両ビットとも0だと
            // 演算結果が出力に一切加算されない=完全無音)。
            // CSoundDevice::noteOn()はpanDirty(前回値との差分)が立った
            // 場合のみupdatePanpot()を呼ぶが、panpotの初期値・既定値は
            // 0(center)であり、MIDI側が明示的にパンCCを送らない限り
            // 「0→0で変化なし」と判定されupdatePanpot()が一度も呼ばれない
            // (volDirtyはnoteOnで強制trueにされるが、panDirtyは同様の
            // 強制がされていない)。結果、control2が実チップのリセット値
            // (0=両ビットOFF)のまま放置され、ADPCM-Bがセンターパンでは
            // 恒久的に無音になっていた。他デバイスのパンレジスタと違い
            // このチップは「毎ノートオンで確実に書く」必要があるため、
            // 上位のdirtyフラグに関係なくここで無条件に呼ぶ。
            updatePanpot(ch);
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
//    0x200-0x204: PCM ROM/RAM書き込み (波形データの配置はhwif側の責務の
//                 ため、本ドライバでは使用しない。ハードウェア仕様として記載のみ)
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
    {}

    std::string getDescriptor() const override { return "ADPCM-A (YM2610) 6ch"; }

    void init() override {
        setReg(0, 0xbf, true);
        setReg(0, 0, true);
    }

    // offset(バイト単位、adpcm_packer出力のstart_offsetそのもの)を
    // voices_[prog]へそのまま登録するだけ。チップメモリへの書き込みは
    // 行わない(波形データは既にhwif側で配置済みという前提)。このチップの
    // アドレスレジスタは下位8bitを持たない(0x10+ch/0x18+chとも8bit
    // シフト後の値のみ)ため、offset/lengthは256byte境界に整列済みで
    // あることが前提(pcmbank.jsonのboundary:256、adpcm_packer側の責務)。
    // 万一整列していない場合は無警告で下位ビットが切り捨てられ、
    // 再生アドレスが実際の配置位置とずれるため、ここで検知して警告する
    // (CYmDeltaと同じ理由でFITOM_X側で値を丸めることはしない)。
    void registerVoice(int prog, uint32_t offset, uint32_t length) override {
        if (prog < 0 || prog >= 128) return;
        if ((offset & 0xFF) != 0 || (length & 0xFF) != 0) {
            FITOM_LOG_WARN("CAdPcm2610A: voice[" << prog << "] offset=" << offset
                << " length=" << length << " is not aligned to a 256-byte boundary; "
                "Start/End address register will be truncated (adpcm_packer boundary設定を確認してください)");
        }
        voices_[prog].startAddr = offset;
        voices_[prog].length    = length;
        usedMem_ += length;
    }

protected:
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

    // offset(バイト単位、adpcm_packer出力のstart_offsetそのもの)を
    // voices_[prog]へそのまま登録するだけ。チップメモリへの書き込みは
    // 行わない(波形データは既にhwif側で配置済みという前提)。このチップの
    // アドレスレジスタは24bitフル(下位バイトの切り捨てなし)。
    void registerVoice(int prog, uint32_t offset, uint32_t length) override {
        if (prog < 0 || prog >= 128) return;
        voices_[prog].startAddr = offset;
        voices_[prog].length    = length;
        usedMem_ += length;
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
                + (kPitchOrigin * 64)
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
    // 2026-07-24: DeltaNテーブル生成の真因は FnumUtils.h の
    // FnumRegistry::generateTable() 側にあった(FnumTableType::DeltaNケースで
    // divideの乗算が丸ごと欠落しており、実チップの式 delta_n=round(2^16*freq*
    // divide/master) になっていなかった)。そちらを修正したため、ここは
    // OPNA/OPNB/OPN2等のFMチップドライバ(createCOPNA/createCOPNB/createCOPN2)
    // と同様、port->getClock()(実クロック)をmasterとして使う。
    // (一度はここをport->getClock()化する修正のみでFnumUtils.h側の式を
    //  直さないまま試し、dn=3〜4という壊滅的な値になって差し戻した経緯が
    //  あるが、原因はクロック取得の是非ではなくFnumUtils.h の式自体だった)
    int clock = (p ? p->getClock() : 0);
    if (clock <= 0) clock = sr; // 取得できない場合のみ従来通りsrにフォールバック

    switch (deviceType) {
    case DEVICE_PCMD8:
    case DEVICE_MA2:
        return std::make_unique<CAdPcmZ280>(p, clock, 4 * 1024 * 1024); // 4MB
    case DEVICE_MA1:
    case DEVICE_ADPCMB_Y8950:
        // Y8950 (YM3801) 内蔵ADPCM。DEVICE_ADPCMB_Y8950が正式な識別子
        // (2026年7月新設)。旧汎用識別子DEVICE_ADPCMは削除した
        // (動いていなかったコードとの互換性維持は不要と判断)。
        return std::make_unique<CYmDelta>(
            deviceType, p, 0x20, clock, 72, 256 * 1024, DEVICE_Y8950,
            kY8950_DeltaT);
    case DEVICE_ADPCMB_OPNA:
        // OPNA(YM2608)内蔵ADPCM-B。fnumDivide=144はOPNA用マスタークロック分周比。
        return std::make_unique<CYmDelta>(
            deviceType, p, 0x20, clock, 144, 256 * 1024, DEVICE_OPNA,
            kOPNA_DeltaT);
    case DEVICE_ADPCMB:
        // OPNB(YM2610/YM2610B)内蔵ADPCM-B。OPNAとはレジスタマップが異なる。
        return std::make_unique<CYmDelta>(
            deviceType, p, 0x20, clock, 144, 256 * 1024, DEVICE_OPNB,
            kOPNB_DeltaT);
    case DEVICE_ADPCMA:
        // ADPCM-A (YM2610) は ADPCM-B (Delta-T) とは全く異なるレジスタ体系。
        return std::make_unique<CAdPcm2610A>(p, clock, 1024 * 1024, DEVICE_OPNB); // 1MB
    default:
        return nullptr;
    }
}

} // namespace fitom
