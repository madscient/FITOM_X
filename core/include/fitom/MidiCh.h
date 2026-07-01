#pragma once
// fitom/MidiCh.h
// MIDI チャンネル処理クラス (モダナイズ版)
//
// 旧 MIDI.h からの変更点:
//   - FMVOICE* → PatchResolver (マルチレイヤー対応)
//   - CSoundDevice* Device → 複数デバイス (layers 経由)
//   - TCHAR → std::string
//   - BOOL → bool
//   - BOOST 依存なし

#include "fitom/FITOMdefine.h"
#include "fitom/PatchData.h"
#include "fitom/PatchManager.h"
#include "fitom/VoiceProcessor.h"
#include "fitom/ISoundDevice.h"
#include "fitom/DrumData.h"
#include <cstdint>
#include <string>
#include <array>
#include <vector>

namespace fitom {

class CFITOM;
class PatchManager;

// ================================================================
//  IMidiCh: MIDI チャンネルインターフェース (旧 CMidiCh)
// ================================================================
class IMidiCh {
public:
    virtual ~IMidiCh() = default;

    // チャンネルメッセージ
    virtual void progChange(uint8_t prog)         {}
    virtual void noteOn(uint8_t note, uint8_t vel){}
    virtual void noteOff(uint8_t note)            {}
    virtual void allNoteOff()                      {}
    // CC#120 (All Sound Off): デフォルトは allNoteOff() と同じ。
    // FM 系チャンネル (CInstCh) は forceDamp を使う独自実装でオーバーライドする。
    virtual void allSoundOff()                     { allNoteOff(); }
    virtual void resetAllCtrl()                    {}

    // コントロールチェンジ
    virtual void setVolume(uint8_t vol)            {}
    virtual void setExpression(uint8_t exp)        {}
    virtual void setPanpot(uint8_t pan)            {}
    virtual void setPitchBend(uint16_t pb)         {}
    virtual void setSustain(uint8_t sus)           {}
    virtual void setModulation(uint8_t dep)        {}
    virtual void setFootCtrl(uint8_t dep)          {}
    virtual void setBreathCtrl(uint8_t dep)        {}
    virtual void setPortamento(bool on)            {}
    virtual void setPortTime(uint8_t pt)           {}
    virtual void setLegato(bool leg)               {}
    virtual void setSostenuto(bool sos)            {}
    virtual void setForceDamp(bool fd)             {}
    virtual void bankSelMSB(uint8_t msb)           {}
    virtual void bankSelLSB(uint8_t lsb)           {}

    // RPN / NRPN
    virtual void setBendRange(uint8_t range)       {}
    virtual void setFineTune(uint16_t tune)        {}
    virtual void setRPNRegister(uint16_t reg, uint16_t val)  {}
    virtual void setNRPNRegister(uint16_t reg, uint16_t val) {}

    // コールバック
    virtual void timerCallback(uint32_t tick)      = 0;
    virtual void pollingCallback()                 {}
    virtual void midiClockCallback(uint32_t tick)  {}

    // モニタリング
    virtual uint8_t  getLastNote()    const { return 0xFF; }
    virtual uint8_t  getProgramNo()   const { return 0xFF; }
    virtual uint16_t getBankNo()      const { return 0; }
    virtual uint8_t  getVolume()      const { return 127; }
    virtual uint8_t  getExpression()  const { return 127; }
    virtual uint8_t  getPanpot()      const { return 64; }
    virtual uint16_t getPitchBend()   const { return 8192; }
    virtual bool     getSustain()     const { return false; }
    virtual uint8_t  getPoly()        const { return 0; }

    virtual bool isInst()   const { return false; }
    virtual bool isRhythm() const { return false; }
    virtual bool isThru()   const { return false; }
};

// ================================================================
//  PortaCtrl: ポルタメント制御 (旧 CPortaCtrl 相当)
// ================================================================
class PortaCtrl {
public:
    enum class State { Idle, Running };

    PortaCtrl() = default;

    void start(uint8_t dst)  { end_ = dst; state_ = State::Running; count_ = 0; }
    void setSource(uint8_t s){ start_ = current_ = s; fine_ = 0; }
    void stop()              { state_ = State::Idle; }
    void enable(bool e)      { enabled_ = e; }
    void setSpeed(uint8_t s) { speed_ = s; }
    void update();

