#pragma once
// fitom/HWPort.h  (v2)
// HW I/F バックエンド DLL (IHWPlugin) → IPort アダプター
//
// ─── 変更点 (v1 からの差分) ──────────────────────────────────────────────────
//   hw::HWControllerBase を直接 include しない。
//   IHWPlugin.h の C API (HWHandle) 経由でのみ HW を操作する。
//   DLL は実行時ロード (PluginLoader 経由)。

#include "IPort.h"
#include "PluginLoader.h"
#include <fitom/IHWPlugin.h>
#include <memory>
#include <string>
#include <mutex>
#include <unordered_map>
#include <filesystem>

namespace fitom {

// -------------------------------------------------------
//  HWPluginInstance: HW バックエンド DLL の RAII ラッパー
// -------------------------------------------------------
class HWPluginInstance {
public:
    using PFN_GetName    = const char*  (FITOM_HWP_CALL*)();
    using PFN_Init       = HWResult     (FITOM_HWP_CALL*)(const char*);
    using PFN_Shutdown   = void         (FITOM_HWP_CALL*)();
    using PFN_Enumerate  = const char*  (FITOM_HWP_CALL*)();
    using PFN_FreeString = void         (FITOM_HWP_CALL*)(const char*);
    using PFN_Open       = HWResult     (FITOM_HWP_CALL*)(const char*, HWHandle*);
    using PFN_Close      = void         (FITOM_HWP_CALL*)(HWHandle);
    using PFN_Write      = HWResult     (FITOM_HWP_CALL*)(HWHandle, uint16_t, uint8_t);
    using PFN_WriteBlock = HWResult     (FITOM_HWP_CALL*)(HWHandle, uint8_t, const uint8_t*, size_t);
    using PFN_Reset      = HWResult     (FITOM_HWP_CALL*)(HWHandle, unsigned int);
    using PFN_GetClock          = int      (FITOM_HWP_CALL*)(HWHandle);
    using PFN_GetPanpot         = int      (FITOM_HWP_CALL*)(HWHandle);
    using PFN_IsOpen            = bool     (FITOM_HWP_CALL*)(HWHandle);
    using PFN_GetLatencySamples = uint32_t (FITOM_HWP_CALL*)(HWHandle);
    using PFN_SetDelaySamples   = void     (FITOM_HWP_CALL*)(HWHandle, uint32_t);

    static std::shared_ptr<HWPluginInstance> load(const std::filesystem::path& dllPath);

    std::string              name()       const;
    std::string              enumerate()  const;   // JSON 文字列
    // HWPlugin_Init() を呼ぶ。DLLロード後、Enumerate/Open等の他の関数を
    // 呼ぶ前に必ず呼ぶこと (プラグイン側は初期化前の呼び出しに対し失敗を
    // 返す契約になっている)。profile_pathが空文字の場合、プラグイン自身の
    // デフォルト探索ルールに従う。
    // 戻り値: HWPlugin_Init()の戻り値をそのまま返す
    //   (HW_OK=成功、HW_ERR_INVALID_ARG=プロファイル解析失敗、
    //    HW_ERR_OPEN_FAILED=デバイス/ストリーム起動失敗)。
    HWResult                  init(const std::string& profilePath) const;

    // HWPlugin_Shutdown() を呼ぶ(エクスポートされていないプラグインでは
    // 何もしない)。二重呼び出しを避けるため、内部で一度だけ実行する
    // ガードを持つ(HWPluginRegistry::closeAll()から呼ばれる想定だが、
    // 万一複数回呼ばれても安全)。
    void shutdown() const;

