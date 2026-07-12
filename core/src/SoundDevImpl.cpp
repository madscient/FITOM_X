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
#include <cmath>

// F-number テーブルは FnumRegistry が動的生成して返す
// (旧 Fnum.cpp の静的配列 FnumTable[] は使用しない)
#include "fitom/FnumUtils.h"

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
    , fnumTable_(fitom::FnumRegistry::instance().getTable(
                    fnumType,
                    fnumMaster > 0 ? fnumMaster : 3993600,
                    fnumDivide > 0 ? fnumDivide : 144,
                    noteOffset))
    , fnumMaster_(fnumMaster)
    , fnumDivide_(fnumDivide)
    , fnumType_(fnumType)
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

// ─── findBestCh: 1パス走査でベストチャンネルを選択 ─────────────────────────
//
// 優先度スコア (高いほど優先):
//   4: owner+patch 一致 かつ Empty/Releasing
//   3: patch 一致       かつ Empty/Releasing
//   2: 任意             かつ Empty/Releasing
//   1: Running (強制奪取候補, noteOnAge 最大のもの)  allowSteal=true 時のみ
//
// priorCh_ から循環探索するため、直前に割り当てたチャンネルの次から始まる。
// 割り当て成功後に priorCh_ を進めることでラウンドロビンを実現する。
uint8_t CSoundDevice::findBestCh(IMidiCh* owner, const HwPatch* patch,
                                  bool allowSteal) const noexcept
{
    int      bestScore = 0;
    uint8_t  bestCh   = 0xFF;
    uint16_t bestAge  = 0;

    for (int i = 0; i < maxChs_; ++i) {
        const uint8_t ci = static_cast<uint8_t>((priorCh_ + i) % maxChs_);
        const auto&   s  = chState_[ci];

        if (!s.isEnabled() || !s.autoAssign) continue;

        int score = 0;
        if (s.isEmpty() || s.isReleasing()) {
            if (owner && patch
                && s.owner == owner && s.hwPatch.id == patch->id)
                score = 4;                            // owner+patch 完全一致
            else if (patch && s.hwPatch.id == patch->id)
                score = 3;                            // patch 一致
            else
                score = 2;                            // 任意の空き/Releasing
        } else if (allowSteal && s.isRunning()) {
            // 強制奪取候補: 最古 (noteOnAge 最大) のチャンネル
            if (s.noteOnAge > bestAge) {
                score    = 1;
                bestAge  = s.noteOnAge;
            }
        }

        // スコアが改善した場合のみ更新
        // 同スコアなら priorCh_ に近い方 (先に見つかった方) を優先
        if (score > bestScore) {
            bestScore = score;
            bestCh    = ci;
            if (bestScore == 4) break; // 最高優先度なら即打ち切り
        }
    }

    return bestCh;
}

// ─── queryCh: 後方互換ラッパー ───────────────────────────────────────────────
uint8_t CSoundDevice::queryCh(IMidiCh* owner, const HwPatch* patch, int mode)
{
    // mode=1: 空き/Releasing のみ (allowSteal=false)
    // mode=0: Running も対象    (allowSteal=true)
    uint8_t ret = findBestCh(owner, patch, mode == 0);
    if (ret != 0xFF)
        priorCh_ = static_cast<uint8_t>((ret + 1) % maxChs_);
    return ret;
}

uint8_t CSoundDevice::allocCh(IMidiCh* owner, const HwPatch* patch, uint8_t vel,
                               const SwPatch* swPatch, const SampleZonePatch* samplePatch)
{
    // 仮想関数 queryCh() 経由でチャンネルを選択する。
    // (修正前は findBestCh() を直接呼んでいたため、派生クラスが queryCh()
    //  をオーバーライドしてデバイス制約 (例: OPMノイズ→ch7固定、
    //  SSGノイズ→ch2固定) を課していても、allocCh() 経由の発音では
    //  その制約が仮想ディスパッチされず完全に無視される重大なバグがあった。
    //  CSpanDevice/CUnison 経由 (複数チップ束ね時) は queryCh() を正しく
    //  呼んでいたため制約が効いていたが、単体チップ構成では effectively
    //  死んでいた。)
    //
    // mode=1(奪取なし)→mode=0(奪取あり)の2段階で試す。
    // findBestCh(allowSteal=false) は score 2-4 (空き/Releasing) のみを
    // 評価し、score 1 (強制奪取候補) の判定を完全にスキップする設計のため、
    // 空き/Releasingのchが1つでもあれば1回目(mode=1)で必ず見つかり、
    // 元の「1回のfindBestCh(allowSteal=true)呼び出し」と全く同じ結果になる
    // (1パスのまま、コスト増加なし)。全ch使用中の場合のみ2回目(mode=0)が
    // 走り、最古ノートの強制奪取が行われる。
    uint8_t ret = 0xFF;
    for (int mode : {1, 0}) {
        ret = queryCh(owner, patch, mode); // 仮想呼び出し
        if (ret != 0xFF) break;
    }

    if (ret == 0xFF) {
        FITOM_LOG_WARN("allocCh: no channel available");
        return 0xFF;
    }

    if (chState_[ret].isRunning()) {
        FITOM_LOG_DEBUG("allocCh: forced steal ch=" << static_cast<int>(ret)
            << " age=" << chState_[ret].noteOnAge);
    }

    assignCh(ret, owner, patch, vel, swPatch, samplePatch);
    return ret;
}

