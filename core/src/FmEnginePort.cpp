// fitom/FmEnginePort.cpp
// FmEngineApi DLL アダプター実装

#include "fitom/FmEnginePort.h"
#include "fitom/Log.h"
#include <stdexcept>
#include <cassert>
#include <algorithm>

namespace fitom {

// -------------------------------------------------------
//  FmEngineInstance
// -------------------------------------------------------

std::shared_ptr<FmEngineInstance> FmEngineInstance::create(
    PluginLoader loader, uint32_t sampleRate)
{
    auto self = std::shared_ptr<FmEngineInstance>(new FmEngineInstance());
    self->loader_ = std::move(loader);

    auto& v = self->vtable_;
    auto& l = self->loader_;

    // 必須シンボルを一括ロード
    v.Create           = l.symRequired<PFN_FmEngine_Create          >("FmEngine_Create");
    v.Destroy          = l.symRequired<PFN_FmEngine_Destroy         >("FmEngine_Destroy");
    v.Inquiry          = l.symRequired<PFN_FmEngine_Inquiry         >("FmEngine_Inquiry");
    v.GetSupportedChip = l.symRequired<PFN_FmEngine_GetSupportedChip>("FmEngine_GetSupportedChip");
    v.AddChip          = l.symRequired<PFN_FmEngine_AddChip         >("FmEngine_AddChip");
    v.GetChipName      = l.symRequired<PFN_FmEngine_GetChipName     >("FmEngine_GetChipName");
    v.GetNativeRate    = l.symRequired<PFN_FmEngine_GetNativeRate   >("FmEngine_GetNativeRate");
    v.GetSampleRate    = l.symRequired<PFN_FmEngine_GetSampleRate   >("FmEngine_GetSampleRate");
    v.Write            = l.symRequired<PFN_FmEngine_Write           >("FmEngine_Write");
    v.SetGain          = l.symRequired<PFN_FmEngine_SetGain         >("FmEngine_SetGain");
    v.GetGain          = l.symRequired<PFN_FmEngine_GetGain         >("FmEngine_GetGain");
    v.SetMemory        = l.symRequired<PFN_FmEngine_SetMemory       >("FmEngine_SetMemory");
    v.GetMemorySize    = l.symRequired<PFN_FmEngine_GetMemorySize   >("FmEngine_GetMemorySize");
    v.Generate         = l.symRequired<PFN_FmEngine_Generate        >("FmEngine_Generate");

    // エンジン名取得（FITOM 拡張）
    auto getName = l.sym<const char*(*)()>("FmEngine_GetEngineName");
    if (getName) {
        self->name_ = getName();
    } else {
        // フォールバック: DLL ファイル名を識別子として使用
        self->name_ = l.path().stem().string();
        FITOM_LOG_WARN("FmEngine_GetEngineName not found in " << l.path().string()
            << "; using filename: " << self->name_);
    }

    // エンジンインスタンス生成
    self->handle_ = v.Create(sampleRate);
    if (!self->handle_) {
        throw std::runtime_error("FmEngine_Create returned null for " + self->name_);
    }

    FITOM_LOG_INFO("FmEngine loaded: " << self->name_
        << " (sample_rate=" << sampleRate << ")");
    return self;
}

FmEngineInstance::~FmEngineInstance()
{
    if (handle_ && vtable_.Destroy) {
        vtable_.Destroy(handle_);
        handle_ = nullptr;
    }
    // loader_ のデストラクタが DLL をアンロードする
    FITOM_LOG_DEBUG("FmEngine unloaded: " << name_);
}

std::vector<std::string> FmEngineInstance::supportedChips() const
{
    std::vector<std::string> result;
    if (!handle_) return result;
    uint32_t n = vtable_.Inquiry(handle_);
    result.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (const char* name = vtable_.GetSupportedChip(handle_, i))
            result.emplace_back(name);
    }
    return result;
}

// -------------------------------------------------------
//  FmEngineRegistry
// -------------------------------------------------------