    bool    isEnabled()     const { return enabled_; }
    bool    isRunning()     const { return state_ == State::Running; }
    uint8_t getCurrentNote()const { return current_; }
    uint8_t getCurrentFine()const { return fine_; }
    uint8_t getSpeed()      const { return speed_; }

private:
    uint8_t start_   = 0;
    uint8_t end_     = 0;
    uint8_t current_ = 0;
    uint8_t fine_    = 0;
    uint8_t speed_   = 64;
    bool    enabled_ = false;
    State   state_   = State::Idle;
    uint32_t count_  = 0;
};

// ================================================================
//  CInstCh: 通常音色チャンネル (旧 CInstCh 相当)
//  マルチレイヤーパッチ対応版
// ================================================================
class CInstCh : public IMidiCh {
public:
    CInstCh(uint8_t ch, CFITOM* parent);
    ~CInstCh() override;

    // ─── チャンネル割り当て ─────────────────────────────────────────
    // 旧 Assign(CSoundDevice*, poly) の後継
    // profile の channel_map から PatchManager・devices を設定する
    void setup(PatchManager* pm, CFITOM* fitom, uint8_t poly);

    // ─── MIDI メッセージ ───────────────────────────────────────────
    void progChange(uint8_t prog) override;
    void noteOn(uint8_t note, uint8_t vel) override;
    void noteOff(uint8_t note) override;
    void allNoteOff() override;   // CC#123
    void allSoundOff() override;  // CC#120: forceDamp で即座に消音
    void resetAllCtrl() override;

    // ─── コントロールチェンジ ──────────────────────────────────────
    void setVolume(uint8_t vol) override;
    void setExpression(uint8_t exp) override;
    void setPanpot(uint8_t pan) override;
    void setPitchBend(uint16_t pb) override;
    void setSustain(uint8_t sus) override;
    void setModulation(uint8_t dep) override;
    void setFootCtrl(uint8_t dep) override;
    void setBreathCtrl(uint8_t dep) override;
    void setPortamento(bool on) override;
    void setPortTime(uint8_t pt) override;
    void setLegato(bool leg) override;
    void setSostenuto(bool sos) override;
    void setForceDamp(bool fd) override;
    void bankSelMSB(uint8_t msb) override;
    void bankSelLSB(uint8_t lsb) override;
    void setBendRange(uint8_t range) override;
    void setFineTune(uint16_t tune) override;
    void setRPNRegister(uint16_t reg, uint16_t val) override;
    void setNRPNRegister(uint16_t reg, uint16_t val) override;

    // ─── コールバック ──────────────────────────────────────────────
    void timerCallback(uint32_t tick) override;
    void midiClockCallback(uint32_t tick) override;

    // ─── モニタリング ──────────────────────────────────────────────
    uint8_t  getLastNote()   const override;
    uint8_t  getProgramNo()  const override { return programNo_; }
    uint16_t getBankNo()     const override { return bankSelL_; }
    uint8_t  getVolume()     const override { return volume_; }
    uint8_t  getExpression() const override { return expression_; }
    uint8_t  getPanpot()     const override { return panpot_; }
    uint16_t getPitchBend()  const override { return pitchBend_; }
    bool     getSustain()    const override { return sustain_; }
    uint8_t  getPoly()       const override { return poly_; }
    bool     isInst()        const override { return true; }

    // 現在のレイヤー数 (GUI モニタリング用)
    int activeLayerCount() const { return resolver_.layerCount(); }

private:
    // ─── ノート履歴 ────────────────────────────────────────────────
    struct NoteHist {
        uint8_t        layerIdx = 0xFF;
        uint8_t        devCh   = 0xFF;
        uint8_t        note    = 0xFF;
        ISoundDevice*  dev     = nullptr;
        bool isValid() const { return devCh != 0xFF; }
    };
    static constexpr int MAX_NOTES = 16;
    std::array<NoteHist, MAX_NOTES> notes_;
    int timbres_ = 0;   // 現在発音中のノート数

    NoteHist* findNote(uint8_t note, int layerIdx = -1);
    void enterNote(int layerIdx, uint8_t devCh, uint8_t note, ISoundDevice* dev);
    void leaveNote(int histIdx);

    // ─── 内部ヘルパー ──────────────────────────────────────────────
    ISoundDevice* getLayerDevice(int layerIdx) const;
    void applyVolExpToAll();
    void applyPanpotToAll();
    void applyPitchBendToAll();
    void applyLFOToAll();

    // ─── MIDI チャンネル状態 ───────────────────────────────────────
    uint8_t  ch_;
    uint8_t  programNo_  = 0;
    uint8_t  bankSelM_   = 0;
    uint8_t  bankSelL_   = 0;
    uint8_t  volume_     = 100;
    uint8_t  expression_ = 127;
    uint8_t  panpot_     = 64;
    uint16_t pitchBend_  = 8192;
    uint8_t  bendRange_  = 2;
    uint16_t tuning_     = 8192;
    bool     sustain_    = false;
    bool     legato_     = false;
    bool     sostenuto_  = false;
    bool     forceDamp_  = false;
    uint8_t  poly_       = 1;
    bool     mono_       = false;
    uint8_t  pmDepth_        = 0;
    uint8_t  amDepth_        = 0;
    uint8_t  pmRate_         = 0;
    uint8_t  amRate_         = 0;
    int16_t  modDepthRange_  = 32; // RPN#5: CC#1=127 時の最大デプス [Fnum steps]
    uint8_t  phyCh_      = 127;

