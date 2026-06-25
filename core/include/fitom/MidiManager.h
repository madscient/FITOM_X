#pragma once
// fitom/MidiManager.h
// MIDI バックエンド DLL のロードと管理
//
// IMidiPlugin.h の C API を動的ロードし、
// コアから使いやすい C++ クラスとしてラップする。

#include "fitom/PluginLoader.h"
#include "fitom/IMidiPlugin.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <filesystem>

namespace fitom {

// -------------------------------------------------------
//  MidiPluginInstance: MIDI バックエンド DLL の RAII ラッパー
// -------------------------------------------------------
class MidiPluginInstance {
public:
    using PFN_GetName       = const char*  (FITOM_MIDIP_CALL*)();
    using PFN_EnumerateIn   = const char*  (FITOM_MIDIP_CALL*)();
    using PFN_EnumerateOut  = const char*  (FITOM_MIDIP_CALL*)();
    using PFN_FreeString    = void         (FITOM_MIDIP_CALL*)(const char*);
    using PFN_OpenIn        = MidiResult   (FITOM_MIDIP_CALL*)(const char*, MidiInCallback, void*, MidiInHandle*);
    using PFN_CloseIn       = void         (FITOM_MIDIP_CALL*)(MidiInHandle);
    using PFN_OpenOut       = MidiResult   (FITOM_MIDIP_CALL*)(const char*, MidiOutHandle*);
    using PFN_CloseOut      = void         (FITOM_MIDIP_CALL*)(MidiOutHandle);
    using PFN_Send          = MidiResult   (FITOM_MIDIP_CALL*)(MidiOutHandle, const uint8_t*, size_t, uint64_t);

    static std::shared_ptr<MidiPluginInstance> load(const std::filesystem::path& dllPath);

    std::string              name()         const;
    std::vector<std::string> enumerateIn()  const;
    std::vector<std::string> enumerateOut() const;

    PFN_FreeString  FreeString = nullptr;
    PFN_OpenIn      OpenIn     = nullptr;
    PFN_CloseIn     CloseIn    = nullptr;
    PFN_OpenOut     OpenOut    = nullptr;
    PFN_CloseOut    CloseOut   = nullptr;
    PFN_Send        Send       = nullptr;

private:
    MidiPluginInstance() = default;
    PluginLoader    loader_;
    PFN_GetName     GetName_      = nullptr;
    PFN_EnumerateIn  EnumerateIn_  = nullptr;
    PFN_EnumerateOut EnumerateOut_ = nullptr;
};

// -------------------------------------------------------
//  MidiInPort: MIDI In デバイスの RAII ラッパー
// -------------------------------------------------------
class MidiInPort {
public:
    using Callback = std::function<void(const uint8_t*, size_t, uint64_t)>;

    MidiInPort(std::shared_ptr<MidiPluginInstance> plugin,
               const std::string& deviceName,
               Callback callback);
    ~MidiInPort();

    MidiInPort(const MidiInPort&)            = delete;
    MidiInPort& operator=(const MidiInPort&) = delete;

    const std::string& deviceName() const { return deviceName_; }

private:
    static void FITOM_MIDIP_CALL rawCallback(
        const uint8_t* data, size_t len,
        uint64_t timestamp_ns, void* user_data);

    std::shared_ptr<MidiPluginInstance> plugin_;
    std::string                         deviceName_;
    Callback                            callback_;
    MidiInHandle                        handle_ = nullptr;
};

// -------------------------------------------------------
//  MidiManager: MIDI In/Out の生存管理
// -------------------------------------------------------
class MidiManager {
public:
    // DLL をロードする
    bool loadPlugin(const std::filesystem::path& dllPath);

    bool isLoaded() const { return plugin_ != nullptr; }

    // デバイス列挙
    std::vector<std::string> enumerateIn()  const;
    std::vector<std::string> enumerateOut() const;

    // 指定デバイス名で MIDI In を開く
    // callback: (data, len, timestamp_ns) を受け取る関数
    bool openIn(const std::string& deviceName,
                MidiInPort::Callback callback);

    // 全 MIDI In を閉じる
    void closeAll();

    // プラグイン名
    std::string pluginName() const;

private:
    std::shared_ptr<MidiPluginInstance>  plugin_;
    std::vector<std::unique_ptr<MidiInPort>> openPorts_;
};

} // namespace fitom