void FmEngineRegistry::registerEngine(const std::string& name,
                                      const std::string& dllPath,
                                      uint32_t sampleRate)
{
    std::lock_guard<std::mutex> lk(mutex_);
    Entry e;
    e.dllPath    = dllPath;
    e.sampleRate = sampleRate;
    e.instance   = nullptr; // 遅延ロード
    entries_[name] = std::move(e);
}

std::shared_ptr<FmEngineInstance> FmEngineRegistry::get(const std::string& name)
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = entries_.find(name);
    if (it == entries_.end()) {
        FITOM_LOG_ERR("FmEngineRegistry: engine '" << name << "' not registered");
        return nullptr;
    }

    // 初回アクセス時にロード
    if (!it->second.instance) {
        try {
            auto loader = PluginLoader::load(it->second.dllPath);
            it->second.instance = FmEngineInstance::create(
                std::move(loader), it->second.sampleRate);

            // エンジン名の一致を検証
            if (it->second.instance->name() != name) {
                FITOM_LOG_ERR("FmEngine name mismatch: config='" << name
                    << "' dll='" << it->second.instance->name() << "'");
                it->second.instance = nullptr;
                return nullptr;
            }
        } catch (const std::exception& e) {
            FITOM_LOG_ERR("Failed to load FmEngine '" << name << "': " << e.what());
            return nullptr;
        }
    }
    return it->second.instance;
}

void FmEngineRegistry::generateAll(float* outL, float* outR, uint32_t samples)
{
    // ゼロ初期化
    std::fill(outL, outL + samples, 0.0f);
    std::fill(outR, outR + samples, 0.0f);

    // 各エンジンの出力を加算（ミックス）
    // 注: mutex を取らない。オーディオコールバックスレッドからの呼び出しで
    //     登録・削除は行わないという前提。
    thread_local std::vector<float> tmpL, tmpR;
    tmpL.resize(samples);
    tmpR.resize(samples);

    for (auto& [name, entry] : entries_) {
        if (!entry.instance) continue;
        auto& v = entry.instance->vtable();
        auto  h = entry.instance->handle();

        std::fill(tmpL.begin(), tmpL.end(), 0.0f);
        std::fill(tmpR.begin(), tmpR.end(), 0.0f);

        v.Generate(h, tmpL.data(), tmpR.data(), samples);

        for (uint32_t i = 0; i < samples; ++i) {
            outL[i] += tmpL[i];
            outR[i] += tmpR[i];
        }
    }
}

// -------------------------------------------------------
//  FmEnginePort
// -------------------------------------------------------

FmEnginePort::FmEnginePort(std::shared_ptr<FmEngineInstance> engine,
                           const std::string& chipName,
                           uint32_t clock)
    : engine_(std::move(engine)), chipName_(chipName)
{
    assert(engine_);
    FmResult r = engine_->vtable().AddChip(
        engine_->handle(), chipName_.c_str(), clock, &chipId_);
    if (r != FM_OK) {
        throw std::runtime_error(
            "FmEngine_AddChip failed for '" + chipName_
            + "' in engine '" + engine_->name()
            + "' (error=" + std::to_string(r) + ")");
    }
    nativeRate_ = engine_->vtable().GetNativeRate(engine_->handle(), chipId_);
    FITOM_LOG_DEBUG("FmEnginePort: chip=" << chipName_
        << " id=" << chipId_ << " nativeRate=" << nativeRate_);
}

void FmEnginePort::write(uint16_t addr, uint16_t data)
{
    // IPort::write(addr, data) のアドレス規則:
    //   addr 上位 8bit → port (OPN 系の port0/1 等)
    //   addr 下位 8bit → reg (レジスタアドレス)
    uint32_t port = (addr >> 8) & 0xFF;
    uint8_t  reg  = addr & 0xFF;
    engine_->vtable().Write(engine_->handle(), chipId_,
                            reg, static_cast<uint8_t>(data), port);
}

void FmEnginePort::setGain(float l, float r)
{
    engine_->vtable().SetGain(engine_->handle(), chipId_, l, r);
}

void FmEnginePort::setMemory(FmMemoryType type, const uint8_t* data, uint32_t size)
{
    engine_->vtable().SetMemory(engine_->handle(), chipId_, type, data, size);
}

} // namespace fitom
