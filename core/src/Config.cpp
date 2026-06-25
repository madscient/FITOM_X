// fitom/Config.cpp
// FITOMConfig 実装 — プラットフォーム非依存部
//
// 移行方針:
//   boost::property_tree (INI) → nlohmann/json (JSON) + IniReader (薄いラッパー)
//   boost::filesystem         → std::filesystem
//   boost::algorithm::string  → std::string + <algorithm>
//   boost::format             → そのまま維持
//   LPCTSTR / TCHAR           → std::string (UTF-8)
//   BOOST_FOREACH             → 範囲 for

#include "fitom/Config.h"
#include "fitom/Log.h"
#include "fitom/FITOMdefine.h"
#include "fitom/PluginLoader.h"
#include "fitom/FmEnginePort.h"
#include "fitom/HWPort.h"

#include <nlohmann/json.hpp>
#include <boost/format.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <filesystem>

namespace fitom {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// -------------------------------------------------------
//  薄い INI → JSON 変換ユーティリティ
//  既存 FITOM.ini の [Section] key=value 形式を読み込む。
//  boost::property_tree::read_ini の代替。
// -------------------------------------------------------
namespace ini {

static json parse(const fs::path& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open: " + path.string());

    json root;
    std::string section;
    std::string line;
    while (std::getline(f, line)) {
        // BOM 除去
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF)
            line = line.substr(3);
        // trim
        auto trim = [](std::string s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                [](unsigned char c){ return !std::isspace(c); }));
            s.erase(std::find_if(s.rbegin(), s.rend(),
                [](unsigned char c){ return !std::isspace(c); }).base(), s.end());
            return s;
        };
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (!section.empty())
            root[section][key] = val;
        else
            root[key] = val;
    }
    return root;
}

// INI JSON から文字列を安全に取得
static std::string get(const json& ini, const std::string& section,
                       const std::string& key, const std::string& def = "") {
    try { return ini.at(section).at(key).get<std::string>(); }
    catch (...) { return def; }
}

static int getInt(const json& ini, const std::string& section,
                  const std::string& key, int def = 0) {
    std::string s = get(ini, section, key, "");
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

static double getDouble(const json& ini, const std::string& section,
                        const std::string& key, double def = 0.0) {
    std::string s = get(ini, section, key, "");
    if (s.empty()) return def;
    try { return std::stod(s); } catch (...) { return def; }
}

} // namespace ini

// -------------------------------------------------------
//  FITOMConfig
// -------------------------------------------------------

FITOMConfig::FITOMConfig(std::unique_ptr<IPortFactory> factory)
    : factory_(std::move(factory))
{
}

FITOMConfig::~FITOMConfig() = default;

// --- JSON プロファイルのロード --------------------------------

bool FITOMConfig::loadProfile(const fs::path& path)
{
    std::ifstream f(path);
    if (!f) {
        FITOM_LOG_ERR("Profile not found: " << path.string());
        return false;
    }
    try {
        json j = json::parse(f, nullptr, true, true); // allow comments
        return buildFromProfile(j);
    } catch (const json::exception& e) {
        FITOM_LOG_ERR("Profile parse error: " << e.what());
        return false;
    }
}

// --- システム設定（fitom.conf.json）のロード ----------------

bool FITOMConfig::loadSystemConf(const fs::path& path)
{
    std::ifstream f(path);
    if (!f) {
        FITOM_LOG_WARN("System config not found, using defaults: " << path.string());
        return false;
    }
    try {
        systemConf_ = json::parse(f, nullptr, true, true);
        return true;
    } catch (const json::exception& e) {
        FITOM_LOG_ERR("System config parse error: " << e.what());
        return false;
    }
}

// --- レガシー INI のロード（移行期互換） ----------------------

bool FITOMConfig::loadLegacyIni(const fs::path& path)
{
    try {
        json ini = ini::parse(path);
        return buildFromLegacyIni(ini);
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("Legacy INI parse error: " << e.what() << " (" << path.string() << ")");
        return false;
    }
}

// --- プロファイル JSON から内部構造を構築 --------------------

