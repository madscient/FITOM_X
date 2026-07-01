#pragma once
// fitom/ISoundDevice.h
// 音源デバイス抽象インターフェース (モダナイズ版)
//
// 旧 SoundDev.h の ISoundDevice / CSoundDevice から移行。
// 変更点:
//   - FMVOICE*     → const HwPatch* / FmVoice* (段階移行: 両方受け付ける)
//   - CPort*       → fitom::IPort*
//   - TCHAR*       → std::string
//   - BOOL         → bool
//   - VoiceProcessor を内包 (UpdateVolExp / UpdateOpLFO を代替)
//   - ポリチャンネル管理を ChState に集約

#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include "fitom/VoiceProcessor.h"
#include "fitom/PatchData.h"
#include "fitom/Fnum.h"
#include <cstdint>
#include <string>
#include <array>

namespace fitom {

class IMidiCh;  // forward

// ================================================================
//  チャンネル状態 (旧 CHATTR の後継)
// ================================================================
struct ChState {
    enum class Status { Empty, Disabled, Assigned, Running, Releasing };

    Status   status    = Status::Empty;
    bool     autoAssign = true;   // 旧 dva
    IMidiCh* owner     = nullptr;

    HwPatch  hwPatch;             // 現在割り当てられた HW パッチ
    VoiceProcessor proc;          // SW パラメータ処理エンジン

    uint8_t  lastNote  = 0xFF;
    int16_t  fineFreq  = 0;
    uint16_t noteOnAge = 0;       // 古いチャンネルを奪う優先度

    // レガシー互換: FNUM (旧 ISoundDevice::FNUM)
    struct Fnum {
        uint8_t  block = 0;
        uint16_t fnum  = 0;
    } lastFnum;

    uint8_t  velocity   = 100;
    uint8_t  volume     = 127;
    uint8_t  expression = 127;
    int8_t   panpot     = 0;    // -64..+63
    bool     sustain    = false;

    uint16_t releaseTimer = 0;  // Releasing フェーズの残り tick 数

    bool isEmpty()     const { return status == Status::Empty; }
    bool isRunning()   const { return status == Status::Running; }
    bool isReleasing() const { return status == Status::Releasing; }
    bool isActive()    const { return status == Status::Running
                                   || status == Status::Releasing; }
    bool isAssigned()  const { return status == Status::Assigned; }
    bool isEnabled()   const { return status != Status::Disabled; }

    void init() {
        status       = Status::Empty;
        autoAssign   = true;
        owner        = nullptr;
        lastNote     = 0xFF;
        fineFreq     = 0;
        noteOnAge    = 0;
        releaseTimer = 0;
        velocity     = 100;
        volume       = 127;
        expression   = 127;
        panpot       = 0;
        sustain      = false;
        proc.reset();
    }

    // kReleasingHoldMs: リリースフェーズ保持時間 (ms)
    // 大半の FM 音色で 2 秒あれば十分消音する
    static constexpr uint16_t kReleasingHoldMs = 2000;

    void assign(IMidiCh* ch) { owner = ch; status = Status::Assigned; }
    void run()               { status = Status::Running; noteOnAge = 0; }
    void release() {
        status       = Status::Releasing;
        releaseTimer = kReleasingHoldMs;
    }
    void free()    { owner = nullptr; status = Status::Empty; proc.reset(); }
    void disable()           { status = Status::Disabled; }
    void enable()            { if (status == Status::Disabled) status = Status::Empty; }
};

// ================================================================
//  ISoundDevice: 純粋仮想インターフェース
// ================================================================
class ISoundDevice {
public:
    virtual ~ISoundDevice() = default;

    // ─── デバイス情報 ───────────────────────────────────────────────────
    virtual uint8_t     getDeviceType() const = 0;
    virtual uint8_t     getChCount()    const = 0;
    virtual IPort*      getPort()             = 0;
    virtual std::string getDescriptor() const = 0;

    // ─── チャンネル割り当て ─────────────────────────────────────────────
    virtual uint8_t allocCh(IMidiCh* owner, const HwPatch* patch)              = 0;
    virtual uint8_t assignCh(uint8_t ch, IMidiCh* owner, const HwPatch* patch) = 0;
    // queryCh は findBestCh に統合 (後方互換のため残す)
    virtual uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode)    = 0;
    // releaseCh は noteOff に内包 (後方互換のため残す)
    virtual void    releaseCh(uint8_t ch)                                       = 0;
    virtual void    enableCh(uint8_t ch, bool enable)                       = 0;
    virtual uint8_t getAvailableChs() const                                 = 0;

