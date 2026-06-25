// fitom/HWPort.cpp
// hw::HWControllerBase → IPort アダプター実装（IHWPlugin DLL 経由）

#include "fitom/HWPort.h"
#include "fitom/Log.h"
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

    FITOM_LOG_INFO("HWPlugin loaded: " << self->name()
        << " from " << dllPath.string());
    return self;
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

// -------------------------------------------------------
//  HWPort
// -------------------------------------------------------

HWPort::HWPort(std::shared_ptr<HWPluginInstance> plugin,
               const std::string& paramsJson)
    : plugin_(std::move(plugin))
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

void HWPort::write(uint16_t addr, uint16_t data)
{
    plugin_->Write(handle_, addr, static_cast<uint8_t>(data));
}

void HWPort::writeBurst(uint16_t startAddr, const uint8_t* data, std::size_t len)
{
    // a_high != 0 の場合は Write() の逐次呼び出しにフォールバック
    uint8_t a_high = static_cast<uint8_t>(startAddr >> 8);
    if (a_high != 0) {
        IPort::writeBurst(startAddr, data, len);
        return;
    }
    plugin_->WriteBlock(handle_, static_cast<uint8_t>(startAddr & 0xFF), data, len);
}

uint8_t HWPort::status()
{
    return plugin_->IsOpen(handle_) ? 0 : 0xFF;
}

void HWPort::reset()
{
    plugin_->Reset(handle_, 10u);
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

} // namespace fitom
