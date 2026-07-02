#pragma once
// fitom/Config.h  (改訂版)
// FITOMConfig — ISoundDevice を直接保持するよう拡張

#include "fitom/IPort.h"
#include "fitom/FmEnginePort.h"
#include "fitom/HWPort.h"
#include "fitom/PatchData.h"
#include "fitom/ISoundDevice.h"

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>

namespace fitom {

class PatchManager; // 前方宣言 (loadDrumBanks の引数用)

class ISoundDevice;

struct DeviceEntry {
    std::string                    label;
    // shared_ptr: sub-device 自動生成 (例: OPNA→FM+SSG+ADPCMB) で
    // 複数の DeviceEntry が同一の物理/エミュレーターポートを共有するため。
    std::shared_ptr<IPort>         port;
    std::unique_ptr<ISoundDevice>  device;
    uint32_t                       deviceType  = 0;
    int                            sampleRate  = 44100;
    // B-2: 2ポートチップ用の2番目のポート（HW SPFM extra_slot）
    std::shared_ptr<IPort>         port2;        // nullptr = 1ポート
    int                            extraSlot   = -1; // -1 = 未使用
    // リズムモード (OPLL/OPL/OPL2/OPL3 等、チップ内蔵リズム音源を持つ
    // デバイス共通のオプション)。特定チップに限定しない汎用フィールド。
    bool                           rhythmMode  = false;
    // sub-device 自動生成で同一チップ指定から生成された兄弟エントリを
    // 識別するためのグループID (-1 = 単独デバイス)。同じ物理ポートを
    // 共有するデバイス群は同じ compositeGroup を持つ。
    int                            compositeGroup = -1;
};

struct ChannelMapEntry {
    int midiCh      = 0;
    int deviceIndex = 0;
    int poly        = 1;
};

class IPortFactory {
public:
    virtual ~IPortFactory() = default;
    virtual std::unique_ptr<IPort> createPort(const nlohmann::json& cfg) { return nullptr; }
};

class FITOMConfig {
public:
    explicit FITOMConfig(std::unique_ptr<IPortFactory> factory = nullptr);
    virtual ~FITOMConfig();

    bool loadSystemConf(const std::filesystem::path& path);
    bool loadProfile(const std::filesystem::path& path);
    bool loadLegacyIni(const std::filesystem::path& path);

    void setHWPlugin(std::shared_ptr<HWPluginInstance> plugin);

    int              getDeviceCount()              const;
    IPort*           getDevicePort(int index)      const;
    // B-2: 2ポートチップ (OPN2/OPNA/OPL3等) の2番目のポート。単一ポート
    // デバイスや未使用の場合は nullptr。
    IPort*           getDevicePort2(int index)     const;
    ISoundDevice*    getDevice(int index)          const;
    uint32_t         getDeviceType(int index)      const;
    // リズムモード (OPLL/OPL系等、チップ内蔵リズム音源の有効/無効)
    bool             getDeviceRhythmMode(int index) const;
    std::string      getDeviceLabel(int index)     const;

    // ─── VoicePatchType (音色パッチ互換性分類) ──────────────────────────────
    // devices_[index] の deviceType (DEVICE_*) から対応する VoicePatchType
    // (VOICE_PATCH_* 、0x10〜0x74) を返す。未対応の場合は VOICE_PATCH_NONE(0)。
    uint8_t getVoicePatchType(int deviceIndex) const;

    // voicePatchType に一致する最初のデバイスのインデックスを返す。
    // 見つからない場合は -1。旧FITOMの CFITOMConfig::GetLogDeviceFromID 相当
    // (完全一致のみ、互換フォールバックは将来実装)。
    int findDeviceIndexByVoicePatchType(uint8_t voicePatchType) const;

    // deviceType (DEVICE_*) → VoicePatchType (VOICE_PATCH_*) の静的変換。
    // インスタンス状態に依存しないため static。
    static uint8_t deviceTypeToVoicePatchType(uint32_t deviceType) noexcept;

