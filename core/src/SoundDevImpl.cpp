// fitom/SoundDevImpl.cpp
// CSoundDevice 共通実装
//
// 旧 SoundDev.cpp からの移行ポイント:
//   CHATTR       → ChState  (VoiceProcessor 内包)
//   FMVOICE*     → HwPatch* / VoiceProcessor
//   UpdateOpLFO  → VoiceProcessor::onTick() の tlUpdateMask で代替
//   UpdateVolExp → VoiceProcessor::effectiveTL() で代替
//   CLFOControl  → LfoControl (VoiceProcessor.h)

#include "fitom/ISoundDevice.h"
#include "fitom/Log.h"
#include "fitom/Fnum.h"
#include <cstring>
#include <algorithm>

extern "C" {
    // Fnum.cpp が提供するテーブル
    extern const uint16_t FnumTable[];
    extern const uint16_t FnumTableSSG[];
}

namespace fitom {

// ================================================================
//  静的定数
// ================================================================
const uint8_t CSoundDevice::kCarrierMask[8] = {
    0x08, 0x08, 0x08, 0x08, 0x0A, 0x0E, 0x0E, 0x0F
};

// ================================================================
//  コンストラクタ / デストラクタ
// ================================================================
CSoundDevice::CSoundDevice(uint8_t deviceType, uint8_t maxChs, IPort* port,
                            int fnumMaster, int fnumDivide,
                            int noteOffset, FnumTableType fnumType, int regSize)
    : deviceType_(deviceType)
    , maxChs_(maxChs)
    , opCount_(4)
    , port_(port)
    , regBak_(nullptr)
    , regSize_(static_cast<size_t>(regSize))
    , fnumTable_(fnumType == FnumTableType::SSG ? FnumTableSSG : FnumTable)
    , fnumMaster_(fnumMaster)
    , noteOffset_(noteOffset)
    , masterVolume_(127)
    , priorCh_(0)
{
    if (regSize > 0) {
        regBak_ = new uint8_t[regSize];
        std::memset(regBak_, 0, regSize);
    }
    for (int i = 0; i < MAX_CHS; ++i) {
        chState_[i].init();
        if (i >= maxChs_) chState_[i].disable();
    }
}

CSoundDevice::~CSoundDevice()
{
    delete[] regBak_;
}

// ================================================================
//  レジスタアクセス
// ================================================================
void CSoundDevice::setReg(uint16_t reg, uint8_t data, bool forceWrite)
{
    if (!port_) return;
    if (!forceWrite && regBak_ && reg < regSize_ && regBak_[reg] == data) return;
    if (regBak_ && reg < regSize_) regBak_[reg] = data;
    port_->write(reg, data);
}

uint8_t CSoundDevice::getReg(uint16_t reg) const
{
    if (regBak_ && reg < regSize_) return regBak_[reg];
    if (port_) return port_->read(reg);
    return 0;
}

// ================================================================
//  デスクリプタ
// ================================================================
std::string CSoundDevice::getDescriptor() const
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02X (%dch)", deviceType_, maxChs_);
    return buf;
}

// ================================================================
//  チャンネル割り当て
// ================================================================

uint8_t CSoundDevice::queryCh(IMidiCh* owner, const HwPatch* patch, int mode)
{
    // mode=1: 空きチャンネルのみ / mode=0: 使用中でも可
    uint8_t ret = 0xFF;
    uint8_t tmp = priorCh_;

    auto isCandidate = [&](const ChState& s) -> bool {
        if (!s.isEnabled() || !s.autoAssign) return false;
        return mode ? s.isEmpty() : s.isEnabled();
    };

    if (owner && patch) {
        // 同じ owner + 同じパッチ ID → 再利用優先
        for (int i = 0; i < maxChs_; ++i) {
            const auto& s = chState_[tmp];
            if (isCandidate(s) && s.owner == owner
                && s.hwPatch.id == patch->id) {
                ret = tmp; break;
            }
            tmp = (tmp + 1) % maxChs_;
        }
    } else if (patch) {
        // 同じパッチ ID
        tmp = priorCh_;
        for (int i = 0; i < maxChs_; ++i) {
            const auto& s = chState_[tmp];
            if (isCandidate(s) && s.hwPatch.id == patch->id) {
                ret = tmp; break;
            }
            tmp = (tmp + 1) % maxChs_;
        }
    } else {
        // 任意の空きチャンネル
        tmp = priorCh_;
        for (int i = 0; i < maxChs_; ++i) {
            const auto& s = chState_[tmp];
            if (isCandidate(s)) { ret = tmp; break; }
            tmp = (tmp + 1) % maxChs_;
        }
    }

    if (ret != 0xFF) priorCh_ = (priorCh_ + 1) % maxChs_;
    return ret;
}