uint8_t CSoundDevice::assignCh(uint8_t ch, IMidiCh* owner, const HwPatch* patch, uint8_t vel,
                                const SwPatch* swPatch, const SampleZonePatch* samplePatch)
{
    if (ch >= maxChs_) return 0xFF;
    auto& s = chState_[ch];
    if (!s.isEnabled()) return 0xFF;

    // 他の owner が使用中なら先に止める (noteOff が release を内包)
    if (s.isActive() && s.owner && s.owner != owner)
        noteOff(ch);

    s.assign(owner);
    s.velocity = vel;
    // patch/samplePatchは排他 (呼び出し側であるCInstCh::noteOnが
    // layer.voicePatchType == VOICE_PATCH_AWM かどうかで使い分ける)。
    if (patch) {
        s.hwPatch = *patch;
        s.samplePatch = nullptr;
        // VoiceProcessor::onNoteOn()をupdateVoice()より前に呼ぶ
        // (ドキュメント化された正しい設計、voice-data-design.mdの
        // フェーズ6手順3参照。2026年7月訂正。以前はnoteOn()側で
        // 遅延して呼ばれており、updateVoice()内のキャリア側ベロシティ
        // 補正値が常に未計算のまま実機へ送信されるバグがあった)。
        FmVoice dummy;
        dummy.hw = s.hwPatch.hw;
        for (int i = 0; i < 4; ++i) dummy.hwOp[i] = s.hwPatch.hwOp[i];
        if (swPatch) {
            dummy.sw = swPatch->sw;
            for (int i = 0; i < 4; ++i) dummy.swOp[i] = swPatch->swOp[i];
        }
        s.proc.onNoteOn(s.volume, s.expression, vel, dummy);
        updateVoice(ch);
    } else if (samplePatch) {
        s.samplePatch = samplePatch;
        updateVoice(ch);
    }
    return ch;
}

// releaseCh: 後方互換ラッパー。noteOff が release() を内包するため
// 呼び出し不要になったが、外部から呼ばれる場合のために残す。
void CSoundDevice::releaseCh(uint8_t ch)
{
    if (ch < maxChs_ && chState_[ch].isRunning())
        chState_[ch].release();
}

// ─── マスターピッチ変更 ────────────────────────────────────────────────────
// デフォルト実装: FnumRegistry のキャッシュを更新し、
// 発音中チャンネルの F-number を即時再計算して書き込む。
// OPM のようにチップ固有の計算が必要な場合はオーバーライドする。
bool CSoundDevice::isChOwnedBy(uint8_t ch, const IMidiCh* owner) const
{
    if (ch >= maxChs_) return false;
    const auto& s = chState_[ch];
    return s.isActive() && s.owner == owner;
}

void CSoundDevice::setCC1Modulation(uint8_t ch, uint8_t cc1, int16_t maxDepth)
{
    if (ch >= maxChs_) return;
    auto& s = chState_[ch];
    if (!s.isActive()) return;
    s.proc.setCC1Modulation(cc1, maxDepth);
    // デプスが変わったので F-number を再計算
    if (s.proc.channelLfoActive() || cc1 == 0)
        updateFnumber(ch, true);
}

void CSoundDevice::onMasterPitchChanged(double pitchHz)
{
    // FnumRegistry のキャッシュはセッター側でクリア済み (呼び出し元責務)
    // fnumTable_ を再取得してキャッシュを更新
    // (コンストラクタと同じフォールバック値を使う)
    const int master = (fnumMaster_ > 0) ? fnumMaster_ : 3993600;
    const int divide = (fnumDivide_ > 0) ? fnumDivide_ : 144;
    if (fnumType_ != FnumTableType::None) {
        fnumTable_ = FnumRegistry::instance().getTable(
            fnumType_, master, divide, noteOffset_);
    }
    // 発音中チャンネルの F-number を再計算
    for (int ch = 0; ch < maxChs_; ++ch) {
        if (chState_[ch].isActive())
            updateFnumber(static_cast<uint8_t>(ch), true);
    }
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
    s.volDirty = true; // 新規ノートオンは常にベロシティが変わるため強制的にdirty扱い
    s.run();
    s.noteOnAge = 0;

    // VoiceProcessor::onNoteOn()は、assignCh()内でupdateVoice()より前に
    // 既に呼ばれている(2026年7月訂正、voice-data-design.mdのフェーズ6
    // 手順3参照)。ここで再度呼ぶと、LFOリセット等が二重に走ってしまう
    // ため呼ばない。

    // CInstCh::noteOn は volume/expression/sustain/panpot を update=false で
    // 先に一括設定するため (assignCh内のupdateVoice()より後のタイミングで
    // ChStateだけ更新される)、ここで dirty なものだけをまとめて実際の
    // レジスタへ反映する。同一チャンネルでCC値が変化していない場合
    // (モノフォニックのレガート等) は dirty が立たないため、該当の
    // update*() 呼び出し自体が発生せず、冗長なレジスタ書き込みを避けられる。
    if (s.volDirty)     { updateVolExp(ch);  s.volDirty  = false; }
    if (s.panDirty)     { updatePanpot(ch);  s.panDirty  = false; }
    if (s.sustainDirty) { updateSustain(ch); s.sustainDirty = false; }

    updateKey(ch, true);
}