    // VoicePatchType (VOICE_PATCH_*) → VoiceGroup (HwBankRegistry検索キー) の静的変換。
    static uint32_t voicePatchTypeToVoiceGroup(uint8_t voicePatchType) noexcept;

    // プロファイル/バンクJSONの "group" 文字列 → VoicePatchType の静的変換。
    // 対応する文字列が無い場合は VOICE_PATCH_NONE(0) を返す。
    static uint8_t stringToVoicePatchType(const std::string& s) noexcept;

    // ─── Sub-device 自動生成 (composite chip) ──────────────────────────────
    // 1つの物理/エミュレーターチップ指定 (例: "OPNA") から、内部的に複数の
    // ISoundDevice インスタンス (例: FM本体 + SSG + ADPCM-B) を自動生成する。
    // 各サブデバイスは同一の物理ポートを共有するが、独立した devices_[] の
    // エントリとして登録され、それぞれ別の VoicePatchType で識別可能になる。
    struct SubDeviceSpec {
        uint32_t    deviceType;    // このサブデバイスの DEVICE_*
        const char* labelSuffix;   // ラベルに付与する接尾辞 (例: "-SSG")
        bool        usesExtraPort; // 2ポート目 (extraPort) を必要とするか
        bool        rhythmCapable; // rhythm_mode をこのサブデバイスに適用するか
    };

    // baseDeviceType (プロファイルの "chip" から解決した DEVICE_*) が
    // 複数サブデバイスへの展開を必要とするかどうか。
    // 展開が必要なら true を返し、outSpec に構成一覧を書き込む。
    // 展開不要 (単独デバイスのまま) なら false を返す。
    static bool resolveCompositeSpec(uint32_t baseDeviceType,
                                      std::vector<SubDeviceSpec>& outSpec);

    int                getMidiInputCount()          const;
    const std::string& getMidiInputName(int index)  const;

    const std::vector<ChannelMapEntry>& getChannelMap() const { return channelMap_; }

    const std::string& getAudioDevice()     const;
    uint32_t           getAudioSampleRate() const;

    void    setMasterVolume(uint8_t vol);
    uint8_t getMasterVolume()   const;
    double  getMasterPitch()    const { return masterPitch_; }
    void    setMasterPitch(double p)  { masterPitch_ = p; }

    FmEngineRegistry& getFmEngineRegistry();

    using ProgressCb = std::function<void(const std::string&)>;
    void setProgressCallback(ProgressCb cb) { progressCb_ = std::move(cb); }

protected:
    virtual bool buildFromProfile(const nlohmann::json& j);
    virtual bool buildFromLegacyIni(const nlohmann::json& ini);
    virtual void buildDevice(const nlohmann::json& dev);

    // buildDevice() の FMENGINE/HW 両ブランチ共通処理。
    // composite chip (resolveCompositeSpec が true を返す場合) なら
    // 複数の DeviceEntry を、そうでなければ単独の DeviceEntry を devices_ に追加する。
    void pushDeviceEntries(const std::string& baseLabel, uint32_t baseDeviceType,
                            std::shared_ptr<IPort> port, std::shared_ptr<IPort> port2,
                            int sampleRate, int extraSlot, bool rhythmModeFromProfile);
    int nextCompositeGroupId_ = 0;
    // profile の banks.drum_banks[] を PatchManager に登録する
    void loadDrumBanks(const nlohmann::json& j, PatchManager& pm,
                        const std::filesystem::path& baseDir);
    virtual void validateProfile();
    virtual void loadLegacyManualDevices(const nlohmann::json& ini);

    FmEngineRegistry fmRegistry_;
    std::shared_ptr<HWPluginInstance> hwPlugin_;

    std::vector<DeviceEntry>     devices_;
    std::vector<std::string>     midiInputNames_;
    std::vector<ChannelMapEntry> channelMap_;

    std::string audioDevice_;
    uint32_t    audioSampleRate_ = 48000;
    uint8_t     masterVolume_    = 100;
    double      masterPitch_     = 440.0;

    nlohmann::json systemConf_;
    nlohmann::json profileJson_;

    std::unique_ptr<IPortFactory> factory_;
    ProgressCb progressCb_;
};

} // namespace fitom
