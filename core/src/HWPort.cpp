// fitom/HWPort.cpp
// hw::HWControllerBase → IPort アダプター実装（IHWPlugin DLL 経由）

#include "fitom/HWPort.h"
#include "fitom/Log.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace fitom {

// -------------------------------------------------------
//  HWPluginInstance
// -------------------------------------------------------

std::shared_ptr<HWPluginInstance> HWPluginInstance::load(
    const std::filesystem::path& dllPath)
{
    auto self = std::shared_ptr<HWPluginInstance>(new HWPluginInstance());
    self->loader_ = PluginLoader::load(dllPath);

    auto& l = self->loader_;

    self->GetName_   = l.symRequired<PFN_GetName   >("HWPlugin_GetName");
    self->Init_      = l.symRequired<PFN_Init      >("HWPlugin_Init");
    self->Enumerate_ = l.symRequired<PFN_Enumerate  >("HWPlugin_Enumerate");
    self->FreeString = l.symRequired<PFN_FreeString  >("HWPlugin_FreeString");
    self->Open       = l.symRequired<PFN_Open        >("HWPlugin_Open");
    self->Close      = l.symRequired<PFN_Close       >("HWPlugin_Close");
    self->Write      = l.symRequired<PFN_Write       >("HWPlugin_Write");
    self->WriteBlock = l.symRequired<PFN_WriteBlock  >("HWPlugin_WriteBlock");
    self->Reset      = l.symRequired<PFN_Reset       >("HWPlugin_Reset");
    self->GetClock   = l.symRequired<PFN_GetClock    >("HWPlugin_GetClock");
    self->GetPanpot  = l.symRequired<PFN_GetPanpot   >("HWPlugin_GetPanpot");
    self->IsOpen     = l.symRequired<PFN_IsOpen      >("HWPlugin_IsOpen");
    // オプション関数 (未実装のプラグインでは symOptional が nullptr を返す。
    // 呼び出し側は必ずnullptrチェックすること)。
    self->GetLatencySamples = l.symOptional<PFN_GetLatencySamples>("HWPlugin_GetLatencySamples");
    self->SetDelaySamples   = l.symOptional<PFN_SetDelaySamples  >("HWPlugin_SetDelaySamples");
    // HWPlugin_Shutdown (2026年7月新設): 未エクスポートのプラグインとの
    // 後方互換のためoptional。詳細はIHWPlugin.hの宣言コメント参照。
    self->Shutdown_          = l.symOptional<PFN_Shutdown        >("HWPlugin_Shutdown");

    FITOM_LOG_INFO("HWPlugin loaded: " << self->name()
        << " from " << dllPath.string());
    return self;
}

void HWPluginInstance::shutdown() const
{
    if (shutdownDone_) return; // 二重実行防止
    shutdownDone_ = true;
    if (!Shutdown_) return;    // 未エクスポート(旧プラグイン)なら何もしない
    FITOM_LOG_INFO("HWPlugin shutting down: " << name());
    Shutdown_();
}

std::string HWPluginInstance::name() const
{
    return GetName_ ? GetName_() : "(unknown)";
}

std::string HWPluginInstance::enumerate() const
{
    if (!Enumerate_) return "[]";
    const char* s = Enumerate_();
    if (!s) return "[]";
    std::string result(s);
    if (FreeString) FreeString(s);
    return result;
}

HWResult HWPluginInstance::init(const std::string& profilePath) const
{
    // Init_はsymRequiredでロードされているため、load()が成功していれば
    // 常に非nullptr (nullptrチェック不要)。
    return Init_(profilePath.empty() ? nullptr : profilePath.c_str());
}

// -------------------------------------------------------
//  HWPluginRegistry
// -------------------------------------------------------

void HWPluginRegistry::registerPlugin(const std::string& name,
                                       const std::filesystem::path& dllPath,
                                       const std::filesystem::path& profilePath)
{
    std::lock_guard<std::mutex> lk(mutex_);

    try {
        auto plugin = HWPluginInstance::load(dllPath);

        // HWPlugin_Init() は他の全関数(Enumerate/Open等)より前に必ず呼ぶ
        // (プラグイン側の契約: 初期化前の呼び出しは失敗を返す)。
        // profilePathが空ならnullptrを渡し、プラグイン自身のデフォルト
        // 探索ルール(DLLと同じディレクトリ等)に従わせる。
        // Init失敗時はこのプラグイン自体を登録しない
        // (Enumerate/Openを一切呼べない状態のプラグインを使い物として
        //  entries_に残すのは危険なため)。
        HWResult r = plugin->init(profilePath.string());
        if (r != HW_OK) {
            FITOM_LOG_ERR("HWPlugin '" << name << "': HWPlugin_Init failed (code="
                << static_cast<int>(r) << ", profile=" << profilePath.string() << ")");
            return;
        }

        entries_[name] = plugin;
        FITOM_LOG_INFO("HWPlugin registered: " << name << " (" << dllPath.string()
            << ", profile=" << (profilePath.empty() ? "(default)" : profilePath.string()) << ")");
    } catch (const std::exception& ex) {
        FITOM_LOG_ERR("Failed to load HW plugin '" << name << "' from "
            << dllPath.string() << ": " << ex.what());
    }
}

std::shared_ptr<HWPluginInstance> HWPluginRegistry::get(const std::string& name)
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = entries_.find(name);
    if (it == entries_.end()) {
        FITOM_LOG_ERR("HWPluginRegistry: plugin '" << name << "' not registered");
        return nullptr;
    }
    return it->second;
}

