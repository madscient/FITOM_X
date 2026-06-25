#pragma once
// fitom/FmEnginePort.h  (v2)
// FmEngineApi 互換 DLL → IPort アダプター
//
// ─── 複数 DLL の同時使用 ────────────────────────────────────────────────────
//   FmEngineRegistry がエンジン名 → PluginLoader のマップを管理する。
//   config.json に記述した全エントリを初回アクセス時に一括ロードする。
//
//   {
//     "fm_engines": [
//       { "name": "YMEngine",  "dll": "YMEngine.dll",  "sample_rate": 48000 },
//       { "name": "AYEngine",  "dll": "AYEngine.dll",  "sample_rate": 48000 }
//     ],
//     "devices": [
//       { "if": "FMENGINE", "engine": "YMEngine", "chip": "OPNA", "clock": 0 },
//       { "if": "FMENGINE", "engine": "AYEngine", "chip": "SSG",  "clock": 0 }
//     ]
//   }

#include "IPort.h"
#include "PluginLoader.h"
#include <fitom/IFmEnginePlugin.h>
#include <FmEngineApi.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace fitom {

// -------------------------------------------------------
//  FmEngineHandle の RAII ラッパー + 関数ポインタ一式
// -------------------------------------------------------
struct FmEngineVtable {
    PFN_FmEngine_Create           Create           = nullptr;
    PFN_FmEngine_Destroy          Destroy          = nullptr;
    PFN_FmEngine_Inquiry          Inquiry          = nullptr;
    PFN_FmEngine_GetSupportedChip GetSupportedChip = nullptr;
    PFN_FmEngine_AddChip          AddChip          = nullptr;
    PFN_FmEngine_GetChipName      GetChipName      = nullptr;
    PFN_FmEngine_GetNativeRate    GetNativeRate    = nullptr;
    PFN_FmEngine_GetSampleRate    GetSampleRate    = nullptr;
    PFN_FmEngine_Write            Write            = nullptr;
    PFN_FmEngine_SetGain          SetGain          = nullptr;
    PFN_FmEngine_GetGain          GetGain          = nullptr;
    PFN_FmEngine_SetMemory        SetMemory        = nullptr;
    PFN_FmEngine_GetMemorySize    GetMemorySize    = nullptr;
    PFN_FmEngine_Generate         Generate         = nullptr;
};

class FmEngineInstance {
public:
    static std::shared_ptr<FmEngineInstance> create(
        PluginLoader loader, uint32_t sampleRate);

    ~FmEngineInstance();

    FmEngineHandle      handle() const { return handle_; }
    const FmEngineVtable& vtable() const { return vtable_; }
    std::string         name()   const { return name_; }

    std::vector<std::string> supportedChips() const;

private:
    FmEngineInstance() = default;
    PluginLoader   loader_;
    FmEngineVtable vtable_;
    FmEngineHandle handle_ = nullptr;
    std::string    name_;
};

// -------------------------------------------------------
//  FmEngineRegistry: エンジン名 → FmEngineInstance マップ
//  config から一括登録し、FmEnginePort 生成時に共有する
// -------------------------------------------------------
class FmEngineRegistry {
public:
    struct Entry {
        std::string            dllPath;
        uint32_t               sampleRate;
        std::shared_ptr<FmEngineInstance> instance; // 遅延ロード
    };

    // config.json の fm_engines 配列から登録
    void registerEngine(const std::string& name,
                        const std::string& dllPath,
                        uint32_t sampleRate = 48000);

    // エンジン名で取得 (初回アクセス時にロード)
    std::shared_ptr<FmEngineInstance> get(const std::string& name);

    // 全エンジンの波形生成 (オーディオコールバックから呼ぶ)
    void generateAll(float* outL, float* outR, uint32_t samples);

private:
    std::mutex                          mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

// -------------------------------------------------------
//  FmEnginePort: IPort 実装
// -------------------------------------------------------
class FmEnginePort final : public IPort {
public:
    // engineName: FmEngineRegistry に登録済みのエンジン名
    // chipName  : "OPNA" / "OPL2" 等
    // clock     : 0 でチップ標準クロック
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

    uint32_t    chipId()           const { return chipId_; }
    void        setGain(float l, float r);
    void        setMemory(FmMemoryType type, const uint8_t* data, uint32_t size);

private:
    std::shared_ptr<FmEngineInstance> engine_;
    std::string chipName_;
    uint32_t    chipId_     = 0;
    uint32_t    nativeRate_ = 0;
};

} // namespace fitom