    // ─── 発音制御 ───────────────────────────────────────────────────────
    virtual void noteOn(uint8_t ch, uint8_t vel)                            = 0;
    virtual void noteOff(uint8_t ch)                                        = 0;

    // ch が今も owner の発音として有効か確認する (Running/Releasing かつ owner 一致)。
    // Sostenuto の遅延 NoteOff 等、保持していたチャンネル参照が
    // ボイススティールで別の発音に再利用されていないかの確認に使う。
    virtual bool isChOwnedBy(uint8_t ch, const IMidiCh* owner) const = 0;

    // フォースダンプ: 発音中チャンネルを強制的に急速減衰させる。
    // CC#120 (All Sound Off) から呼ばれる。noteOff とは異なり、
    // EG のリリースレートを一時的に最大化してから release() する。
    // デフォルト実装: 単に noteOff() を呼ぶ (EG を持たないデバイス向け)。
    // FM 系チップは各ドライバでオーバーライドし、RR を最大値に書いてから
    // noteOff() を呼ぶことで通常のリリースより速く消音する。
    virtual void forceDamp(uint8_t ch) { noteOff(ch); }

    // マスターピッチ変更通知 (デフォルト実装: FnumTable キャッシュ破棄 + 再計算)
    virtual void onMasterPitchChanged(double pitchHz);

    // ─── パラメータ設定 ─────────────────────────────────────────────────
    virtual void setVoice(uint8_t ch, const HwPatch& patch, bool update = true)  = 0;
    virtual void setNoteFine(uint8_t ch, uint8_t note, int16_t fine, bool update = true) = 0;
    virtual void setVolume(uint8_t ch, uint8_t vol, bool update = true)     = 0;
    virtual void setVelocity(uint8_t ch, uint8_t vel, bool update = true)   = 0;
    virtual void setExpression(uint8_t ch, uint8_t exp, bool update = true) = 0;
    virtual void setPanpot(uint8_t ch, int8_t pan, bool update = true)      = 0;
    virtual void setSustain(uint8_t ch, bool sus, bool update = true)       = 0;
    virtual void setMasterVolume(uint8_t vol)                               = 0;

    // ─── HW LFO (チップ内蔵 LFO) ────────────────────────────────────────
    virtual void enablePM(uint8_t ch, bool on)              {}
    virtual void enableAM(uint8_t ch, bool on)              {}
    virtual void setPMDepth(uint8_t ch, uint8_t dep)        {}
    virtual void setAMDepth(uint8_t ch, uint8_t dep)        {}
    virtual void setPMRate(uint8_t ch, uint8_t rate)        {}
    virtual void setAMRate(uint8_t ch, uint8_t rate)        {}

    // ─── 直接レジスタアクセス ───────────────────────────────────────────
    virtual void    setReg(uint16_t reg, uint8_t data, bool forceWrite = false) = 0;
    virtual uint8_t getReg(uint16_t reg) const                                  = 0;
    virtual void    flush()                                                     {}

    // ─── ライフサイクル ─────────────────────────────────────────────────
    virtual void reset() = 0;
    virtual void init()  = 0;

    // ─── コールバック ───────────────────────────────────────────────────
    virtual void pollingCallback()             {}
    virtual void timerCallback(uint32_t tick)  = 0;

    // ─── モニタリング ───────────────────────────────────────────────────
    virtual const HwPatch* getCurrentPatch(uint8_t ch) const   = 0;
    virtual uint8_t        getCurrentNote(uint8_t ch)  const   = 0;
};

// ================================================================
//  CSoundDevice: ISoundDevice の共通実装基底クラス
//  チップドライバはこれを継承する。
// ================================================================
class CSoundDevice : public ISoundDevice {
public:
    static constexpr int MAX_CHS = 16;

    CSoundDevice(uint8_t deviceType, uint8_t maxChs, IPort* port,
                 int fnumMaster, int fnumDivide,
                 int noteOffset = -576,
                 FnumTableType fnumType = FnumTableType::Fnumber,
                 int regSize = 0);
    ~CSoundDevice() override;