void CSoundDevice::noteOff(uint8_t ch)
{
    if (ch >= maxChs_) return;
    auto& s = chState_[ch];
    if (!s.isActive()) return;   // Running/Releasing のみ処理

    // KeyOff をチップに送出 (派生クラスで実装)
    // ※ 既に Releasing の場合も再度 KeyOff を送って問題ない
    // (チップは既に EG リリース中なので無害)

    s.release();   // → Status::Releasing、releaseTimer = kReleasingHoldMs
    // LFO を kReleasingHoldMs ms かけてフェードアウト
    s.proc.onNoteOff(ChState::kReleasingHoldMs);
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
    auto& s = chState_[ch];
    if (s.volume != vol) { s.volume = vol; s.volDirty = true; }
    if (update && s.volDirty) { updateVolExp(ch); s.volDirty = false; }
}

void CSoundDevice::setVelocity(uint8_t ch, uint8_t vel, bool update)
{
    if (ch >= maxChs_ || vel >= 128) return;
    auto& s = chState_[ch];
    if (s.velocity != vel) { s.velocity = vel; s.volDirty = true; }
    if (update && s.volDirty) { updateVolExp(ch); s.volDirty = false; }
}

void CSoundDevice::setExpression(uint8_t ch, uint8_t exp, bool update)
{
    if (ch >= maxChs_) return;
    auto& s = chState_[ch];
    if (s.expression != exp) { s.expression = exp; s.volDirty = true; }
    if (update && s.volDirty) { updateVolExp(ch); s.volDirty = false; }
}

void CSoundDevice::setPanpot(uint8_t ch, int8_t pan, bool update)
{
    if (ch >= maxChs_) return;
    auto& s = chState_[ch];
    if (s.panpot != pan) { s.panpot = pan; s.panDirty = true; }
    if (update && s.panDirty) { updatePanpot(ch); s.panDirty = false; }
}

void CSoundDevice::setSustain(uint8_t ch, bool sus, bool update)
{
    if (ch >= maxChs_) return;
    auto& s = chState_[ch];
    if (s.sustain != sus) { s.sustain = sus; s.sustainDirty = true; }
    if (update && s.sustainDirty) { updateSustain(ch); s.sustainDirty = false; }
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
        if (!s.isActive()) continue;   // Running または Releasing を処理

        // Releasing フェーズ: タイマーを減算して 0 になったら解放
        if (s.isReleasing()) {
            if (s.releaseTimer > 0) --s.releaseTimer;
            if (s.releaseTimer == 0) {
                s.free();
                continue;
            }
        } else {
            ++s.noteOnAge;
        }

        // FmVoice を組み立てて VoiceProcessor に渡す
        FmVoice fv;
        fv.hw   = s.hwPatch.hw;
        for (int i = 0; i < 4; ++i) fv.hwOp[i] = s.hwPatch.hwOp[i];

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

ChState::Fnum CSoundDevice::getFnumberFromHz(double hz) const
{
    ChState::Fnum ret{0, 0};
    if (hz <= 0.0 || fnumMaster_ <= 0) return ret;

    // FnumUtils.h の generateTable() と同じ式 (FnumTableType::Fnumber専用):
    //   val = freq * (2^17 / master) * divide
    double val = hz * (std::pow(2.0, 17.0) / fnumMaster_)
               * (fnumDivide_ > 0 ? fnumDivide_ : 144);

    // 11bit(0-2047)に収まるまでオクターブ正規化
    int oct = 0;
    while (val >= 2048.0 && oct < 7) { val /= 2.0; ++oct; }
    while (val <  1024.0 && oct > 0) { val *= 2.0; --oct; }

    ret.fnum  = static_cast<uint16_t>(std::clamp<long long>(std::llround(val), 0, 2047));
    ret.block = static_cast<uint8_t>(oct);
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