uint8_t CSoundDevice::allocCh(IMidiCh* owner, const HwPatch* patch)
{
    // 優先順位: owner+patch 一致 → patch 一致 → 任意空き → 強制奪取
    uint8_t ret = queryCh(owner, patch, 1);
    if (ret == 0xFF) ret = queryCh(nullptr, patch, 1);
    if (ret == 0xFF) ret = queryCh(nullptr, nullptr, 1);

    if (ret == 0xFF) {
        // 最も古い発音中チャンネルを奪う
        uint16_t maxAge = 0;
        int cand = 0xFF;
        for (int i = 0; i < maxChs_; ++i) {
            auto& s = chState_[i];
            if (s.autoAssign && s.isEnabled() && s.noteOnAge > maxAge) {
                maxAge = s.noteOnAge;
                cand = i;
            }
        }
        ret = static_cast<uint8_t>(cand);
        if (ret == 0xFF) ret = queryCh(nullptr, nullptr, 0);
        FITOM_LOG_DEBUG("allocCh: forced steal ch=" << static_cast<int>(ret)
            << " age=" << maxAge);
    }

    if (ret != 0xFF) {
        assignCh(ret, owner, patch);
    }
    return ret;
}

uint8_t CSoundDevice::assignCh(uint8_t ch, IMidiCh* owner, const HwPatch* patch)
{
    if (ch >= maxChs_) return 0xFF;
    auto& s = chState_[ch];

    // 発音中なら先に止める
    if (s.isRunning() && s.owner && s.owner != owner) {
        // 親に NoteOff を通知 (前回の音を止める)
        noteOff(ch);
    }

    if (!s.isEnabled()) return 0xFF;

    s.assign(owner);
    if (patch) {
        s.hwPatch = *patch;
        updateVoice(ch);
    }
    return ch;
}

void CSoundDevice::releaseCh(uint8_t ch)
{
    if (ch < maxChs_) chState_[ch].release();
}

void CSoundDevice::enableCh(uint8_t ch, bool enable)
{
    if (ch < maxChs_) {
        if (enable) chState_[ch].enable();
        else        chState_[ch].disable();
    }
}

uint8_t CSoundDevice::getAvailableChs() const
{
    uint8_t n = 0;
    for (int i = 0; i < maxChs_; ++i)
        if (chState_[i].isEnabled()) ++n;
    return n;
}

// ================================================================
//  発音制御
// ================================================================

void CSoundDevice::noteOn(uint8_t ch, uint8_t vel)
{
    if (ch >= maxChs_) return;
    auto& s = chState_[ch];
    if (!s.isEnabled()) return;

    s.velocity = vel;
    s.run();
    s.noteOnAge = 0;

    // VoiceProcessor を起動（LFO リセット・ベロシティ計算）
    FmVoice dummy;
    dummy.hw   = s.hwPatch.hw;
    for (int i = 0; i < 4; ++i) dummy.hwOp[i] = s.hwPatch.hwOp[i];
    // SwPatch は CInstCh 側が VoiceProcessor に適用済み
    // ここでは volume/expression はデフォルト値で計算
    s.proc.onNoteOn(s.volume, s.expression, vel, dummy);

    updateKey(ch, true);
}

void CSoundDevice::noteOff(uint8_t ch)
{
    if (ch >= maxChs_) return;
    auto& s = chState_[ch];
    if (!s.isRunning()) return;

    s.release();
    s.proc.onNoteOff();
    updateKey(ch, false);
}

// ================================================================
//  パラメータ設定
// ================================================================

void CSoundDevice::setVoice(uint8_t ch, const HwPatch& patch, bool update)
{
    if (ch >= maxChs_) return;
    auto& s = chState_[ch];
    // 同じパッチなら書き込みスキップ
    if (s.hwPatch.id == patch.id) return;
    s.hwPatch = patch;
    if (update) updateVoice(ch);
}

void CSoundDevice::setNoteFine(uint8_t ch, uint8_t note, int16_t fine, bool update)
{
    if (ch >= maxChs_) return;
    auto& s = chState_[ch];
    s.lastNote = note;
    s.fineFreq  = fine;
    if (update) updateFnumber(ch, true);
}

void CSoundDevice::setVolume(uint8_t ch, uint8_t vol, bool update)
{
    if (ch >= maxChs_) return;
    chState_[ch].volume = vol;
    if (update) updateVolExp(ch);
}

void CSoundDevice::setVelocity(uint8_t ch, uint8_t vel, bool update)
{
    if (ch >= maxChs_ || vel >= 128) return;
    chState_[ch].velocity = vel;
    if (update) updateVolExp(ch);
}

void CSoundDevice::setExpression(uint8_t ch, uint8_t exp, bool update)
{
    if (ch >= maxChs_) return;
    chState_[ch].expression = exp;
    if (update) updateVolExp(ch);
}