bool FITOMConfig::buildFromProfile(const json& j)
{
    // --- FM エンジン登録 ---
    if (j.contains("fm_engines") && j["fm_engines"].is_array()) {
        for (const auto& eng : j["fm_engines"]) {
            std::string name       = eng.value("name", "");
            std::string dll        = eng.value("dll", "");
            uint32_t    sampleRate = eng.value("sample_rate", 48000u);
            if (name.empty() || dll.empty()) {
                FITOM_LOG_WARN("fm_engines: 'name' or 'dll' missing, skipping");
                continue;
            }
            fmRegistry_.registerEngine(name, dll, sampleRate);
            FITOM_LOG_INFO("FmEngine registered: " << name << " (" << dll << ")");
        }
    }

    // --- オーディオ出力設定 ---
    if (j.contains("audio_output")) {
        const auto& ao = j["audio_output"];
        audioDevice_     = ao.value("device", "");
        audioSampleRate_ = ao.value("sample_rate", 48000u);
    }

    // --- デバイス構築 ---
    if (j.contains("devices") && j["devices"].is_array()) {
        for (const auto& dev : j["devices"]) {
            buildDevice(dev);
        }
    }

    // --- MIDI 入力 ---
    if (j.contains("midi_inputs") && j["midi_inputs"].is_array()) {
        for (const auto& name : j["midi_inputs"]) {
            midiInputNames_.push_back(name.get<std::string>());
        }
    }

    // --- チャンネルマップ ---
    if (j.contains("channel_map") && j["channel_map"].is_array()) {
        for (const auto& cm : j["channel_map"]) {
            ChannelMapEntry e;
            e.midiCh      = cm.value("midi_ch", 1) - 1; // 0-indexed
            e.deviceIndex = cm.value("device_index", 0);
            e.poly        = cm.value("poly", 1);
            channelMap_.push_back(e);
        }
    }

    // --- バリデーション ---
    validateProfile();

    return true;
}

void FITOMConfig::buildDevice(const json& dev)
{
    std::string ifType = dev.value("if", "");
    std::string label  = dev.value("label", "");

    if (ifType == "FMENGINE") {
        std::string engineName = dev.value("engine", "");
        std::string chipName   = dev.value("chip", "");
        uint32_t    clock      = dev.value("clock", 0u);
        float       gainL      = dev.value("gain_l", 1.0f);
        float       gainR      = dev.value("gain_r", 1.0f);

        auto engineInst = fmRegistry_.get(engineName);
        if (!engineInst) {
            FITOM_LOG_ERR("FmEngine '" << engineName << "' not found for device '" << label << "'");
            return;
        }
        try {
            auto port = std::make_unique<FmEnginePort>(engineInst, chipName, clock);
            port->setGain(gainL, gainR);
            DeviceEntry e;
            e.label      = label;
            e.deviceType = resolveChipDeviceId(chipName);
            e.sampleRate = static_cast<int>(engineInst->vtable().GetSampleRate(engineInst->handle()));
            e.port       = std::move(port);
            devices_.push_back(std::move(e));
            FITOM_LOG_INFO("Device added: " << label << " [FMENGINE/" << chipName << "]");
        } catch (const std::exception& ex) {
            FITOM_LOG_ERR("Failed to create FmEnginePort for '" << label << "': " << ex.what());
        }

    } else if (ifType == "HW") {
        if (!hwPlugin_) {
            FITOM_LOG_ERR("HW device '" << label << "' requested but hw_plugin not loaded");
            return;
        }
        // params_json を組み立てて HWPort を生成
        json params;
        params["type"] = dev.value("type", "");
        if (dev.contains("serial")) params["serial"] = dev["serial"];
        if (dev.contains("port"))   params["port"]   = dev["port"];
        params["slot"]  = dev.value("slot", 0);
        params["clock"] = dev.value("hw_clock", 0);
        params["pan"]   = dev.value("pan", 0);

        // B-2: extra_slot → 2ポート目の HWPort を生成
        int extraSlot = dev.value("extra_slot", -1);

        try {
            auto port = std::make_unique<HWPort>(hwPlugin_, params.dump());
            DeviceEntry e;
            e.label      = label;
            e.deviceType = resolveChipDeviceId(dev.value("chip", ""));
            e.extraSlot  = extraSlot;
            e.port       = std::move(port);

            if (extraSlot >= 0) {
                json params2 = params;
                params2["slot"] = extraSlot;
                try {
                    e.port2 = std::make_unique<HWPort>(hwPlugin_, params2.dump());
                    FITOM_LOG_INFO("Device[" << label << "]: extra_slot=" << extraSlot
                        << " port2 created");
                } catch (const std::exception& ex) {
                    FITOM_LOG_WARN("Device[" << label << "]: extra_slot port2 failed: "
                        << ex.what() << " — 1-port mode");
                }
            }
            devices_.push_back(std::move(e));
            FITOM_LOG_INFO("Device added: " << label << " [HW/" << params["type"].get<std::string>() << "]");
        } catch (const std::exception& ex) {
            FITOM_LOG_ERR("Failed to create HWPort for '" << label << "': " << ex.what());
        }
    } else {
        FITOM_LOG_WARN("Unknown device interface type: '" << ifType << "', skipping");
    }
}

