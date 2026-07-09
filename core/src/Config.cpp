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
#include "fitom/PatchManager.h"
#include "fitom/Log.h"
#include "fitom/FITOMdefine.h"
#include "fitom/PluginLoader.h"
#include "fitom/HWPort.h"
#include "fitom/DeviceFactory.h"

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

// resolveChipDeviceId は本ファイル後方の static 関数で定義されるが、
// buildDevice() から先に使われるため前方宣言する。
static uint32_t resolveChipDeviceId(const std::string& chipName);

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

bool FITOMConfig::loadProfile(const fs::path& path, PatchManager* patchMgr)
{
    std::ifstream f(path);
    if (!f) {
        FITOM_LOG_ERR("Profile not found: " << path.string());
        return false;
    }
    try {
        json j = json::parse(f, nullptr, true, true); // allow comments
        // バンクファイルパスの解決基点は「プロファイルファイル自身の
        // 親ディレクトリ」ではなく、カレントワーキングディレクトリとする。
        // プロファイル内の "file" (例: "banks/OPN/gm/xxx.hwbank.json") は
        // banks/ が config/ の兄弟ディレクトリ (= プロジェクトルート直下)
        // にある前提で書かれており、config/profiles/ を基点にすると
        // 誤ったパスになってしまう。空パスを渡すことで
        // baseDir/file が実質 file (CWD相対) のまま解決されるようにする。
        return buildFromProfile(j, patchMgr, std::filesystem::path{});
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

bool FITOMConfig::buildFromProfile(const json& j, PatchManager* patchMgr,
                                    const std::filesystem::path& baseDir)
{
    // --- HW プラグイン登録 (実機/エミュレータ問わず、IHWPluginを実装する
    //     DLLを複数登録できる。devices[]側で名前を指定して使い分ける) ---
    if (j.contains("hw_plugins") && j["hw_plugins"].is_array()) {
        for (const auto& p : j["hw_plugins"]) {
            std::string name = p.value("name", "");
            std::string dll  = p.value("dll", "");
            if (name.empty() || dll.empty()) {
                FITOM_LOG_WARN("hw_plugins: 'name' or 'dll' missing, skipping");
                continue;
            }
            // profile: HWPlugin_Init()に渡すプラグイン固有の設定ファイル
            // パス。省略時はプラグイン自身のデフォルト探索ルールに従う。
            // FITOM_X自身はこのプロファイルの内容を一切解釈しない
            // (エミュレータか実機かを区別しないという設計原則を保つ)。
            std::string profile = p.value("profile", "");
            hwPluginRegistry_.registerPlugin(name, dll, profile);

            // auto_devices: HWPlugin_Enumerate() が返すJSON配列を、そのまま
            // devices[] エントリとして使う (params_jsonとして直接転送可能な
            // 形式である前提)。FitomEmuIF等、自身のプロファイルで既に
            // chip構成が確定しているプラグイン向け。
            // 注意: 実機ハードウェアの列挙は通常 type/serial/port/index
            // のみで chip 情報を含まない(挿入チップは自動検出できない)ため、
            // この機能は主にエミュレーター統合プラグイン向け。
            if (p.value("auto_devices", false)) {
                auto plugin = hwPluginRegistry_.get(name);
                if (plugin) {
                    try {
                        json arr = json::parse(plugin->enumerate());
                        if (arr.is_array()) {
                            int autoIdx = 0;
                            for (const auto& entry : arr) {
                                json devJson = entry;
                                devJson["if"]     = "HW";
                                devJson["plugin"] = name;
                                if (!devJson.contains("label")) {
                                    std::string base = devJson.value("chip",
                                        devJson.value("type", name));
                                    devJson["label"] = base + "#" + std::to_string(autoIdx);
                                }
                                buildDevice(devJson);
                                ++autoIdx;
                            }
                            FITOM_LOG_INFO("HWPlugin '" << name << "': auto-discovered "
                                << arr.size() << " device(s) via HWPlugin_Enumerate()");
                        } else {
                            FITOM_LOG_WARN("HWPlugin '" << name
                                << "': auto_devices requested but Enumerate() did not "
                                   "return a JSON array, skipping");
                        }
                    } catch (const std::exception& ex) {
                        FITOM_LOG_WARN("HWPlugin '" << name
                            << "': auto_devices enumerate parse failed: " << ex.what());
                    }
                }
            }
        }
    }

    // --- デバイス構築 ---
    if (j.contains("devices") && j["devices"].is_array()) {
        for (const auto& dev : j["devices"]) {
            buildDevice(dev);
        }
    }

    // --- リニアステレオ化 (CLinearPanDevice、明示指定のみ) ---
    // 同種デバイス自動束ねより前に評価する。
    mergeStereoPairDevices();

    // --- 同種デバイス自動束ね (CSpanDevice) ---
    // sub-device 展開・ステレオ化が完了した devices_ 全体を対象に、
    // 同一 VoicePatchType・同一物理接続種別のエントリを検出して統合する。
    mergeSpannableDevices();

    // --- MIDI 入力 ---
    if (j.contains("midi_inputs") && j["midi_inputs"].is_array()) {
        for (const auto& name : j["midi_inputs"]) {
            midiInputNames_.push_back(name.get<std::string>());
        }
    }
    if (j.contains("midi_backend") && j["midi_backend"].is_object()) {
        midiBackendDll_ = j["midi_backend"].value("dll", "");
    }

    // --- 音色バンク一式のロード (hw/sw/patch/drum/scc_wave/pcm) ---
    // patchMgr が渡された場合のみ実行する (省略時は従来通りデバイス構成
    // のみを構築し、音色バンクロードはスキップする、後方互換動作)。
    // (以前はここでbanksセクションが一切処理されておらず、プロファイルの
    //  hw_banks/sw_banks/patch_banks/drum_banks等が実際には一度もロード
    //  されないという重大な不具合があったため修正)
    if (patchMgr) {
        loadDrumBanks(j, *patchMgr, baseDir);
    }

    // --- バリデーション ---
    validateProfile();

    return true;
}

void FITOMConfig::buildDevice(const json& dev)
{
    std::string ifType = dev.value("if", "");
    std::string label  = dev.value("label", "");

    if (ifType == "HW") {
        std::string pluginName = dev.value("plugin", "");
        auto plugin = hwPluginRegistry_.get(pluginName);
        if (!plugin) {
            FITOM_LOG_ERR("HW device '" << label << "' requested but plugin '"
                << pluginName << "' not registered/loaded");
            return;
        }
        // params_json はプラグインごとに異なる (FitomEmuIFなら
        // {type,engine,chip,index,pan}、物理HWなら{type,serial,port,slot,...}
        // 等)。FITOM本体はエミュレータか実機かを一切区別しないため、
        // deviceエントリのJSONから制御用フィールド(if/label/plugin/
        // extra_slot/rhythm_mode/stereo_pair)を除いた残り全てを、
        // そのままプラグインに転送する。
        json params = dev;
        params.erase("if");
        params.erase("label");
        params.erase("plugin");
        params.erase("extra_slot");
        params.erase("rhythm_mode");
        params.erase("stereo_pair");

        // B-2: extra_slot → 2ポート目の HWPort を生成 (2ポート目は
        // 元のparamsの"slot"(チップスロット番号)をextra_slotの値で
        // 上書きして開く。FitomHwIFのparams_json仕様上、"slot"がチップの
        // スロット番号("index"はRE1/RE4のUSB接続順という別の意味なので
        // 混同しないよう注意)。
        int extraSlot = dev.value("extra_slot", -1);

        try {
            auto port = std::make_shared<HWPort>(plugin, params.dump());
            std::shared_ptr<IPort> port2;
            if (extraSlot >= 0) {
                json params2 = params;
                params2["slot"] = extraSlot;
                try {
                    port2 = std::make_shared<HWPort>(plugin, params2.dump());
                    FITOM_LOG_INFO("Device[" << label << "]: extra_slot=" << extraSlot
                        << " port2 created");
                } catch (const std::exception& ex) {
                    FITOM_LOG_WARN("Device[" << label << "]: extra_slot port2 failed: "
                        << ex.what() << " — 1-port mode");
                }
            }
            uint32_t deviceType = resolveChipDeviceId(dev.value("chip", ""));
            bool rhythmMode = dev.value("rhythm_mode", false);
            bool stereoPair = dev.value("stereo_pair", false);
            pushDeviceEntries(label, deviceType, port, port2, 44100, extraSlot, rhythmMode, stereoPair);
        } catch (const std::exception& ex) {
            FITOM_LOG_ERR("Failed to create HWPort for '" << label << "': " << ex.what());
        }
    } else {
        FITOM_LOG_WARN("Unknown device interface type: '" << ifType << "', skipping");
    }
}

void FITOMConfig::validateProfile()
{
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

    // チャンネルマップ (Channel.ch1〜ch16) は廃止。
    // MIDI ch10(0-indexed:ch9)は固定でリズムチャンネル(GM準拠)、
    // ポリフォニー数はProgChange時に解決されたデバイスのチャンネル数から
    // 動的に決定されるため、レガシーINIのこのセクションは読み捨てる。

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

HWPluginRegistry& FITOMConfig::getHWPluginRegistry() { return hwPluginRegistry_; }

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

int FITOMConfig::getDeviceSampleRate(int index) const {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return 44100;
    return devices_[index].sampleRate;
}

bool FITOMConfig::getDeviceRhythmMode(int index) const {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return false;
    return devices_[index].rhythmMode;
}

// ================================================================
//  VoicePatchType (音色パッチ互換性分類) 変換テーブル
//
//  DeviceFactory の DEVICE_* (チップドライバ生成用IDと deviceType) とは
//  独立した分類。ボイスパラメータ・ハードウェア機能の互換性のみに着目する。
// ================================================================

uint8_t FITOMConfig::deviceTypeToVoicePatchType(uint32_t deviceType) noexcept
{
    switch (deviceType) {
    case DEVICE_OPN:  case DEVICE_OPNB: case DEVICE_OPNC:
        return VOICE_PATCH_OPN;
    case DEVICE_OPN2: case DEVICE_OPN2C: case DEVICE_OPN2L:
    case DEVICE_OPNA: case DEVICE_OPN3L:
    case DEVICE_2610B: case DEVICE_F286: case DEVICE_OPN3:
        return VOICE_PATCH_OPN2;

    case DEVICE_OPM:  return VOICE_PATCH_OPM;
    case DEVICE_OPP:  return VOICE_PATCH_OPM;   // YM2164 は OPM 互換
    case DEVICE_OPZ:  return VOICE_PATCH_OPZ;
    case DEVICE_OPZ2: return VOICE_PATCH_OPZ2;

    case DEVICE_OPL:    case DEVICE_Y8950: return VOICE_PATCH_OPL;
    case DEVICE_OPL2:                      return VOICE_PATCH_OPL2;
    // COPL3_2はOPL2よりWSが広い(3bit/8波形 vs 2bit/4波形)ため別分類。
    // OPL2へのフォールバックはWS<4の場合のみ許可 (DeviceFactory::acceptsFallback)。
    case DEVICE_OPL3_2:                    return VOICE_PATCH_OPL3_2;

    case DEVICE_OPLL:   return VOICE_PATCH_OPLL;
    case DEVICE_OPLLP:  return VOICE_PATCH_OPLLP;
    case DEVICE_OPLLX:  return VOICE_PATCH_OPLLX;
    case DEVICE_OPLL2:  return VOICE_PATCH_OPLL; // YM2420 は OPLL(0x28) に統合済み
    case DEVICE_VRC7:   return VOICE_PATCH_VRC7;

    case DEVICE_OPL3: case DEVICE_OPN3_L3: return VOICE_PATCH_OPL3;
    case DEVICE_OPL4AWM: return VOICE_PATCH_AWM;

    // OPL系内蔵リズムチャンネル: COPNARhythm/COPLLRhythm(VOICE_PATCH_NONE
    // のまま、findDeviceIndexByDeviceType()で個別に検索する設計)とは異なり、
    // リズム音が実際のFMオペレータパラメータを要求するため、専用の
    // VoicePatchType(VOICE_PATCH_OPL_RHY)を持たせる(2026年7月)。
    case DEVICE_OPL_RHY: return VOICE_PATCH_OPL_RHY;

    case DEVICE_SSG: case DEVICE_PSG: case DEVICE_SSGL: case DEVICE_SSGLP:
    case DEVICE_SSGS: case DEVICE_DSG:
        return VOICE_PATCH_SSG;
    case DEVICE_EPSG:  return VOICE_PATCH_EPSG;
    case DEVICE_DCSG:  return VOICE_PATCH_DCSG;
    case DEVICE_SAA:   return VOICE_PATCH_SAA;    // CSAA1099 実装済み

    case DEVICE_SCC: case DEVICE_SCCP: return VOICE_PATCH_SCC;

    case DEVICE_ADPCMB: return VOICE_PATCH_ADPCMB;
    case DEVICE_ADPCMB_OPNA: return VOICE_PATCH_ADPCMB; // 音色パラメータ形式はOPNBと共通
    case DEVICE_ADPCMA: return VOICE_PATCH_ADPCMA;
    case DEVICE_PCMD8:  return VOICE_PATCH_PCMD8;

    default:
        return VOICE_PATCH_NONE;
    }
}

uint32_t FITOMConfig::voicePatchTypeToVoiceGroup(uint8_t vpt) noexcept
{
    switch (vpt) {
    case VOICE_PATCH_OPN:  case VOICE_PATCH_OPN2:
        return VOICE_GROUP_OPNA;
    case VOICE_PATCH_OPM:  case VOICE_PATCH_OPZ:  case VOICE_PATCH_OPZ2:
        return VOICE_GROUP_OPM;
    case VOICE_PATCH_OPL:  case VOICE_PATCH_OPL2: case VOICE_PATCH_OPL3_2:
        return VOICE_GROUP_OPL2;
    case VOICE_PATCH_OPLL: case VOICE_PATCH_OPLLP:
    case VOICE_PATCH_OPLLX: case VOICE_PATCH_VRC7:
        return VOICE_GROUP_OPLL;
    case VOICE_PATCH_OPL3:
        return VOICE_GROUP_OPL3;
    // OPL系内蔵リズムチャンネル専用。独立した名前空間とする(通常の
    // OPL2用HwBankとは別管理、2026年7月)。旧FITOMのDEVICE_RHYTHM由来の
    // 既存グループを流用する。
    case VOICE_PATCH_OPL_RHY:
        return VOICE_GROUP_RHYTHM;
    case VOICE_PATCH_SD1: case VOICE_PATCH_MA3:
    case VOICE_PATCH_MA5: case VOICE_PATCH_MA7:
        return VOICE_GROUP_MA3;
    case VOICE_PATCH_SSG: case VOICE_PATCH_EPSG:
    case VOICE_PATCH_DCSG: case VOICE_PATCH_SAA:
    case VOICE_PATCH_SCC:
        return VOICE_GROUP_PSG;
    case VOICE_PATCH_ADPCMB_Y8950: case VOICE_PATCH_ADPCMB:
    case VOICE_PATCH_ADPCMA: case VOICE_PATCH_PCMD8:
    case VOICE_PATCH_AWM:
        return VOICE_GROUP_PCM;
    default:
        return VOICE_GROUP_NONE;
    }
}

uint8_t FITOMConfig::stringToVoicePatchType(const std::string& s) noexcept
{
    // OPNA/OPNBは音色パラメータ互換性の観点でOPN2に統合済み
    if (s == "OPN")      return VOICE_PATCH_OPN;
    if (s == "OPN2" || s == "OPNA" || s == "OPNB") return VOICE_PATCH_OPN2;
    if (s == "OPM")       return VOICE_PATCH_OPM;
    if (s == "OPZ")       return VOICE_PATCH_OPZ;
    if (s == "OPZ2")      return VOICE_PATCH_OPZ2;
    if (s == "OPL")       return VOICE_PATCH_OPL;
    if (s == "OPL2")      return VOICE_PATCH_OPL2;
    if (s == "OPL3_2")    return VOICE_PATCH_OPL3_2;
    if (s == "OPL_RHY")   return VOICE_PATCH_OPL_RHY;
    if (s == "OPLL")      return VOICE_PATCH_OPLL;
    if (s == "OPLLP")     return VOICE_PATCH_OPLLP;
    if (s == "OPLLX")     return VOICE_PATCH_OPLLX;
    if (s == "VRC7")      return VOICE_PATCH_VRC7;
    if (s == "OPL3")      return VOICE_PATCH_OPL3;
    if (s == "SD1" || s == "SD-1") return VOICE_PATCH_SD1;
    if (s == "MA3" || s == "MA-3") return VOICE_PATCH_MA3;
    if (s == "MA5" || s == "MA-5") return VOICE_PATCH_MA5;
    if (s == "MA7" || s == "MA-7") return VOICE_PATCH_MA7;
    if (s == "SSG")       return VOICE_PATCH_SSG;
    if (s == "AY8930")    return VOICE_PATCH_EPSG;
    if (s == "DCSG")      return VOICE_PATCH_DCSG;
    if (s == "SAA1099" || s == "SAA") return VOICE_PATCH_SAA;
    if (s == "SCC" || s == "SCCP")    return VOICE_PATCH_SCC;
    if (s == "ADPCMB_Y8950") return VOICE_PATCH_ADPCMB_Y8950;
    if (s == "ADPCMB")    return VOICE_PATCH_ADPCMB;
    if (s == "ADPCMA")    return VOICE_PATCH_ADPCMA;
    if (s == "PCMD8")     return VOICE_PATCH_PCMD8;
    if (s == "AWM")       return VOICE_PATCH_AWM;
    // 後方互換: 旧来の粗い "PSG"/"PCM" 指定は代表値にフォールバック
    if (s == "PSG")       return VOICE_PATCH_SSG;
    if (s == "PCM")       return VOICE_PATCH_ADPCMB;
    return VOICE_PATCH_NONE;
}

// ================================================================
//  Sub-device 自動生成 (composite chip)
// ================================================================
// ================================================================
//  Sub-device 自動生成 (composite chip)
// ================================================================
void FITOMConfig::pushDeviceEntries(const std::string& baseLabel, uint32_t baseDeviceType,
                                     std::shared_ptr<IPort> port, std::shared_ptr<IPort> port2,
                                     int sampleRate, int extraSlot, bool rhythmModeFromProfile,
                                     bool stereoPairRequested)
{
    std::vector<SubDeviceSpec> spec;
    if (resolveCompositeSpec(baseDeviceType, spec)) {
        // composite展開されたサブデバイス群へのstereo_pair適用は複雑になるため
        // 現時点では非対応 (将来課題)。単独チップのみサポートする。
        if (stereoPairRequested) {
            FITOM_LOG_WARN("stereo_pair: '" << baseLabel
                << "' はcomposite展開対象のため無視されます (単独チップのみ対応)");
        }
        int group = nextCompositeGroupId_++;
        for (const auto& s : spec) {
            DeviceEntry e;
            e.label          = baseLabel + s.labelSuffix;
            e.deviceType     = s.deviceType;
            e.sampleRate     = sampleRate;
            e.extraSlot      = extraSlot;
            e.port           = port;                          // 全サブデバイスで共有
            e.port2          = s.usesExtraPort ? port2 : nullptr;
            e.rhythmMode     = s.rhythmCapable ? rhythmModeFromProfile : false;
            e.compositeGroup = group;
            FITOM_LOG_INFO("Device added: " << e.label
                << " (composite sub-device of '" << baseLabel << "', type=0x"
                << std::hex << s.deviceType << ")");
            devices_.push_back(std::move(e));
        }
    } else {
        DeviceEntry e;
        e.label      = baseLabel;
        e.deviceType = baseDeviceType;
        e.sampleRate = sampleRate;
        e.extraSlot  = extraSlot;
        e.port       = port;
        e.port2      = port2;
        e.rhythmMode = rhythmModeFromProfile;
        e.stereoPairRequested = stereoPairRequested;
        FITOM_LOG_INFO("Device added: " << e.label);
        devices_.push_back(std::move(e));
    }
}

// ================================================================
//  同種デバイス自動束ね (CSpanDevice)
// ================================================================
// ================================================================
//  リニアステレオ化 (CLinearPanDevice、明示指定のみ)
// ================================================================
void FITOMConfig::mergeStereoPairDevices()
{
    struct Key {
        uint8_t     voicePatchType;
        std::string interfaceDesc;
        bool operator==(const Key& o) const {
            return voicePatchType == o.voicePatchType && interfaceDesc == o.interfaceDesc;
        }
    };

    std::vector<bool> removed(devices_.size(), false);

    for (size_t i = 0; i < devices_.size(); ++i) {
        if (removed[i] || !devices_[i].stereoPairRequested || !devices_[i].port) continue;
        if (devices_[i].port->getPanpot() != 1) continue; // L側のみを起点に探す (pan=1)

        uint8_t vptI = deviceTypeToVoicePatchType(devices_[i].deviceType);
        if (vptI == VOICE_PATCH_NONE) continue;
        Key keyI{vptI, devices_[i].port->getInterfaceDesc()};

        for (size_t j = 0; j < devices_.size(); ++j) {
            if (i == j || removed[j] || !devices_[j].stereoPairRequested || !devices_[j].port) continue;
            if (devices_[j].port->getPanpot() != 2) continue; // R側 (pan=2)
            uint8_t vptJ = deviceTypeToVoicePatchType(devices_[j].deviceType);
            if (vptJ == VOICE_PATCH_NONE) continue;
            Key keyJ{vptJ, devices_[j].port->getInterfaceDesc()};
            if (!(keyI == keyJ)) continue;

            // devices_[i] (L) に devices_[j] (R) を統合する
            devices_[i].stereoPairPort = devices_[j].port;
            removed[j] = true;
            FITOM_LOG_INFO("mergeStereoPairDevices: '" << devices_[j].label
                << "' (R) merged into '" << devices_[i].label
                << "' (L) as CLinearPanDevice pair");
            break; // 1つのL側につきR側は1つだけ対応
        }
    }

    std::vector<DeviceEntry> result;
    result.reserve(devices_.size());
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (!removed[i]) result.push_back(std::move(devices_[i]));
    }
    devices_ = std::move(result);
}

void FITOMConfig::mergeSpannableDevices()
{
    // グループ化キー: (voicePatchType, interfaceDesc, panpot)。
    // ただし stereoPairPort 設定済み (Step1でステレオ化済み) のエントリは
    // もはや単一のL/Rパンという概念を持たないため、panpot をキーから除外する
    // (panpot=-1 の特別値で表現し、常に一致させる)。
    struct Key {
        uint8_t     voicePatchType;
        std::string interfaceDesc;
        int         panpot;
        bool operator==(const Key& o) const {
            return voicePatchType == o.voicePatchType
                && interfaceDesc  == o.interfaceDesc
                && panpot         == o.panpot;
        }
    };
    auto makeKey = [this](const DeviceEntry& e) -> Key {
        uint8_t vpt = deviceTypeToVoicePatchType(e.deviceType);
        int pan = e.stereoPairPort ? -1 : e.port->getPanpot();
        return Key{vpt, e.port->getInterfaceDesc(), pan};
    };

    std::vector<bool> merged(devices_.size(), false);

    for (size_t i = 0; i < devices_.size(); ++i) {
        if (merged[i] || !devices_[i].port) continue;
        uint8_t vptI = deviceTypeToVoicePatchType(devices_[i].deviceType);
        if (vptI == VOICE_PATCH_NONE) continue; // 未分類デバイスは束ね対象外

        Key keyI = makeKey(devices_[i]);

        for (size_t j = i + 1; j < devices_.size(); ++j) {
            if (merged[j] || !devices_[j].port) continue;
            if (devices_[i].port.get() == devices_[j].port.get()) continue; // 同一ポートは対象外
            uint8_t vptJ = deviceTypeToVoicePatchType(devices_[j].deviceType);
            if (vptJ == VOICE_PATCH_NONE) continue;

            Key keyJ = makeKey(devices_[j]);
            if (!(keyI == keyJ)) continue;

            // devices_[j] (モノラルまたはステレオペア済み) を devices_[i] に統合する
            devices_[i].spanGroups.push_back({devices_[j].port, devices_[j].stereoPairPort});
            for (auto& g : devices_[j].spanGroups) devices_[i].spanGroups.push_back(g);
            merged[j] = true;
            FITOM_LOG_INFO("mergeSpannableDevices: '" << devices_[j].label
                << "' merged into '" << devices_[i].label
                << "' (VoicePatchType=0x" << std::hex << (int)vptI << ")");
        }
    }

    // 統合された (merged==true) エントリを削除する
    std::vector<DeviceEntry> result;
    result.reserve(devices_.size());
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (!merged[i]) result.push_back(std::move(devices_[i]));
    }
    devices_ = std::move(result);
}

int FITOMConfig::getDeviceSpanGroupCount(int index) const {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return 0;
    return static_cast<int>(devices_[index].spanGroups.size());
}

IPort* FITOMConfig::getDeviceSpanGroupPrimary(int index, int k) const {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return nullptr;
    const auto& sg = devices_[index].spanGroups;
    if (k < 0 || k >= static_cast<int>(sg.size())) return nullptr;
    return sg[k].primary.get();
}

IPort* FITOMConfig::getDeviceSpanGroupStereoPair(int index, int k) const {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return nullptr;
    const auto& sg = devices_[index].spanGroups;
    if (k < 0 || k >= static_cast<int>(sg.size())) return nullptr;
    return sg[k].stereoPair.get();
}

IPort* FITOMConfig::getDeviceStereoPairPort(int index) const {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return nullptr;
    return devices_[index].stereoPairPort.get();
}

bool FITOMConfig::resolveCompositeSpec(uint32_t baseDeviceType,
                                        std::vector<SubDeviceSpec>& outSpec)
{
    outSpec.clear();
    switch (baseDeviceType) {
    case DEVICE_OPNA:
    case DEVICE_F286:
    case DEVICE_OPN3:
        // OPNA本体(FM 6ch) + SSG(3ch) + ADPCM-B + リズム(6パート)。
        // 全サブデバイスが同一の物理ポート(+extraPort)を共有する。
        // ADPCM-BはOPNB系と異なるレジスタマップのため DEVICE_ADPCMB_OPNA を使う。
        outSpec.push_back({baseDeviceType,      "-FM",     true,  false});
        outSpec.push_back({DEVICE_SSG,          "-SSG",    false, false});
        outSpec.push_back({DEVICE_ADPCMB_OPNA,  "-ADPCMB", false, false});
        outSpec.push_back({DEVICE_OPNA_RHY,     "-RHYTHM", false, true});
        return true;

    case DEVICE_2610B:
        // OPNB/OPNBB (YM2610/YM2610B): FM本体 + SSG + ADPCM-A + ADPCM-B。
        // YM2610無印は ADPCM-B のメモリ空間が無いが (実質OPNA兼用扱い)、
        // ここでは新しい YM2610B (2610B) 系のみ ADPCM-B も併せて生成する。
        outSpec.push_back({baseDeviceType, "-FM",     true,  false});
        outSpec.push_back({DEVICE_SSG,     "-SSG",    false, false});
        outSpec.push_back({DEVICE_ADPCMA,  "-ADPCMA", false, false});
        outSpec.push_back({DEVICE_ADPCMB,  "-ADPCMB", false, false});
        return true;

    case DEVICE_OPL3:
    case DEVICE_OPN3_L3:
        // OPL3: 4OPモード(6ch) + 2OP残余(6ch)。同一の物理ポート(port1+port2)を共有する。
        // COPL3_2側のport1サブチップ(ch6-8)にrhythm_modeを適用すると
        // "OPL3(6ch)+OPL2(3ch)+Rhythm(5ch)" 構成になる。
        outSpec.push_back({DEVICE_OPL3,   "-4OP", true, false});
        outSpec.push_back({DEVICE_OPL3_2, "-2OP", true, true});
        return true;

    case DEVICE_OPL4:
        // OPL4 = OPL3(FM部、完全互換) + AWM(PCM部、YRW801 ROM音色)。
        // FM部はOPL3と同じ3サブデバイス構成(4OP+2OP)、AWM部はさらに別ポート
        // (メモリアクセス系、ここではport共有のみで足りる)を追加する。
        outSpec.push_back({DEVICE_OPL3,    "-FM-4OP", true,  false});
        outSpec.push_back({DEVICE_OPL3_2,  "-FM-2OP", true,  false});
        outSpec.push_back({DEVICE_OPL4AWM, "-AWM",    false, false});
        return true;

    case DEVICE_OPLL:
    case DEVICE_OPLL2:
    case DEVICE_OPLLP:
    case DEVICE_OPLLX:
        // OPLL系: 本体(9ch、rhythm_mode時はch6-8無効化) + リズム(5パート)。
        // 同一の物理ポートを共有する。リズムデバイスはOPLLファミリ共通の
        // レジスタ体系のため、派生型に関わらずDEVICE_OPLL_RHYを使う。
        // (VRC7はリズム回路自体を持たないため対象外)
        outSpec.push_back({baseDeviceType,  "-FM",     false, true});
        outSpec.push_back({DEVICE_OPLL_RHY, "-RHYTHM", false, false});
        return true;

    default:
        return false;
    }
}

uint8_t FITOMConfig::getVoicePatchType(int deviceIndex) const
{
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(devices_.size()))
        return VOICE_PATCH_NONE;
    return deviceTypeToVoicePatchType(devices_[deviceIndex].deviceType);
}