    // ─── パッチ管理 ────────────────────────────────────────────────
    PatchManager*  patchMgr_  = nullptr;
    CFITOM*        fitom_     = nullptr;
    PatchResolver  resolver_;

    // ─── ポルタメント ──────────────────────────────────────────────
    PortaCtrl portamento_;

    // ─── RPN / NRPN ────────────────────────────────────────────────
    uint16_t rpnReg_  = 0x3FFF;
    uint16_t nrpnReg_ = 0x3FFF;
    uint8_t  dataLSB_ = 0;
};

// ================================================================
//  CRhythmCh: リズムチャンネル
//  Program Change → DrumPatch 選択
//  Note On → DrumNote に従い発音
// ================================================================
class CRhythmCh : public IMidiCh {
public:
    CRhythmCh(uint8_t ch, CFITOM* parent);
    ~CRhythmCh() override;

    void progChange(uint8_t prog) override;
    void bankSelMSB(uint8_t msb) override;
    void bankSelLSB(uint8_t lsb) override;
    void setVolume(uint8_t vol) override;
    void noteOn(uint8_t note, uint8_t vel) override;
    void noteOff(uint8_t note) override;
    void allNoteOff() override;
    void resetAllCtrl() override;
    void timerCallback(uint32_t tick) override;
    void setRPNRegister(uint16_t reg, uint16_t val) override;
    void setNRPNRegister(uint16_t reg, uint16_t val) override;

    uint8_t  getLastNote()  const override { return lastNote_; }
    uint8_t  getProgramNo() const override { return programNo_; }
    uint8_t  getVolume()    const override { return volume_; }
    bool     isRhythm()     const override { return true; }

    int activeNoteCount() const;

private:
    // ─── レイヤー発音単位 ─────────────────────────────────────────
    // 1 受信ノートが最大 MAX_TONE_LAYERS 本の発音を持つ
    struct LayerSlot {
        ISoundDevice* dev    = nullptr;
        uint8_t       devCh  = 0xFF;
        uint8_t       layerIdx = 0;
        bool isActive() const { return dev != nullptr; }
    };

    // ─── ノート発音履歴 ───────────────────────────────────────────
    // noteHist_[midiNote] = そのノートが持つ全レイヤーの発音状態
    struct NoteSlots {
        std::array<LayerSlot, MAX_TONE_LAYERS> layers;
        uint16_t gateRem = 0;   // 残りゲートタイム (全レイヤー共通)
        bool anyActive() const {
            for (const auto& l : layers) if (l.isActive()) return true;
            return false;
        }
        void stopAll() {
            for (auto& l : layers) {
                if (l.isActive()) {
                    l.dev->noteOff(l.devCh);
                    l.dev->releaseCh(l.devCh);
                    l = LayerSlot{};
                }
            }
            gateRem = 0;
        }
    };

    // ─── ノートごとのリアルタイム調整 (NRPN) ─────────────────────
    struct NoteAdj {
        int16_t pan   = 0;
        int16_t vel   = 0;
        int16_t pitch = 0;
    };

    std::array<NoteSlots, 128> noteSlots_;
    std::array<NoteAdj,   128> noteAdj_;

    // ─── ノートごとの PatchResolver キャッシュ ───────────────────
    // progChange 時に全ノートのリゾルバを更新するのは重いため、
    // NoteOn 時にオンデマンドで解決してキャッシュする
    struct NoteCache {
        uint8_t       patchBank = 0xFF;
        uint8_t       patchProg = 0xFF;
        ResolvedPatch resolved;
        bool isValid() const { return resolved.isValid(); }
    };
    std::array<NoteCache, 128> noteCache_;

    CFITOM*  fitom_     = nullptr;
    uint8_t  ch_        = 9;
    uint8_t  lastNote_  = 0xFF;
    uint8_t  volume_    = 100;
    uint8_t  programNo_ = 0;
    uint8_t  bankSelM_  = 0;
    uint8_t  bankSelL_  = 0;

    const DrumPatch* currentPatch_ = nullptr;

    // NoteOn 内部処理
    void applyNoteOn(uint8_t midiNote, uint8_t vel, const DrumNote& dn);
    // ノートの ResolvedPatch を取得（キャッシュ優先）
    const ResolvedPatch* resolveNote(uint8_t midiNote, const DrumNote& dn);
    // キャッシュ全クリア（progChange 時）
    void clearNoteCache();
};

} // namespace fitom