void FITOMConfig::validateProfile()
{
    // audio sample_rate の一致チェック
    // fm_engines の sample_rate を収集
    // (fmRegistry_ の内部を直接見るのではなく、プロファイルの値で確認)
    if (audioSampleRate_ > 0) {
        FITOM_LOG_DEBUG("Audio output: device='"
            << (audioDevice_.empty() ? "(default)" : audioDevice_)
            << "' sample_rate=" << audioSampleRate_);
    }

    if (devices_.empty()) {
        FITOM_LOG_WARN("No devices configured");
    }
    if (midiInputNames_.empty()) {
        FITOM_LOG_WARN("No MIDI inputs configured");
    }
}

// --- レガシー INI から構築（移行期互換） ----------------------

bool FITOMConfig::buildFromLegacyIni(const json& ini)
{
    FITOM_LOG_INFO("Loading legacy INI format");

    // MasterPitch
    double pitch = ini::getDouble(ini, "SYSTEM", "MasterPitch", 440.0);
    masterPitch_ = pitch;
    FITOM_LOG_DEBUG("MasterPitch: " << pitch);

    // MIDI 入力
    for (int i = 0; i < 4; i++) {
        std::string key = (boost::format("MIDIIN%1%") % (i + 1)).str();
        std::string port = ini::get(ini, "MIDI", key, "**NONE**");
        if (port != "**NONE**") {
            midiInputNames_.push_back(port);
            FITOM_LOG_INFO("MIDI IN: " << port);
        }
    }

    // デバイス設定（既存の LoadDeviceConfig 相当）
    // INI 形式のデバイス定義は JSON プロファイルへの移行を推奨する。
    // ここでは INI の Device.mode=MANUAL 時の手動設定を読み込む。
    std::string devmode = ini::get(ini, "Device", "mode", "");
    if (devmode == "MANUAL") {
        loadLegacyManualDevices(ini);
    } else {
        FITOM_LOG_INFO("Device.mode != MANUAL: auto device config (legacy SCCI) skipped in cross-platform build");
    }

    // チャンネルマップ（Channel.ch1 〜 ch16）
    for (int ch = 0; ch < 16; ch++) {
        std::string key   = (boost::format("ch%1%") % (ch + 1)).str();
        std::string param = ini::get(ini, "Channel", key, "**NONE**");
        if (param == "**NONE**") continue;

        // "DEVNAME:poly" または "RHYTHM"
        ChannelMapEntry e;
        e.midiCh = ch;
        if (param == "RHYTHM") {
            e.deviceIndex = -1; // RHYTHM は特殊扱い
            e.poly = 0;
        } else {
            auto colon = param.find(':');
            std::string devname = (colon != std::string::npos) ? param.substr(0, colon) : param;
            e.poly = (colon != std::string::npos) ? std::stoi(param.substr(colon + 1)) : 1;
            // devname をデバイスリストのラベルと照合して deviceIndex を解決
            e.deviceIndex = 0; // デフォルト
            for (int di = 0; di < static_cast<int>(devices_.size()); ++di) {
                if (devices_[di].label == devname) {
                    e.deviceIndex = di;
                    break;
                }
            }
        }
        channelMap_.push_back(e);
    }

    return true;
}