// -------------------------------------------------------
//  HWPort
// -------------------------------------------------------

HWPort::HWPort(std::shared_ptr<HWPluginInstance> plugin,
               const std::string& paramsJson)
    : plugin_(std::move(plugin))
    , paramsJson_(paramsJson)
{
    assert(plugin_);
    HWResult r = plugin_->Open(paramsJson.c_str(), &handle_);
    if (r != HW_OK || !handle_) {
        throw std::runtime_error(
            "HWPlugin_Open failed (error=" + std::to_string(r)
            + ") params=" + paramsJson);
    }
    FITOM_LOG_INFO("HWPort opened: " << paramsJson);
}

HWPort::~HWPort()
{
    if (handle_ && plugin_) {
        plugin_->Close(handle_);
        handle_ = nullptr;
    }
}

// ── レイテンシ同期 ───────────────────────────────────────────────────────────

uint32_t HWPort::getLatencySamples() const
{
    if (!handle_ || !plugin_ || !plugin_->GetLatencySamples)
        return 0;   // 実装なし → 即時デバイス(物理チップ等)とみなす
    return plugin_->GetLatencySamples(handle_);
}

void HWPort::setDelaySamples(uint32_t delay_samples)
{
    if (!handle_ || !plugin_ || !plugin_->SetDelaySamples)
        return;     // 実装なし → 何もしない (旧 DLL との互換)
    plugin_->SetDelaySamples(handle_, delay_samples);
}

// ─────────────────────────────────────────────────────────────────────────────

void HWPort::write(uint16_t addr, uint16_t data)
{
    plugin_->Write(handle_, addr, static_cast<uint8_t>(data));
    std::lock_guard<std::mutex> lk(shadowMutex_);
    shadowRegs_[addr] = static_cast<uint8_t>(data);
}

void HWPort::writeBurst(uint16_t startAddr, const uint8_t* data, std::size_t len)
{
    // a_high != 0 の場合は Write() の逐次呼び出しにフォールバック
    // (write()側でシャドウ更新も行われる)
    uint8_t a_high = static_cast<uint8_t>(startAddr >> 8);
    if (a_high != 0) {
        IPort::writeBurst(startAddr, data, len);
        return;
    }
    plugin_->WriteBlock(handle_, static_cast<uint8_t>(startAddr & 0xFF), data, len);
    std::lock_guard<std::mutex> lk(shadowMutex_);
    for (std::size_t i = 0; i < len; ++i) {
        std::size_t addr = static_cast<std::size_t>(startAddr & 0xFF) + i;
        if (addr < shadowRegs_.size()) shadowRegs_[addr] = data[i];
    }
}

uint8_t HWPort::status()
{
    return plugin_->IsOpen(handle_) ? 0 : 0xFF;
}

void HWPort::reset()
{
    plugin_->Reset(handle_, 10u);
    // HWリセット後、実チップのレジスタは既定値(多くは0)に戻るため、
    // シャドウ側もそれに合わせてクリアする。
    std::lock_guard<std::mutex> lk(shadowMutex_);
    shadowRegs_.fill(0);
}

std::string HWPort::getDesc()
{
    return "HW[" + plugin_->name() + "]";
}

std::string HWPort::getInterfaceDesc()
{
    return plugin_->name();
}

int HWPort::getClock()
{
    return plugin_->GetClock(handle_);
}

int HWPort::getPanpot()
{
    return plugin_->GetPanpot(handle_);
}

uint8_t HWPort::getShadowReg(uint16_t addr) const
{
    std::lock_guard<std::mutex> lk(shadowMutex_);
    return shadowRegs_[addr];
}

void HWPort::getShadowRegRange(uint16_t startAddr, uint8_t* out, size_t len) const
{
    std::lock_guard<std::mutex> lk(shadowMutex_);
    for (size_t i = 0; i < len; ++i) {
        uint32_t addr = static_cast<uint32_t>(startAddr) + static_cast<uint32_t>(i);
        out[i] = (addr < shadowRegs_.size()) ? shadowRegs_[addr] : 0;
    }
}

std::string HWPort::getPhysicalChipName() const
{
    try {
        auto j = nlohmann::json::parse(paramsJson_);
        std::string type = j.value("type", "");

        if (type == "FMHWIF") {
            // fitom_fmhwif: {type:"FMHWIF", engine, chip, index, ...}
            std::string engine = j.value("engine", "");
            std::string chip   = j.value("chip", "");
            std::string name = engine.empty() ? chip
                              : chip.empty()   ? engine
                                                : (engine + "/" + chip);
            int index = j.value("index", 0);
            if (index > 0) name += " #" + std::to_string(index);
            return name.empty() ? "FMHWIF" : name;
        }

        // 物理HW: {type:"RE1"/"RE4"/"SPFM_TOWER"/"SPFM_LIGHT", serial or port, slot, ...}
        std::string ident;
        if (j.contains("serial")) ident = j.value("serial", "");
        else if (j.contains("port")) ident = j.value("port", "");

        std::string name = type;
        if (!ident.empty()) name += " " + ident;
        if (j.contains("slot")) name += " slot" + std::to_string(j.value("slot", 0));
        return name.empty() ? "(unknown)" : name;
    } catch (const std::exception&) {
        return "(unknown)";
    }
}

} // namespace fitom
