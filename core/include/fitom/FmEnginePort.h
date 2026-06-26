#pragma once
// fitom/FmEnginePort.h
// FmEngineApi 互換 DLL → IPort アダプター

#include "IPort.h"
#include "PluginLoader.h"
#include <FmEngineApi.h>  // backends/fm_engine/include/ に配置
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace fitom {

// -------------------------------------------------------
//  FmEngineApi.h の各関数に対応する関数ポインタ typedef
//  FmEngineApi.h は typedef を提供しないので、ここで定義する
// -------------------------------------------------------
using PFN_FmEngine_Create          = FmEngineHandle (FMENGINE_CALL*)(uint32_t);
using PFN_FmEngine_Destroy         = void           (FMENGINE_CALL*)(FmEngineHandle);
using PFN_FmEngine_Inquiry         = uint32_t       (FMENGINE_CALL*)(FmEngineHandle);
using PFN_FmEngine_GetSupportedChip= const char*    (FMENGINE_CALL*)(FmEngineHandle, uint32_t);
using PFN_FmEngine_AddChip         = FmResult       (FMENGINE_CALL*)(FmEngineHandle, const char*, uint32_t, uint32_t*);
using PFN_FmEngine_GetChipName     = const char*    (FMENGINE_CALL*)(FmEngineHandle, uint32_t);
using PFN_FmEngine_GetNativeRate   = uint32_t       (FMENGINE_CALL*)(FmEngineHandle, uint32_t);
using PFN_FmEngine_GetSampleRate   = uint32_t       (FMENGINE_CALL*)(FmEngineHandle);
using PFN_FmEngine_Write           = FmResult       (FMENGINE_CALL*)(FmEngineHandle, uint32_t, uint8_t, uint8_t, uint32_t);
using PFN_FmEngine_SetGain         = FmResult       (FMENGINE_CALL*)(FmEngineHandle, uint32_t, float, float);
using PFN_FmEngine_GetGain         = FmResult       (FMENGINE_CALL*)(FmEngineHandle, uint32_t, float*, float*);
using PFN_FmEngine_SetMemory       = FmResult       (FMENGINE_CALL*)(FmEngineHandle, uint32_t, FmMemoryType, const uint8_t*, uint32_t);
using PFN_FmEngine_GetMemorySize   = uint32_t       (FMENGINE_CALL*)(FmEngineHandle, uint32_t, FmMemoryType);
using PFN_FmEngine_Generate        = FmResult       (FMENGINE_CALL*)(FmEngineHandle, float*, float*, uint32_t);

// -------------------------------------------------------
//  FmEngineVtable: DLL から取得した関数ポインタ一式
// -------------------------------------------------------
struct FmEngineVtable {
    PFN_FmEngine_Create            Create           = nullptr;
    PFN_FmEngine_Destroy           Destroy          = nullptr;
    PFN_FmEngine_Inquiry           Inquiry          = nullptr;
    PFN_FmEngine_GetSupportedChip  GetSupportedChip = nullptr;
    PFN_FmEngine_AddChip           AddChip          = nullptr;
    PFN_FmEngine_GetChipName       GetChipName      = nullptr;
    PFN_FmEngine_GetNativeRate     GetNativeRate    = nullptr;
    PFN_FmEngine_GetSampleRate     GetSampleRate    = nullptr;
    PFN_FmEngine_Write             Write            = nullptr;
    PFN_FmEngine_SetGain           SetGain          = nullptr;
    PFN_FmEngine_GetGain           GetGain          = nullptr;
    PFN_FmEngine_SetMemory         SetMemory        = nullptr;
    PFN_FmEngine_GetMemorySize     GetMemorySize    = nullptr;
    PFN_FmEngine_Generate          Generate         = nullptr;
};

class FmEngineInstance {
public:
    static std::shared_ptr<FmEngineInstance> create(
        PluginLoader loader, uint32_t sampleRate);

    ~FmEngineInstance();

    FmEngineHandle        handle() const { return handle_; }
    const FmEngineVtable& vtable() const { return vtable_; }
    std::string           name()   const { return name_; }

    std::vector<std::string> supportedChips() const;

private:
    FmEngineInstance() = default;
    PluginLoader   loader_;
    FmEngineVtable vtable_;
    FmEngineHandle handle_ = nullptr;
    std::string    name_;
};

// -------------------------------------------------------
//  FmEngineRegistry
// -------------------------------------------------------
class FmEngineRegistry {
public:
    struct Entry {
        std::string            dllPath;
        uint32_t               sampleRate;
        std::shared_ptr<FmEngineInstance> instance;
    };

    void registerEngine(const std::string& name,
                        const std::string& dllPath,
                        uint32_t sampleRate = 48000);

    std::shared_ptr<FmEngineInstance> get(const std::string& name);

    void generateAll(float* outL, float* outR, uint32_t samples);

private:
    std::mutex                             mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

// -------------------------------------------------------
//  FmEnginePort: IPort 実装
// -------------------------------------------------------
class FmEnginePort final : public IPort {
public:
    FmEnginePort(std::shared_ptr<FmEngineInstance> engine,
                 const std::string& chipName,
                 uint32_t clock = 0);
    ~FmEnginePort() override = default;

    void     write(uint16_t addr, uint16_t data)    override;
    void     writeRaw(uint16_t addr, uint16_t data) override { write(addr, data); }
    uint8_t  read(uint16_t)                         override { return 0; }
    uint8_t  status()                               override { return 0; }
    void     reset()                                override {}

    std::string getDesc()          override { return "FmEngine:" + chipName_; }
    std::string getInterfaceDesc() override { return engine_->name(); }
    int         getClock()         override { return static_cast<int>(nativeRate_); }
    int         getPanpot()        override { return 0; }

    uint32_t chipId() const { return chipId_; }
    void setGain(float l, float r);
    void setMemory(FmMemoryType type, const uint8_t* data, uint32_t size);

private:
    std::shared_ptr<FmEngineInstance> engine_;
    std::string chipName_;
    uint32_t    chipId_     = 0;
    uint32_t    nativeRate_ = 0;
};

} // namespace fitom