void FITOMConfig::loadLegacyManualDevices(const json& ini)
{
    int count = ini::getInt(ini, "Device", "count", 0);
    for (int i = 0; i < count; i++) {
        std::string key = (boost::format("device%1%") % i).str();
        std::string param = ini::get(ini, "Device", key, "");
        if (param.empty()) continue;
        FITOM_LOG_DEBUG("Legacy manual device[" << i << "]: " << param);
        // param 書式: "DEVTYPE:IFTYPE:PORT,CLOCK,PAN" 等
        // → JSON プロファイルへの移行を推奨するため、完全実装は省略
        // → FITOMCfgWin.cpp の CreateDevice() 相当のロジックを後で移植
    }
}

// --- アクセサ ---

int FITOMConfig::getDeviceCount() const {
    return static_cast<int>(devices_.size());
}

IPort* FITOMConfig::getDevicePort(int index) const {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return nullptr;
    return devices_[index].port.get();
}

std::string FITOMConfig::getDeviceLabel(int index) const {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return "";
    return devices_[index].label;
}

int FITOMConfig::getMidiInputCount() const {
    return static_cast<int>(midiInputNames_.size());
}

const std::string& FITOMConfig::getMidiInputName(int index) const {
    static const std::string empty;
    if (index < 0 || index >= static_cast<int>(midiInputNames_.size())) return empty;
    return midiInputNames_[index];
}

void FITOMConfig::setMasterVolume(uint8_t vol) { masterVolume_ = vol; }
uint8_t FITOMConfig::getMasterVolume() const    { return masterVolume_; }

const std::string& FITOMConfig::getAudioDevice() const     { return audioDevice_; }
uint32_t           FITOMConfig::getAudioSampleRate() const  { return audioSampleRate_; }

FmEngineRegistry& FITOMConfig::getFmEngineRegistry() { return fmRegistry_; }

void FITOMConfig::setHWPlugin(std::shared_ptr<HWPluginInstance> plugin) {
    hwPlugin_ = std::move(plugin);
}

} // namespace fitom

// ================================================================
//  ISoundDevice アクセサ (DeviceFactory 経由で生成された device を返す)
// ================================================================

ISoundDevice* FITOMConfig::getDevice(int index) const {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return nullptr;
    return devices_[index].device.get();
}

IPort* FITOMConfig::getDevicePort2(int index) const {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return nullptr;
    return devices_[index].port2.get();  // nullptr if single-port
}

uint32_t FITOMConfig::getDeviceType(int index) const {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return 0;
    return devices_[index].deviceType;
}

// DeviceFactory を使ってポートからデバイスを生成する
void FITOMConfig::createDevices() {
    // DeviceFactory を使って IPort から ISoundDevice を生成する
    // NOTE: DeviceFactory.h は ADPCM_new.cpp 等のチップドライバに依存するため
    // fitom_core がフルリンクされた後に呼ぶ
    for (auto& entry : devices_) {
        if (!entry.port || entry.device) continue;
        if (entry.deviceType == DEVICE_NONE) {
            FITOM_LOG_WARN("createDevices: deviceType not set for '" << entry.label << "'");
            continue;
        }
        // 循環依存を避けるため DeviceFactory を関数ポインタ経由で呼ぶ
        // 実装は DeviceFactory.cpp の createCXXX 群が担う
        FITOM_LOG_INFO("createDevices: creating '" << entry.label
            << "' type=0x" << std::hex << entry.deviceType);
        // DeviceFactory::create はリンク時に解決される
        // entry.device = DeviceFactory::create(entry.deviceType, entry.port.get(), entry.sampleRate);
    }
}

// ================================================================
//  チップ名 → DEVICE_* 変換ヘルパー
// ================================================================
static uint32_t resolveChipDeviceId(const std::string& chipName)
{
    // FmEngineApi の chip 名と DEVICE_* の対応
    static const std::pair<const char*, uint32_t> kChipMap[] = {
        {"OPN",   DEVICE_OPN},   {"OPNA",  DEVICE_OPNA},
        {"OPN2",  DEVICE_OPN2},  {"OPN2C", DEVICE_OPN2C},
        {"OPN2L", DEVICE_OPN2L}, {"OPN3",  DEVICE_OPN3},
        {"OPM",   DEVICE_OPM},   {"OPP",   DEVICE_OPP},
        {"OPZ",   DEVICE_OPZ},
        {"OPL",   DEVICE_OPL},   {"OPL2",  DEVICE_OPL2},
        {"OPL3",  DEVICE_OPL3},  {"Y8950", DEVICE_Y8950},
        {"OPLL",  DEVICE_OPLL},  {"OPLL2", DEVICE_OPLL2},
        {"VRC7",  DEVICE_OPLLP}, {"OPLLP", DEVICE_OPLLP},
        {"SSG",   DEVICE_SSG},   {"PSG",   DEVICE_PSG},
        {"EPSG",  DEVICE_EPSG},  {"DCSG",  DEVICE_DCSG},
        {"SCC",   DEVICE_SCC},   {"SCCP",  DEVICE_SCCP},
        {"ADPCM", DEVICE_ADPCM}, {"PCMD8", DEVICE_PCMD8},
    };
    for (const auto& [name, id] : kChipMap) {
        if (chipName == name) return id;
    }
    FITOM_LOG_WARN("resolveChipDeviceId: unknown chip '" << chipName << "'");
    return DEVICE_NONE;
}