    PFN_FreeString  FreeString = nullptr;
    PFN_Open        Open       = nullptr;
    PFN_Close       Close      = nullptr;
    PFN_Write       Write      = nullptr;
    PFN_WriteBlock  WriteBlock = nullptr;
    PFN_Reset       Reset      = nullptr;
    PFN_GetClock          GetClock          = nullptr;
    PFN_GetPanpot         GetPanpot         = nullptr;
    PFN_IsOpen            IsOpen            = nullptr;
    PFN_GetLatencySamples GetLatencySamples = nullptr;   // optional: 0 if absent
    PFN_SetDelaySamples   SetDelaySamples   = nullptr;   // optional: no-op if absent

private:
    HWPluginInstance() = default;
    PluginLoader   loader_;
    PFN_GetName    GetName_     = nullptr;
    PFN_Init       Init_        = nullptr;
    PFN_Shutdown   Shutdown_    = nullptr;   // optional: 未エクスポートならnullptr
    PFN_Enumerate  Enumerate_   = nullptr;
    mutable bool   shutdownDone_ = false;
};

// -------------------------------------------------------
//  HWPluginRegistry: 複数の名前付き HWPluginInstance を管理する
//  (FmEngine直接ロード経路は廃止済み。HWPluginRegistryのみ残す)。
//
//  FitomEmuIF.dll (エミュレーター) も、SPFM等の物理ハードウェア用
//  プラグインも、同じ IHWPlugin C API を実装しているため、FITOM本体は
//  「エミュレータか実機か」を一切区別しない。プロファイルに複数の
//  hw_plugins[] を定義し、devices[] 側で名前を指定して使い分ける。
// -------------------------------------------------------
class HWPluginRegistry {
public:
    // DLLをロードした直後にHWPlugin_Init(profilePath)を呼ぶ。
    // FITOM_X自身はプロファイルの内容を一切解釈しない (エミュレータか
    // 実機かを区別しないという設計原則を保つ)。profilePathが空の場合は
    // nullptrを渡し、プラグイン自身のデフォルト探索ルールに従わせる。
    // Init失敗時はこのプラグインを登録しない (get()で取得できない)。
    void registerPlugin(const std::string& name, const std::filesystem::path& dllPath,
                         const std::filesystem::path& profilePath = {});

    std::shared_ptr<HWPluginInstance> get(const std::string& name);

    // 登録済みの全プラグインに対してshutdown()(HWPlugin_Shutdown、
    // エクスポートされていれば)を呼んだ上で、保持しているshared_ptrを
    // 解放する。呼び出し元(HWPluginInstanceを他に保持していないことが
    // 前提)は、これによりプラグインが実際にアンロードされる
    // (参照カウントが0になった時点で~PluginLoader()がFreeLibrary/
    // dlcloseを呼ぶ)。
    // CFITOM::exit()等、明示的なシャットダウンシーケンスの中で、
    // プロセスの静的破棄が始まるより前に呼ぶことを想定している
    // (詳細はHWPlugin_Shutdownの宣言コメント参照)。
    void closeAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, inst] : entries_) {
            if (inst) inst->shutdown();
        }
        entries_.clear();
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<HWPluginInstance>> entries_;
};

// -------------------------------------------------------
//  HWPort: IPort 実装
// -------------------------------------------------------
class HWPort final : public IPort {
public:
    // params_json: IHWPlugin.h に記載のフォーマット
    HWPort(std::shared_ptr<HWPluginInstance> plugin,
           const std::string& paramsJson);
    ~HWPort() override;

    void    write(uint16_t addr, uint16_t data)    override;
    void    writeRaw(uint16_t addr, uint16_t data) override { write(addr, data); }
    void    writeBurst(uint16_t startAddr, const uint8_t* data, std::size_t len) override;
    uint8_t read(uint16_t)  override { return 0; }
    uint8_t status()        override;
    void    reset()         override;

    std::string getDesc()          override;
    std::string getInterfaceDesc() override;
    int         getClock()         override;
    int         getPanpot()        override;

    // レイテンシ同期 (IHWPlugin.h の任意関数; DLL が実装しない場合は0/no-op)
    uint32_t getLatencySamples() const;
    void     setDelaySamples(uint32_t delay_samples);

private:
    std::shared_ptr<HWPluginInstance> plugin_;
    HWHandle handle_ = nullptr;
};

} // namespace fitom