int FITOMConfig::findDeviceIndexByVoicePatchType(uint8_t voicePatchType) const
{
    if (voicePatchType == VOICE_PATCH_NONE) return -1;
    for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
        if (deviceTypeToVoicePatchType(devices_[i].deviceType) == voicePatchType)
            return i;
    }
    return -1;
}

int FITOMConfig::findDeviceIndexByDeviceType(uint32_t deviceType) const
{
    if (deviceType == DEVICE_NONE) return -1;
    for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
        if (devices_[i].deviceType == deviceType) return i;
    }
    return -1;
}

std::vector<int> FITOMConfig::findAllFallbackDeviceIndices(uint8_t sourceVoicePatchType,
                                                             const HwPatch& patch) const
{
    std::vector<int> result;
    for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
        if (DeviceFactory::acceptsFallback(devices_[i].deviceType, sourceVoicePatchType, patch)) {
            result.push_back(i);
        }
    }
    return result;
}

// DeviceFactory を使ってポートからデバイスを生成する


// ================================================================
//  チップ名 → DEVICE_* 変換ヘルパー
// ================================================================
static uint32_t resolveChipDeviceId(const std::string& chipName)
{
    // プロファイルのchip名文字列とDEVICE_*の対応
    static const std::pair<const char*, uint32_t> kChipMap[] = {
        {"OPN",   DEVICE_OPN},   {"OPNA",  DEVICE_OPNA},
        {"OPN2",  DEVICE_OPN2},  {"OPN2C", DEVICE_OPN2C},
        {"OPN2L", DEVICE_OPN2L}, {"OPN3",  DEVICE_OPN3},
        {"OPNB",  DEVICE_OPNB},  {"OPNBB", DEVICE_2610B},
        {"OPM",   DEVICE_OPM},   {"OPP",   DEVICE_OPP},
        {"OPZ",   DEVICE_OPZ},
        {"OPL",   DEVICE_OPL},   {"OPL2",  DEVICE_OPL2},
        {"OPL3",  DEVICE_OPL3},  {"OPL3_2", DEVICE_OPL3_2}, {"Y8950", DEVICE_Y8950},
        {"OPL4",  DEVICE_OPL4},
        {"OPLL",  DEVICE_OPLL},  {"OPLL2", DEVICE_OPLL2},
        {"OPLLP", DEVICE_OPLLP}, {"OPLLX", DEVICE_OPLLX},
        {"VRC7",  DEVICE_VRC7},
        {"SSG",   DEVICE_SSG},   {"PSG",   DEVICE_PSG},
        {"EPSG",  DEVICE_EPSG},  {"DCSG",  DEVICE_DCSG},
        {"SCC",   DEVICE_SCC},   {"SCCP",  DEVICE_SCCP},
        {"SAA",   DEVICE_SAA},   {"SAA1099", DEVICE_SAA},
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

    // drum_banks[]: プログラムチェンジ1つぶんずつ、独立したファイル
    // (*.drumkit.json) を割り当てる方式。1ファイルに全prog分を
    // 詰め込む旧方式は、ファイル肥大化のため廃止。
    // "bank"フィールドは持たない (ドラムバンクは常に固定バンク番号0。
    // CRhythmChはMIDI経由でのバンク切替をサポートしないため)。
    for (const auto& entry : banks["drum_banks"]) {
        int prog = entry.value("prog", -1);
        std::string file = entry.value("file", "");
        if (prog < 0 || prog >= 128 || file.empty()) {
            FITOM_LOG_WARN("drum_banks: invalid 'prog' or missing 'file', skipping");
            continue;
        }

        std::filesystem::path path = file;
        if (path.is_relative()) path = baseDir / path;

        pm.loadDrumKitJson(path, prog);
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
            // "group" 文字列 → VoicePatchType → VoiceGroup(検索キー) の2段変換。
            // (旧実装は "OPZ" 等の細分類文字列を判定できず VOICE_GROUP_OPNA に
            //  誤って落ちるバグがあったため、ここで併せて修正する)
            uint8_t  voicePatchType = FITOMConfig::stringToVoicePatchType(groupStr);
            if (voicePatchType == VOICE_PATCH_NONE) {
                FITOM_LOG_WARN("hw_banks: unknown group \"" << groupStr
                    << "\" in " << file << " — skipped");
                continue;
            }
            // サンプルベース音源系 (VOICE_PATCH_AWM等) はHwBankRegistryでは
            // なくSampleZoneBankRegistry (専用スキーマ) に読み込む。
            if (voicePatchType == VOICE_PATCH_AWM) {
                pm.loadSampleZoneBankJson(path, bankNo, voicePatchType);
                continue;
            }
            uint32_t group = FITOMConfig::voicePatchTypeToVoiceGroup(voicePatchType);
            pm.loadHwBankJson(path, group, bankNo, voicePatchType);
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

} // namespace fitom