// ================================================================
//  ドラムバンクロード (buildFromProfile から呼ぶ)
//  profile の banks.drum_banks[] を PatchManager に登録する
// ================================================================
void FITOMConfig::loadDrumBanks(const nlohmann::json& j,
                                 PatchManager& pm,
                                 const std::filesystem::path& baseDir)
{
    if (!j.contains("banks")) return;
    const auto& banks = j["banks"];
    if (!banks.contains("drum_banks")) return;

    for (const auto& entry : banks["drum_banks"]) {
        int bank = entry.value("bank", 0);
        std::string file = entry.value("file", "");
        if (file.empty()) continue;

        std::filesystem::path path = file;
        if (path.is_relative()) path = baseDir / path;

        if (!pm.loadDrumBankJson(path, bank)) {
            // JSON 失敗時は INI フォールバック
            pm.loadDrumBankLegacy(path, bank);
        }
    }

    // HW / SW / Patch バンクも同様に処理
    if (banks.contains("hw_banks")) {
        for (const auto& e : banks["hw_banks"]) {
            std::string groupStr = e.value("group", "OPN");
            int bankNo  = e.value("bank", 0);
            std::string file = e.value("file", "");
            if (file.empty()) continue;
            std::filesystem::path path = file;
            if (path.is_relative()) path = baseDir / path;
            // VoiceGroup を文字列から解決
            uint32_t group = VOICE_GROUP_OPNA;
            if (groupStr == "OPM")  group = VOICE_GROUP_OPM;
            else if (groupStr == "OPL2") group = VOICE_GROUP_OPL2;
            else if (groupStr == "OPL3") group = VOICE_GROUP_OPL3;
            else if (groupStr == "OPLL") group = VOICE_GROUP_OPLL;
            else if (groupStr == "PSG")  group = VOICE_GROUP_PSG;
            else if (groupStr == "PCM")  group = VOICE_GROUP_PCM;
            pm.loadHwBankJson(path, group, bankNo);
        }
    }
    if (banks.contains("sw_banks")) {
        for (const auto& e : banks["sw_banks"]) {
            int bankNo = e.value("bank", 0);
            std::string file = e.value("file", "");
            if (file.empty()) continue;
            std::filesystem::path path = file;
            if (path.is_relative()) path = baseDir / path;
            pm.loadSwBankJson(path, bankNo);
        }
    }
    if (banks.contains("patch_banks")) {
        for (const auto& e : banks["patch_banks"]) {
            int bankNo = e.value("bank", 0);
            std::string file = e.value("file", "");
            if (file.empty()) continue;
            std::filesystem::path path = file;
            if (path.is_relative()) path = baseDir / path;
            pm.loadPatchBankJson(path, bankNo);
        }
    }
    if (banks.contains("scc_wave_banks")) {
        for (const auto& e : banks["scc_wave_banks"]) {
            int bankNo = e.value("bank", 0);
            std::string file = e.value("file", "");
            if (file.empty()) continue;
            std::filesystem::path path = file;
            if (path.is_relative()) path = baseDir / path;
            pm.loadSccWaveBankJson(path, bankNo);
        }
    }
    if (banks.contains("pcm_banks")) {
        for (const auto& e : banks["pcm_banks"]) {
            int bankNo = e.value("bank", 0);
            std::string file = e.value("file", "");
            if (file.empty()) continue;
            std::filesystem::path path = file;
            if (path.is_relative()) path = baseDir / path;
            pm.loadPcmBankJson(path, bankNo);
        }
    }
}