    // ─── ISoundDevice 実装 ──────────────────────────────────────────────
    uint8_t     getDeviceType() const override { return deviceType_; }
    uint8_t     getChCount()    const override { return maxChs_; }
    IPort*      getPort()             override { return port_; }
    std::string getDescriptor() const override;

    uint8_t allocCh(IMidiCh* owner, const HwPatch* patch) override;
    uint8_t assignCh(uint8_t ch, IMidiCh* owner, const HwPatch* patch) override;
    uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode) override;
    void    releaseCh(uint8_t ch) override;

    // 1パス走査でベストチャンネルを選択する内部実装
    // (allocCh / queryCh はこれを呼ぶ)
    uint8_t findBestCh(IMidiCh* owner, const HwPatch* patch,
                       bool allowSteal) const noexcept;
    void    enableCh(uint8_t ch, bool enable) override;
    uint8_t getAvailableChs() const override;

    void noteOn(uint8_t ch, uint8_t vel) override;
    void noteOff(uint8_t ch) override;
    bool isChOwnedBy(uint8_t ch, const IMidiCh* owner) const override;

    void setVoice(uint8_t ch, const HwPatch& patch, bool update = true) override;
    void setNoteFine(uint8_t ch, uint8_t note, int16_t fine, bool update = true) override;
    void setVolume(uint8_t ch, uint8_t vol, bool update = true) override;
    void setVelocity(uint8_t ch, uint8_t vel, bool update = true) override;
    void setExpression(uint8_t ch, uint8_t exp, bool update = true) override;
    void setPanpot(uint8_t ch, int8_t pan, bool update = true) override;
    void setSustain(uint8_t ch, bool sus, bool update = true) override;
    void setMasterVolume(uint8_t vol) override;

    void    setReg(uint16_t reg, uint8_t data, bool forceWrite = false) override;
    uint8_t getReg(uint16_t reg) const override;

    void reset() override;
    void timerCallback(uint32_t tick) override;

    const HwPatch* getCurrentPatch(uint8_t ch) const override;
    uint8_t        getCurrentNote(uint8_t ch)  const override;

    // ─── 旧 API 互換 (移行期) ───────────────────────────────────────────
    // チップドライバが既存コードを参照する箇所のため残す
    ChState* getChState(uint8_t ch) {
        return (ch < maxChs_) ? &chState_[ch] : nullptr;
    }
    const ChState* getChState(uint8_t ch) const {
        return (ch < maxChs_) ? &chState_[ch] : nullptr;
    }

protected:
    // ─── チップ固有の実装 (純粋仮想) ────────────────────────────────────
    virtual void updateVoice(uint8_t ch)                      = 0;
    virtual void updateFreq(uint8_t ch, const ChState::Fnum* fnum = nullptr) = 0;
    virtual void updateVolExp(uint8_t ch)                     = 0;
    virtual void updatePanpot(uint8_t ch)                     = 0;
    virtual void updateSustain(uint8_t ch)                    = 0;
    virtual void updateKey(uint8_t ch, bool keyOn)            = 0;
    virtual void updateTL(uint8_t ch, uint8_t op, uint8_t tl) = 0;
    virtual void updateFnumber(uint8_t ch, bool forceWrite = true);

    // ─── Fnum 計算 ──────────────────────────────────────────────────────
    virtual ChState::Fnum getFnumber(uint8_t ch, int16_t offset = 0) const;

    // ─── 共有リソース ───────────────────────────────────────────────────
    uint8_t         deviceType_;
    uint8_t         maxChs_;
    uint8_t         opCount_;
    IPort*          port_;
    ChState         chState_[MAX_CHS];
    uint8_t*        regBak_;    // レジスタバックアップ
    size_t          regSize_;

    const uint16_t* fnumTable_;
    int             fnumMaster_;
    int             fnumDivide_;     // コンストラクタで設定
    FnumTableType   fnumType_;       // コンストラクタで設定
    int             noteOffset_;
    int             masterVolume_;
    uint8_t         priorCh_;   // 次に奪うチャンネルのヒント

    // キャリアマスク (アルゴリズム → キャリア OP ビットマスク)
    static const uint8_t kCarrierMask[8];
};

} // namespace fitom
