#pragma once
// fitom/CFITOM.h
// FITOM コアシングルトン — モダナイズ版
//
// 旧 FITOM.h からの変更:
//   - boost::thread → std::thread / std::mutex
//   - FMVOICE* → HwPatch* / PatchManager
//   - CPort* / CSoundDevice* → fitom::IPort* / fitom::ISoundDevice*
//   - TCHAR → std::string
//   - CMidiInst → MidiProcessor
//   - MFCダイアログ依存を排除

#include "fitom/FITOMdefine.h"
#include "fitom/Config.h"
#include "fitom/HWPort.h"
#include "fitom/PatchData.h"
#include "fitom/PatchManager.h"
#include "fitom/MidiManager.h"
#include "fitom/ISoundDevice.h"
#include "fitom/MidiCh.h"
#include "fitom/DrumData.h"
#include "fitom/Log.h"
#include "fitom/IPort.h"
#include "fitom/DeviceFactory.h"

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>

namespace fitom {


// ================================================================
//  MidiProcessor: MIDI バイト列 → CMidiCh の MIDI 処理エンジン
//  旧 CMidiInst を std::thread ベースでリプレース
// ================================================================
class MidiProcessor {
public:
    MidiProcessor(std::array<std::unique_ptr<IMidiCh>, 16>& channels,
                  CFITOM* parent, bool clockEnabled);
    ~MidiProcessor() = default;

    // MidiManager から受け取った生バイトを処理する
    // コールバックスレッドから呼ばれる
    void receiveByte(const uint8_t* data, size_t len, uint64_t timestampNs);

    void timerCallback(uint32_t tick);
    void pollingCallback();
    void midiClockCallback(uint32_t tick);

    IMidiCh* getChannel(uint8_t ch) {
        return (ch < 16) ? channels_[ch].get() : nullptr;
    }

private:
    std::array<std::unique_ptr<IMidiCh>, 16>& channels_;
    CFITOM* parent_;
    bool clockEnabled_;

    // MIDI 状態機械
    enum class State { Ready, Wait1, Wait2, SysEx } state_ = State::Ready;
    uint8_t msgBuf_[4] = {};
    uint8_t msgPt_     = 0;
    uint8_t sysexBuf_[8192] = {};
    uint16_t sysexPt_  = 0;
    uint16_t currentStatus_ = 0;

    void processMessage();
    void processSysEx();
    void processControl(uint8_t ch, uint8_t cc, uint8_t val);
    void processRPN(uint8_t ch);
    void processNRPN(uint8_t ch);

    // RPN / NRPN ステート
    struct RpnState {
        uint16_t reg  = 0x3FFF;
        uint8_t  lsb  = 0;
        bool     isNrpn = false;
    };
    std::array<RpnState, 16> rpn_;
};

// ================================================================
//  CFITOM: コアシングルトン
// ================================================================
class CFITOM {
public:
    // シングルトンアクセス
    static CFITOM& instance() {
        static CFITOM inst;
        return inst;
    }

    // ─── 初期化・終了 ──────────────────────────────────────────────
    // config: FITOMConfig 所有権を移譲する
    int  init(std::unique_ptr<FITOMConfig> config,
              std::unique_ptr<PatchManager> patchMgr);
    void exit(bool save = false);

    // ─── デバイスアクセス ─────────────────────────────────────────
    ISoundDevice* getDevice(int index) const;
    int           getDeviceCount() const;

    // CRhythmCh::NoteOn から呼ばれるドラムノート解決 (bankNo = CC#0 値)
    const DrumNote* getDrum(int bankNo, uint8_t prog, uint8_t midiNote) const;

    FITOMConfig& getConfig() const { return *config_; }
    PatchManager& getPatchManager() { return *patchMgr_; }

    // ─── MIDIインスタンスアクセス ─────────────────────────────────
    MidiProcessor* getMidiProcessor(uint8_t idx) {
        return (idx < MAX_MPUS && processors_[idx]) ? processors_[idx].get() : nullptr;
    }

    // ─── タイマー・ポーリング ────────────────────────────────────
    void timerCallback(uint32_t tick);
    int  pollingCallback();
    void midiClockCallback();

    // ─── グローバル操作 ───────────────────────────────────────────
    void allNoteOff();
    void resetAllCtrl();
    void setMasterVolume(uint8_t vol);
    uint8_t getMasterVolume() const;

    // ─── 静的ユーティリティ (旧 CFITOM static メソッド) ─────────
    static uint32_t         getDeviceVoiceType(uint32_t deviceId);
    static uint32_t         getDeviceVoiceGroupMask(uint32_t deviceId);
    static uint32_t         getDeviceIdFromName(const std::string& name);
    static const std::string getDeviceNameFromId(uint32_t deviceId);
    static uint32_t         getDeviceRegSize(uint32_t deviceId);

    // ─── コールバック登録 ────────────────────────────────────────
    using StatusCallback = std::function<void(const std::string&)>;
    void setStatusCallback(StatusCallback cb) { statusCb_ = std::move(cb); }

    // ─── スレッド制御 ────────────────────────────────────────────
    // タイマースレッドを開始する（MFC以外の環境用）
    // MFC環境ではタイマーはシステムから呼ばれるため不要
    void startTimerThread(uint32_t intervalMs = 1);
    void stopTimerThread();

private:
    CFITOM() = default;
    ~CFITOM() { exit(); }
    CFITOM(const CFITOM&) = delete;
    CFITOM& operator=(const CFITOM&) = delete;

    static constexpr int MAX_MPUS = 4;

    // ─── 所有リソース ─────────────────────────────────────────────
    std::unique_ptr<FITOMConfig>   config_;
    std::unique_ptr<PatchManager>  patchMgr_;
    std::unique_ptr<MidiManager>   midiMgr_;

    // MIDI プロセッサとチャンネル (入力ポートごと)
    std::array<std::array<std::unique_ptr<IMidiCh>, 16>, MAX_MPUS> channels_;
    std::array<std::unique_ptr<MidiProcessor>, MAX_MPUS> processors_;

    // ─── デバイスリスト (CFITOM が ISoundDevice を所有) ──────────
    // Config は IPort を所有し、CFITOM は ISoundDevice を所有する
    std::vector<std::unique_ptr<ISoundDevice>> devices_;

    // ─── SplitPort の寿命管理 ─────────────────────────────────────
    // OPNA/OPN2 等の HW 2 ポート構成時に生成する SplitPort。
    // IPort* として CSoundDevice に渡すが、所有権はここで管理する。
    std::vector<std::unique_ptr<SplitPort>> splitPorts_;

    // ─── タイマースレッド ─────────────────────────────────────────
    std::thread         timerThread_;
    std::atomic<bool>   timerRunning_{false};
    uint32_t            timerTick_ = 0;

    // ─── 排他制御 ────────────────────────────────────────────────
    // プロセス間排他: boost::interprocess::named_mutex を使用
    // (std::mutex はプロセス間不可のため Boost のまま維持)
    mutable std::mutex  processMutex_;

    // ─── コールバック ─────────────────────────────────────────────
    StatusCallback statusCb_;

    // 内部ヘルパー
    int  setupMidiInputs();
    void initDevices();
    void syncDeviceLatency();  // initDevices() から呼ばれる
    void timerThreadFunc(uint32_t intervalMs);
};

} // namespace fitom
