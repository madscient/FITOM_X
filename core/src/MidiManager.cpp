// fitom/MidiManager.cpp
// MIDI バックエンド DLL ローダーと管理

#include "fitom/MidiManager.h"
#include "fitom/Log.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace fitom {

// -------------------------------------------------------
//  MidiPluginInstance
// -------------------------------------------------------

std::shared_ptr<MidiPluginInstance> MidiPluginInstance::load(
    const std::filesystem::path& dllPath)
{
    auto self = std::shared_ptr<MidiPluginInstance>(new MidiPluginInstance());
    self->loader_ = PluginLoader::load(dllPath);
    auto& l = self->loader_;

    self->GetName_       = l.symRequired<PFN_GetName      >("MidiPlugin_GetName");
    self->EnumerateIn_   = l.symRequired<PFN_EnumerateIn  >("MidiPlugin_EnumerateIn");
    self->EnumerateOut_  = l.symRequired<PFN_EnumerateOut >("MidiPlugin_EnumerateOut");
    self->FreeString     = l.symRequired<PFN_FreeString    >("MidiPlugin_FreeString");
    self->OpenIn         = l.symRequired<PFN_OpenIn        >("MidiPlugin_OpenIn");
    self->CloseIn        = l.symRequired<PFN_CloseIn       >("MidiPlugin_CloseIn");
    self->OpenOut        = l.symRequired<PFN_OpenOut       >("MidiPlugin_OpenOut");
    self->CloseOut       = l.symRequired<PFN_CloseOut      >("MidiPlugin_CloseOut");
    self->Send           = l.symRequired<PFN_Send          >("MidiPlugin_Send");

    FITOM_LOG_INFO("MidiPlugin loaded: " << self->name()
        << " from " << dllPath.string());
    return self;
}

std::string MidiPluginInstance::name() const
{
    return GetName_ ? GetName_() : "(unknown)";
}

static std::vector<std::string> parseJsonArray(const char* s, decltype(&MidiPlugin_FreeString) freeStr)
{
    std::vector<std::string> result;
    if (!s) return result;
    try {
        auto j = nlohmann::json::parse(s);
        if (j.is_array()) {
            for (const auto& item : j)
                result.push_back(item.get<std::string>());
        }
    } catch (...) {}
    if (freeStr) freeStr(s);
    return result;
}

std::vector<std::string> MidiPluginInstance::enumerateIn() const
{
    const char* s = EnumerateIn_ ? EnumerateIn_() : nullptr;
    return parseJsonArray(s, FreeString);
}

std::vector<std::string> MidiPluginInstance::enumerateOut() const
{
    const char* s = EnumerateOut_ ? EnumerateOut_() : nullptr;
    return parseJsonArray(s, FreeString);
}

// -------------------------------------------------------
//  MidiInPort
// -------------------------------------------------------

MidiInPort::MidiInPort(std::shared_ptr<MidiPluginInstance> plugin,
                       const std::string& deviceName,
                       Callback callback)
    : plugin_(std::move(plugin))
    , deviceName_(deviceName)
    , callback_(std::move(callback))
{
    MidiResult r = plugin_->OpenIn(
        deviceName_.c_str(),
        &MidiInPort::rawCallback,
        this,
        &handle_);

    if (r != MIDI_OK) {
        std::string reason;
        switch (r) {
        case MIDI_ERR_NOT_FOUND:   reason = "device not found";        break;
        case MIDI_ERR_OPEN_FAILED: reason = "open failed";             break;
        case MIDI_ERR_UNAVAILABLE: reason = "plugin unavailable";      break;
        default:                   reason = "error " + std::to_string(r); break;
        }
        throw std::runtime_error(
            "MidiPlugin_OpenIn(\"" + deviceName_ + "\"): " + reason);
    }
    FITOM_LOG_INFO("MIDI In opened: " << deviceName_);
}

MidiInPort::~MidiInPort()
{
    if (handle_ && plugin_) {
        plugin_->CloseIn(handle_);
        handle_ = nullptr;
    }
    FITOM_LOG_DEBUG("MIDI In closed: " << deviceName_);
}

void FITOM_MIDIP_CALL MidiInPort::rawCallback(
    const uint8_t* data, size_t len,
    uint64_t timestamp_ns, void* user_data)
{
    auto* self = static_cast<MidiInPort*>(user_data);
    if (self->callback_) {
        self->callback_(data, len, timestamp_ns);
    }
}

// -------------------------------------------------------
//  MidiManager
// -------------------------------------------------------

bool MidiManager::loadPlugin(const std::filesystem::path& dllPath)
{
    try {
        plugin_ = MidiPluginInstance::load(dllPath);
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("Failed to load MIDI plugin: " << e.what());
        return false;
    }
}

std::vector<std::string> MidiManager::enumerateIn() const
{
    return plugin_ ? plugin_->enumerateIn() : std::vector<std::string>{};
}

std::vector<std::string> MidiManager::enumerateOut() const
{
    return plugin_ ? plugin_->enumerateOut() : std::vector<std::string>{};
}

bool MidiManager::openIn(const std::string& deviceName,
                         MidiInPort::Callback callback)
{
    if (!plugin_) {
        FITOM_LOG_WARN("MIDI plugin not loaded, cannot open: " << deviceName);
        return false;
    }
    try {
        auto port = std::make_unique<MidiInPort>(plugin_, deviceName, std::move(callback));
        openPorts_.push_back(std::move(port));
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_WARN("MIDI In open failed for '" << deviceName << "': " << e.what()
            << " — continuing without this device");
        return false;
    }
}

void MidiManager::closeAll()
{
    openPorts_.clear();
}

std::string MidiManager::pluginName() const
{
    return plugin_ ? plugin_->name() : "(none)";
}

} // namespace fitom