void CSoundDevice::setPanpot(uint8_t ch, int8_t pan, bool update)
{
    if (ch >= maxChs_) return;
    chState_[ch].panpot = pan;
    if (update) updatePanpot(ch);
}

void CSoundDevice::setSustain(uint8_t ch, bool sus, bool update)
{
    if (ch >= maxChs_) return;
    chState_[ch].sustain = sus;
    if (update) updateSustain(ch);
}

void CSoundDevice::setMasterVolume(uint8_t vol)
{
    masterVolume_ = vol;
    for (int ch = 0; ch < maxChs_; ++ch) {
        if (chState_[ch].isRunning()) updateVolExp(ch);
    }
}

// ================================================================
//  リセット
// ================================================================

void CSoundDevice::reset()
{
    for (int i = 0; i < MAX_CHS; ++i) {
        chState_[i].init();
        if (i >= maxChs_) chState_[i].disable();
    }
    if (regBak_) std::memset(regBak_, 0, regSize_);
    priorCh_ = 0;
}

// ================================================================
//  タイマーコールバック (1ms ごと)
// ================================================================

void CSoundDevice::timerCallback(uint32_t tick)
{
    for (int ch = 0; ch < maxChs_; ++ch) {
        auto& s = chState_[ch];
        if (!s.isRunning()) continue;

        ++s.noteOnAge;

        // FmVoice を組み立てて VoiceProcessor に渡す
        FmVoice fv;
        fv.hw   = s.hwPatch.hw;
        for (int i = 0; i < 4; ++i) fv.hwOp[i] = s.hwPatch.hwOp[i];
        // SwPatch は CInstCh::setSwPatch() で proc に設定済みのため fv.sw は不要
        // (proc が内部で保持している sw を使う)

        auto result = s.proc.onTick(fv);

        // チャンネル LFO → F-number 更新
        if (result.needsFreqUpdate) {
            updateFnumber(static_cast<uint8_t>(ch), true);
        }

        // オペレータ LFO → TL 更新
        for (int op = 0; op < 4; ++op) {
            if (result.tlUpdateMask & (1u << op)) {
                updateTL(static_cast<uint8_t>(ch), static_cast<uint8_t>(op),
                         s.proc.effectiveTL(op));
            }
        }
    }
}

// ================================================================
//  F-number 計算
// ================================================================

ChState::Fnum CSoundDevice::getFnumber(uint8_t ch, int16_t offset) const
{
    ChState::Fnum ret;
    if (ch >= maxChs_) return ret;
    const auto& s = chState_[ch];
    if (!fnumTable_ || s.lastNote >= 128) {
        // SSG など fnum テーブルがない場合は fineFreq をそのまま返す
        ret.block = static_cast<uint8_t>(static_cast<uint16_t>(s.fineFreq) >> 12);
        ret.fnum  = static_cast<uint16_t>(s.fineFreq) & 0x0FFF;
        return ret;
    }

    // LFO ピッチ変調を加算
    int16_t lfoOffset = s.proc.channelLfoActive() ? s.proc.channelLfoValue() : 0;
    int32_t totalOffset = static_cast<int32_t>(s.fineFreq) + offset + lfoOffset;

    // noteOffset_ は -576 (= -12 note * 48 cents) が標準
    int32_t index = static_cast<int32_t>(s.lastNote) * 64
                    + (noteOffset_ * 64) / 12
                    + totalOffset;
    int oct = 0;

    // 正規化
    while (index < 0)   { --oct; index += 768; }
    while (index >= 768){ ++oct; index -= 768; }

    ret.fnum = fnumTable_[index];

    if (oct < 0) {
        ret.fnum >>= (-oct);
        oct = 0;
    }
    if (ret.fnum & 0x0800) {
        ret.fnum >>= 1;
        ++oct;
    }
    if (oct > 7) {
        ret.block = 7;
        ret.fnum  = static_cast<uint16_t>((ret.fnum << (oct - 7)) | 1);
    } else {
        ret.block = static_cast<uint8_t>(oct);
    }
    return ret;
}

void CSoundDevice::updateFnumber(uint8_t ch, bool forceWrite)
{
    if (ch >= maxChs_) return;
    ChState::Fnum fnum = getFnumber(ch);
    chState_[ch].lastFnum = fnum;
    if (forceWrite) updateFreq(ch, &fnum);
}

// ================================================================
//  モニタリング
// ================================================================

const HwPatch* CSoundDevice::getCurrentPatch(uint8_t ch) const
{
    if (ch >= maxChs_ || !chState_[ch].isRunning()) return nullptr;
    return &chState_[ch].hwPatch;
}

uint8_t CSoundDevice::getCurrentNote(uint8_t ch) const
{
    if (ch >= maxChs_ || !chState_[ch].isRunning()) return 0xFF;
    return chState_[ch].lastNote;
}

} // namespace fitom
