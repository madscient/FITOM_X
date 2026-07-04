// fitom/PatchManager.cpp
// パッチ解決・バンク管理の実装

#include "fitom/PatchManager.h"
#include "fitom/PcmBankData.h"
#include "fitom/Config.h"
#include "fitom/DeviceFactory.h"
#include "fitom/FITOMdefine.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

namespace fitom {

using json = nlohmann::json;

// ================================================================
//  HwBankRegistry: デバイス ID → VoiceGroup 解決
// ================================================================

HwBankRegistry::VoiceGroup HwBankRegistry::groupFromDeviceId(uint32_t deviceId)
{
    // FITOMdefine.h の DEVICE_* → VOICE_GROUP_* への対応
    // 旧 CFITOM::GetDeviceVoiceGroupMask 相当
    switch (deviceId & 0xFF) {
    case DEVICE_OPM:  case DEVICE_OPP:  case DEVICE_OPZ:  case DEVICE_OPZ2:
        return VOICE_GROUP_OPM;
    case DEVICE_OPN:  case DEVICE_OPN2: case DEVICE_OPNA: case DEVICE_OPN3L:
    case DEVICE_OPNB: case DEVICE_OPNC: case DEVICE_OPN2C: case DEVICE_OPN2L:
    case DEVICE_2610B: case DEVICE_F286: case DEVICE_OPN3:
        return VOICE_GROUP_OPNA;
    case DEVICE_OPL:  case DEVICE_OPL2: case DEVICE_Y8950:
        return VOICE_GROUP_OPL2;
    case DEVICE_OPL3: case DEVICE_OPN3_L3:
        return VOICE_GROUP_OPL3;
    case DEVICE_OPLL: case DEVICE_OPLL2: case DEVICE_OPLLP: case DEVICE_OPLLX:
        return VOICE_GROUP_OPLL;
    case DEVICE_SSG:  case DEVICE_PSG:  case DEVICE_SSGL: case DEVICE_SSGLP:
    case DEVICE_SSGS: case DEVICE_EPSG: case DEVICE_DCSG: case DEVICE_SCC:
    case DEVICE_SCCP: case DEVICE_SAA:  case DEVICE_DSG:
        return VOICE_GROUP_PSG;
    case DEVICE_OPL4: case DEVICE_OPL4ML: case DEVICE_OPL4ML2:
        return VOICE_GROUP_OPL4;
    case DEVICE_ADPCM: case DEVICE_ADPCMA: case DEVICE_ADPCMB:
    case DEVICE_PCMD8: case DEVICE_MA1: case DEVICE_MA2:
    case DEVICE_MA3:   case DEVICE_MA5:  case DEVICE_MA7:
        return VOICE_GROUP_PCM;
    case DEVICE_RHYTHM:
        return VOICE_GROUP_RHYTHM;
    default:
        return VOICE_GROUP_NONE;
    }
}

// ================================================================
//  PatchManager
// ================================================================

PatchBank& PatchManager::getPatchBank(int bankNo)
{
    return patchBanks_[bankNo];
}

const PatchBank* PatchManager::findPatchBank(int bankNo) const
{
    auto it = patchBanks_.find(bankNo);
    return (it != patchBanks_.end()) ? &it->second : nullptr;
}

namespace {

// ================================================================
//  VoicePatchType フォールバック
//
//  voicePatchTypeToVoiceGroup() (Config.cpp) はバンクファイルの名前空間
//  分類が目的であり、「音色データがレジスタレベルで互換性を持つか」を
//  意味しない (例: VOICE_GROUP_PSGはSSG/AY8930/DCSG/SAA1099/SCCを一括り
//  にしているが、ALG/WSフィールドの意味がチップごとに全く異なり
//  互換性が無い)。
//
//  フォールバック可否の判定は、各チップドライバファイルに実装された
//  静的関数 (copnAcceptsFallback等、DeviceFactory::acceptsFallback経由で
//  呼ばれる) に委ねる。判定にはHwPatchの内容 (拡張パラメータの使用有無、
//  プリセット/ユーザー区別等) が必要になる場合があるため、チップ
//  ドライバ自身の知識として実装するのが最も正確 (静的テーブルでは
//  「OPZ→OPMは拡張パラメータ未使用時のみ」のような条件を表現できない)。
//
//  フォールバックは「同じHwPatchデータを、要求されたVoicePatchTypeとは
//  別の接続デバイスで鳴らす」という意味であり、HwBank/HwPatchの検索
//  そのものは常にlayer本来のvoicePatchType基準で行う (データの検索先は
//  変えず、実際に発音を担当するデバイスの選択だけが変わる)。
// ================================================================

// 接続されている全デバイスに対して DeviceFactory::acceptsFallback を
// 順に問い合わせ、受け入れ可能な最初の1つのインデックスを返す
// (-1 = 見つからない)。usedVptにそのデバイスの実際のVoicePatchTypeを返す。
int findFallbackDeviceIndex(const FITOMConfig& config, uint8_t sourceVoicePatchType,
                             const HwPatch& patch, uint8_t& usedVpt) {
    int count = config.getDeviceCount();
    for (int i = 0; i < count; ++i) {
        uint32_t deviceType = config.getDeviceType(i);
        if (DeviceFactory::acceptsFallback(deviceType, sourceVoicePatchType, patch)) {
            usedVpt = FITOMConfig::deviceTypeToVoicePatchType(deviceType);
            return i;
        }
    }
    return -1;
}

} // namespace

ResolvedPatch PatchManager::resolve(const Patch& patch,
                                     const FITOMConfig& config) const
{
    ResolvedPatch result;
    result.patch = &patch;

    // SwPatch の解決
    result.swPatch = swReg_.resolve(patch.swBank, patch.swProg);
    if (!result.swPatch) {
        FITOM_LOG_WARN("SwPatch not found: bank=" << patch.swBank
            << " prog=" << patch.swProg << " — using default");
    }

    // 各 ToneLayer の HwPatch を解決
    // voicePatchType に一致するデバイスが見つからない、または
    // HwBank のタグと不一致の場合、そのレイヤーだけをスキップする
    // (Patch全体は無効にしない。確保できたレイヤーのみ発音する)。
    for (int i = 0; i < MAX_TONE_LAYERS; ++i) {
        const auto& layer = patch.layers[i];
        if (!layer.isActive()) continue;

        // データ (HwBank/HwPatch) は常に layer 本来の voicePatchType 基準で
        // 検索する。フォールバック可否の判定 (OPLLファミリーのプリセット/
        // ユーザー判定等) にHwPatchの内容が必要なため、デバイス検索より先に
        // データを解決する。
        auto group = FITOMConfig::voicePatchTypeToVoiceGroup(layer.voicePatchType);
        const HwBank* bank = hwReg_.find(group, layer.hwBank);
        if (!bank || bank->voicePatchType != layer.voicePatchType) {
            FITOM_LOG_WARN("resolve: layer=" << i
                << " bank=" << (int)layer.hwBank
                << " voicePatchType mismatch (bank="
                << (bank ? bank->voicePatchType : 0)
                << ", layer=" << (int)layer.voicePatchType
                << ") — layer skipped");
            continue;
        }

        const HwPatch* hwPatch = &bank->get(layer.hwProg);
        if (!hwPatch->isValid()) {
            FITOM_LOG_WARN("HwPatch not found: layer=" << i
                << " bank=" << (int)layer.hwBank
                << " prog=" << (int)layer.hwProg);
            continue;
        }

        // デバイス検索: まず厳密一致を試し、無ければ接続済み全デバイスに
        // フォールバック受け入れ可否を問い合わせる。見つかった最初の
        // 1つだけを採用する (同一レイヤーから複数デバイスが同時に
        // 発音することはない)。
        int deviceIndex = config.findDeviceIndexByVoicePatchType(layer.voicePatchType);
        uint8_t usedVpt = layer.voicePatchType;
        if (deviceIndex < 0) {
            deviceIndex = findFallbackDeviceIndex(config, layer.voicePatchType, *hwPatch, usedVpt);
        }
        if (deviceIndex < 0) {
            FITOM_LOG_WARN("resolve: layer=" << i << " voicePatchType=0x"
                << std::hex << (int)layer.voicePatchType
                << " — no matching device (fallback exhausted), layer skipped");
            continue;
        }
        if (usedVpt != layer.voicePatchType) {
            FITOM_LOG_INFO("resolve: layer=" << i << " voicePatchType=0x"
                << std::hex << (int)layer.voicePatchType
                << " not connected, falling back to 0x" << (int)usedVpt);
        }

        ResolvedLayer rl;
        rl.layer       = &layer;
        rl.deviceIndex = deviceIndex;
        rl.hwPatch     = hwPatch;
        result.layers[result.layerCount++] = rl;
    }

    return result;
}

ResolvedPatch PatchManager::resolveDirect(uint8_t voicePatchType, uint8_t hwBank, uint8_t hwProg,
                                          const FITOMConfig& config, Patch& storage) const
{
    auto group = FITOMConfig::voicePatchTypeToVoiceGroup(voicePatchType);
    const HwBank* bank = hwReg_.find(group, hwBank);
    if (!bank || bank->voicePatchType != voicePatchType) {
        FITOM_LOG_WARN("resolveDirect: bank=" << (int)hwBank
            << " voicePatchType mismatch (bank="
            << (bank ? bank->voicePatchType : 0)
            << ", requested=" << (int)voicePatchType << ")");
        return {};
    }

    const HwPatch* hwPatch = &bank->get(hwProg);
    if (!hwPatch->isValid()) {
        FITOM_LOG_WARN("resolveDirect: HwPatch not found bank=" << (int)hwBank
            << " prog=" << (int)hwProg);
        return {};
    }

    int deviceIndex = config.findDeviceIndexByVoicePatchType(voicePatchType);
    if (deviceIndex < 0) {
        uint8_t usedVpt = voicePatchType;
        deviceIndex = findFallbackDeviceIndex(config, voicePatchType, *hwPatch, usedVpt);
    }
    if (deviceIndex < 0) {
        FITOM_LOG_WARN("resolveDirect: voicePatchType=0x" << std::hex << (int)voicePatchType
            << " — no matching device (fallback exhausted)");
        return {};
    }

    // storage (呼び出し元が寿命を保持) に単層Patchを構築する
    storage = Patch::fromSingleLayer(*hwPatch, voicePatchType, hwBank, hwProg);

    ResolvedPatch result;
    result.patch      = &storage;
    result.swPatch     = nullptr;   // 直接モードはSWパッチなし (VTL等は0=無感度)
    ResolvedLayer rl;
    rl.layer       = &storage.layers[0];
    rl.hwPatch     = hwPatch;
    rl.deviceIndex = deviceIndex;
    result.layers[0]  = rl;
    result.layerCount = 1;
    return result;
}


ResolvedPatch PatchManager::resolve(int patchBankNo, int prog,
                                     const FITOMConfig& config) const
{
    const PatchBank* pb = findPatchBank(patchBankNo);
    if (!pb) {
        FITOM_LOG_WARN("PatchBank " << patchBankNo << " not found");
        return {};
    }
    const Patch& p = pb->get(prog);
    if (!p.isValid()) {
        FITOM_LOG_WARN("Patch bank=" << patchBankNo << " prog=" << prog << " is empty");
        return {};
    }
    return resolve(p, config);
}

// ================================================================
//  JSON 変換ユーティリティ
// ================================================================

namespace {

// FmHwOp ↔ JSON
json hwOpToJson(const FmHwOp& op) {
    return json{
        {"AR",op.AR},{"DR",op.DR},{"SL",op.SL},{"SR",op.SR},{"RR",op.RR},
        {"TL",op.TL},{"KSR",op.KSR},{"KSL",op.KSL},
        {"MUL",op.MUL},{"DT1",op.DT1},{"DT2",op.DT2},{"FXV",op.FXV},
        {"AM",op.AM},{"VIB",op.VIB},{"EGT",op.EGT},{"WS",op.WS}
    };
}
void jsonToHwOp(const json& j, FmHwOp& op) {
    auto g = [&](const char* k, uint8_t& v){ if(j.contains(k)) v=j[k].get<uint8_t>(); };
    g("AR",op.AR); g("DR",op.DR); g("SL",op.SL); g("SR",op.SR); g("RR",op.RR);
    g("TL",op.TL); g("KSR",op.KSR); g("KSL",op.KSL);
    g("MUL",op.MUL); g("DT1",op.DT1); g("DT2",op.DT2);
    if (j.contains("FXV")) op.FXV = j["FXV"].get<int16_t>();
    g("AM",op.AM); g("VIB",op.VIB); g("EGT",op.EGT); g("WS",op.WS);
}

json hwPatchToJson(const HwPatch& p) {
    json ops = json::array();
    for (int i = 0; i < MAX_HW_OPS; ++i) ops.push_back(hwOpToJson(p.hwOp[i]));
    return json{
        {"id",p.id},{"name",p.name},
        {"FB",p.hw.FB},{"ALG",p.hw.ALG},{"AMS",p.hw.AMS},{"PMS",p.hw.PMS},{"NFQ",p.hw.NFQ},
        {"FB2",p.hw.FB2},
        {"ops",ops},
        {"ext",json{{"REV",p.ext.REV},{"EGS",p.ext.EGS},{"DM0",p.ext.DM0},
                    {"DT3",p.ext.DT3},{"ALG_EXT",p.ext.ALG_EXT},{"HWEP",p.ext.HWEP}}}
    };
}
HwPatch jsonToHwPatch(const json& j, uint32_t bank, uint32_t prog) {
    HwPatch p;
    p.id = (bank << 16) | prog;
    if (j.contains("name")) {
        std::string n = j["name"].get<std::string>();
        std::strncpy(p.name, n.c_str(), sizeof(p.name)-1);
    }
    auto g8 = [&](const char* k, uint8_t& v){ if(j.contains(k)) v=j[k].get<uint8_t>(); };
    g8("FB",p.hw.FB); g8("ALG",p.hw.ALG); g8("AMS",p.hw.AMS);
    g8("PMS",p.hw.PMS); g8("NFQ",p.hw.NFQ); g8("FB2",p.hw.FB2);
    if (j.contains("ops") && j["ops"].is_array()) {
        for (int i = 0; i < MAX_HW_OPS && i < (int)j["ops"].size(); ++i)
            jsonToHwOp(j["ops"][i], p.hwOp[i]);
    }
    if (j.contains("ext")) {
        // 注意: ext サブオブジェクトのフィールドは ex から読む必要がある。
        // (以前は誤って親スコープ j を参照する g8 を流用しており、
        //  ext.* が正しく読み込まれていなかった)
        const auto& ex = j["ext"];
        auto ge8 = [&](const char* k, uint8_t& v){ if(ex.contains(k)) v=ex[k].get<uint8_t>(); };
        ge8("REV",p.ext.REV); ge8("EGS",p.ext.EGS); ge8("DM0",p.ext.DM0);
        ge8("DT3",p.ext.DT3); ge8("ALG_EXT",p.ext.ALG_EXT);
        if (ex.contains("HWEP")) p.ext.HWEP = ex["HWEP"].get<uint16_t>();
    }
    return p;
}

json swOpToJson(const FmSwOp& op) {
    return json{
        {"VTL",op.VTL},{"VAR",op.VAR},{"VDR",op.VDR},{"VSL",op.VSL},
        {"VSR",op.VSR},{"VRR",op.VRR},{"VLD",op.VLD},{"VLR",op.VLR},
        {"SLW",op.SLW},{"SLS",op.SLS},{"SLM",op.SLM},{"SLD",op.SLD},
        {"SLY",op.SLY},{"SLR",op.SLR},{"SLI",op.SLI}
    };
}
void jsonToSwOp(const json& j, FmSwOp& op) {
    auto g = [&](const char* k, uint8_t& v){ if(j.contains(k)) v=j[k].get<uint8_t>(); };
    g("VTL",op.VTL); g("VAR",op.VAR); g("VDR",op.VDR); g("VSL",op.VSL);
    g("VSR",op.VSR); g("VRR",op.VRR); g("VLD",op.VLD); g("VLR",op.VLR);
    g("SLW",op.SLW); g("SLS",op.SLS); g("SLM",op.SLM); g("SLD",op.SLD);
    g("SLY",op.SLY); g("SLR",op.SLR); g("SLI",op.SLI);
}

json toneLayerToJson(const ToneLayer& l) {
    return json{
        {"voice_patch_type",l.voicePatchType},
        {"hw_bank",l.hwBank},{"hw_prog",l.hwProg},
        {"note_range_lo",l.noteRangeLo},{"note_range_hi",l.noteRangeHi},
        {"transpose",l.transpose},
        {"volume_offset",l.volumeOffset},{"pan_offset",l.panOffset},
        {"enabled",l.enabled}
    };
}
ToneLayer jsonToToneLayer(const json& j) {
    ToneLayer l;
    if(j.contains("voice_patch_type")) l.voicePatchType = j["voice_patch_type"].get<uint8_t>();
    if(j.contains("hw_bank"))      l.hwBank      = j["hw_bank"].get<uint8_t>();
    if(j.contains("hw_prog"))      l.hwProg      = j["hw_prog"].get<uint8_t>();
    if(j.contains("note_range_lo"))l.noteRangeLo = j["note_range_lo"].get<uint8_t>();
    if(j.contains("note_range_hi"))l.noteRangeHi = j["note_range_hi"].get<uint8_t>();
    if(j.contains("transpose"))    l.transpose   = j["transpose"].get<int8_t>();
    if(j.contains("volume_offset"))l.volumeOffset= j["volume_offset"].get<int8_t>();
    if(j.contains("pan_offset"))   l.panOffset   = j["pan_offset"].get<int8_t>();
    if(j.contains("enabled"))      l.enabled     = j["enabled"].get<bool>();
    return l;
}

} // anonymous namespace

// ================================================================
//  HwBank JSON I/O
// ================================================================

bool PatchManager::loadHwBankJson(const std::filesystem::path& path,
                                   HwBankRegistry::VoiceGroup group, int bankNo,
                                   uint8_t voicePatchType)
{
    reportProgress("Loading HwBank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }
    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = hwReg_.getOrCreate(group, bankNo);
        if (j.contains("name")) bank.name = j["name"].get<std::string>();
        bank.filename = path.string();
        bank.voicePatchType = voicePatchType;
        if (j.contains("patches") && j["patches"].is_array()) {
            for (auto& entry : j["patches"]) {
                int prog = entry.value("prog", -1);
                if (prog < 0 || prog >= BANK_PROG_SIZE) continue;
                bank.set(prog, jsonToHwPatch(entry, bankNo, prog));
            }
        }
        FITOM_LOG_INFO("HwBank loaded: " << bank.name
            << " (" << path.filename().string() << ")"
            << " voicePatchType=0x" << std::hex << (int)voicePatchType);
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("HwBank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::saveHwBankJson(const std::filesystem::path& path,
                                   HwBankRegistry::VoiceGroup group, int bankNo) const
{
    const HwBank* bank = hwReg_.find(group, bankNo);
    if (!bank) return false;
    json patches = json::array();
    for (int i = 0; i < BANK_PROG_SIZE; ++i) {
        const auto& p = bank->get(i);
        if (!p.isValid()) continue;
        auto pj = hwPatchToJson(p);
        pj["prog"] = i;
        patches.push_back(pj);
    }
    json out = {{"name", bank->name}, {"patches", patches}};
    std::ofstream f(path);
    if (!f) return false;
    f << out.dump(2);
    return true;
}

// ================================================================
//  SwBank JSON I/O
// ================================================================

bool PatchManager::loadSwBankJson(const std::filesystem::path& path, int bankNo)
{
    reportProgress("Loading SwBank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }
    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = swReg_.getOrCreate(bankNo);
        if (j.contains("name")) bank.name = j["name"].get<std::string>();
        bank.filename = path.string();
        if (j.contains("patches") && j["patches"].is_array()) {
            for (auto& entry : j["patches"]) {
                int prog = entry.value("prog", -1);
                if (prog < 0 || prog >= BANK_PROG_SIZE) continue;
                SwPatch p;
                p.id = (uint32_t(bankNo) << 16) | prog;
                if (entry.contains("name")) {
                    std::string n = entry["name"].get<std::string>();
                    std::strncpy(p.name, n.c_str(), sizeof(p.name)-1);
                }
                if (entry.contains("sw")) {
                    const auto& sw = entry["sw"];
                    auto g8 = [&](const char* k, uint8_t& v){
                        if(sw.contains(k)) v=sw[k].get<uint8_t>();
                    };
                    g8("LWF",p.sw.LWF); g8("LFS",p.sw.LFS); g8("LFM",p.sw.LFM);
                    g8("LFD",p.sw.LFD); g8("LFR",p.sw.LFR); g8("LFI",p.sw.LFI);
                    g8("LDM",p.sw.LDM); g8("LDL",p.sw.LDL);
                }
                if (entry.contains("ops") && entry["ops"].is_array()) {
                    for (int i = 0; i < MAX_HW_OPS && i < (int)entry["ops"].size(); ++i)
                        jsonToSwOp(entry["ops"][i], p.swOp[i]);
                }
                bank.set(prog, p);
            }
        }
        FITOM_LOG_INFO("SwBank loaded: " << bank.name);
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("SwBank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::saveSwBankJson(const std::filesystem::path& path, int bankNo) const
{
    const SwBank* bank = swReg_.find(bankNo);
    if (!bank) return false;

    auto swOpToJson = [](const FmSwOp& op) {
        return json{
            {"VTL",op.VTL},{"VAR",op.VAR},{"VDR",op.VDR},{"VSL",op.VSL},
            {"VSR",op.VSR},{"VRR",op.VRR},{"VLD",op.VLD},{"VLR",op.VLR},
            {"SLW",op.SLW},{"SLS",op.SLS},{"SLM",op.SLM},{"SLD",op.SLD},
            {"SLY",op.SLY},{"SLR",op.SLR},{"SLI",op.SLI}
        };
    };

    json patches = json::array();
    for (int i = 0; i < BANK_PROG_SIZE; ++i) {
        const auto& p = bank->get(i);
        if (!p.isValid()) continue;
        json ops = json::array();
        for (int op = 0; op < MAX_HW_OPS; ++op) ops.push_back(swOpToJson(p.swOp[op]));
        patches.push_back(json{
            {"prog", i}, {"name", p.name},
            {"sw", json{
                {"LWF",p.sw.LWF},{"LFS",p.sw.LFS},{"LFM",p.sw.LFM},
                {"LFD",p.sw.LFD},{"LFR",p.sw.LFR},{"LFI",p.sw.LFI},
                {"LDM",p.sw.LDM},{"LDL",p.sw.LDL}
            }},
            {"ops", ops}
        });
    }
    json out = {{"name", bank->name}, {"patches", patches}};
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << out.dump(2);
    FITOM_LOG_INFO("SwBank saved: " << path.string());
    return true;
}

// ================================================================
//  PatchBank JSON I/O
// ================================================================

bool PatchManager::loadPatchBankJson(const std::filesystem::path& path, int bankNo)
{
    reportProgress("Loading PatchBank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }
    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = getPatchBank(bankNo);
        if (j.contains("name")) bank.name = j["name"].get<std::string>();
        bank.filename = path.string();
        if (j.contains("patches") && j["patches"].is_array()) {
            for (auto& entry : j["patches"]) {
                int prog = entry.value("prog", -1);
                if (prog < 0 || prog >= BANK_PROG_SIZE) continue;
                Patch p;
                p.id = (uint32_t(bankNo) << 16) | prog;
                if (entry.contains("name")) {
                    std::string n = entry["name"].get<std::string>();
                    std::strncpy(p.name, n.c_str(), sizeof(p.name)-1);
                }
                if (entry.contains("sw_bank")) p.swBank = entry["sw_bank"].get<uint8_t>();
                if (entry.contains("sw_prog")) p.swProg = entry["sw_prog"].get<uint8_t>();
                if (entry.contains("poly"))    p.poly   = entry["poly"].get<uint8_t>();
                if (entry.contains("layers") && entry["layers"].is_array()) {
                    int li = 0;
                    for (auto& lj : entry["layers"]) {
                        if (li >= MAX_TONE_LAYERS) break;
                        p.layers[li++] = jsonToToneLayer(lj);
                    }
                }
                bank.set(prog, p);
            }
        }
        FITOM_LOG_INFO("PatchBank loaded: " << bank.name);
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("PatchBank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::savePatchBankJson(const std::filesystem::path& path, int bankNo) const
{
    const PatchBank* bank = findPatchBank(bankNo);
    if (!bank) return false;
    json patches = json::array();
    for (int i = 0; i < BANK_PROG_SIZE; ++i) {
        const auto& p = bank->get(i);
        if (!p.isValid()) continue;
        json layers = json::array();
        for (const auto& l : p.layers) {
            if (l.isActive()) layers.push_back(toneLayerToJson(l));
        }
        patches.push_back(json{
            {"prog",i},{"name",p.name},
            {"sw_bank",p.swBank},{"sw_prog",p.swProg},
            {"poly",p.poly},{"layers",layers}
        });
    }
    json out = {{"name", bank->name}, {"patches", patches}};
    std::ofstream f(path);
    if (!f) return false;
    f << out.dump(2);
    return true;
}

// ================================================================
//  旧 INI 形式の互換ロード
//  FITOMCfg.cpp の ParseVoiceBank 相当
// ================================================================
bool PatchManager::loadHwBankLegacy(const std::filesystem::path& path,
                                     HwBankRegistry::VoiceGroup group, int bankNo)
{
    // 既存 CFITOMConfig::ParseVoiceBank で読んだ CFMBank を
    // HwBankRegistry に変換するブリッジ。
    // フェーズ6 でチップドライバ移行が完了したら本実装に差し替える。
    FITOM_LOG_WARN("loadHwBankLegacy: " << path.string()
        << " — legacy INI loader not yet fully implemented; use JSON format");
    return false;
}

// ================================================================
//  DrumBank JSON I/O
// ================================================================

bool PatchManager::loadDrumBankJson(const std::filesystem::path& path, int bankNo)
{
    reportProgress("Loading DrumBank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }
    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = drumReg_.getOrCreate(bankNo);
        if (j.contains("name")) bank.name = j["name"].get<std::string>();
        bank.filename = path.string();

        if (j.contains("patches") && j["patches"].is_array()) {
            for (const auto& pj : j["patches"]) {
                int prog = pj.value("prog", -1);
                if (prog < 0 || prog >= BANK_PROG_SIZE) continue;

                DrumPatch dp;
                dp.id = (uint32_t(bankNo) << 16) | prog;
                if (pj.contains("name")) {
                    std::string n = pj["name"].get<std::string>();
                    std::strncpy(dp.name, n.c_str(), sizeof(dp.name) - 1);
                }

                if (pj.contains("notes") && pj["notes"].is_array()) {
                    for (const auto& nj : pj["notes"]) {
                        int noteNo = nj.value("note", -1);
                        if (noteNo < 0 || noteNo >= 128) continue;

                        DrumNote& dn = dp.notes[noteNo];
                        dn.enabled    = true;
                        dn.patchBank  = nj.value("patch_bank", static_cast<uint8_t>(0));
                        dn.patchProg  = nj.value("patch_prog", static_cast<uint8_t>(0));
                        dn.playNote   = nj.value("play_note",  static_cast<uint8_t>(60));
                        dn.fineTune   = nj.value("fine_tune",  static_cast<int16_t>(0));
                        dn.fixedCh    = static_cast<int8_t>(nj.value("fixed_ch", -1));
                        dn.pan        = static_cast<int8_t>(nj.value("pan", 0));
                        dn.gateTime   = nj.value("gate_time",  static_cast<uint16_t>(0));
                        if (nj.contains("name")) {
                            std::string nn = nj["name"].get<std::string>();
                            std::strncpy(dn.name, nn.c_str(), sizeof(dn.name) - 1);
                        }
                    }
                }
                bank.set(prog, dp);
            }
        }
        FITOM_LOG_INFO("DrumBank loaded: " << bank.name
            << " (" << path.filename().string() << ")");
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("DrumBank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::saveDrumBankJson(const std::filesystem::path& path, int bankNo) const
{
    const DrumPatchBank* bank = nullptr;
    // DrumBankRegistry に find が必要 — getOrCreate の const 版を追加する方針で
    // ここでは drumReg_ を mutable にして対応
    auto& reg = const_cast<DrumBankRegistry&>(drumReg_);
    if (!reg.hasBank(bankNo)) return false;
    bank = &reg.getOrCreate(bankNo); // 存在確認済み

    json patches = json::array();
    for (int i = 0; i < BANK_PROG_SIZE; ++i) {
        const auto& dp = bank->get(i);
        if (!dp.isValid()) continue;

        json notes = json::array();
        for (int n = 0; n < 128; ++n) {
            const auto& dn = dp.notes[n];
            if (!dn.isActive()) continue;
            notes.push_back(json{
                {"note",       n},
                {"name",       dn.name},
                {"patch_bank", dn.patchBank},
                {"patch_prog", dn.patchProg},
                {"play_note",  dn.playNote},
                {"fine_tune",  dn.fineTune},
                {"fixed_ch",   dn.fixedCh},
                {"pan",        dn.pan},
                {"gate_time",  dn.gateTime}
            });
        }
        patches.push_back(json{
            {"prog",  i},
            {"name",  dp.name},
            {"notes", notes}
        });
    }
    json out = {{"name", bank->name}, {"patches", patches}};
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << out.dump(2);
    FITOM_LOG_INFO("DrumBank saved: " << path.string());
    return true;
}

// 旧 INI 形式のドラムバンク読み込み (移行期互換)
bool PatchManager::loadDrumBankLegacy(const std::filesystem::path& path, int bankNo)
{
    // 旧フォーマット:
    //   [Header]
    //   Type=RHYTHM
    //   BankName=Standard Kit
    //   [Bank]
    //   Note35=Bass Drum,OPN,0,0,24,0,0,-1
    //   ; フォーマット: name, DevName, BankNo, ProgNo, NoteNum:FineTune, Pan, GateTime, Ch
    reportProgress("Loading DrumBank (legacy): " + path.string());

    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }

    auto& bank = drumReg_.getOrCreate(bankNo);
    bank.filename = path.string();

    // INI パーサー (Config.cpp の ini::parse 相当)
    DrumPatch dp;
    dp.id = (uint32_t(bankNo) << 16) | 0; // prog=0 固定 (旧仕様)

    std::string section, line;
    while (std::getline(f, line)) {
        // BOM 除去・trim
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF)
            line = line.substr(3);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
            line.erase(line.begin());
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (section == "Header") {
            if (key == "BankName") {
                bank.name = val;
                std::strncpy(dp.name, val.c_str(), sizeof(dp.name) - 1);
            }
            continue;
        }
        if (section == "Bank" && key.substr(0, 4) == "Note") {
            int noteNo = std::stoi(key.substr(4));
            if (noteNo < 0 || noteNo >= 128) continue;

            // カンマ分割: name,DevName,BankNo,ProgNo,Note[:FineTune],Pan,Gate[,Ch]
            std::vector<std::string> parts;
            std::stringstream ss(val);
            std::string tok;
            while (std::getline(ss, tok, ',')) parts.push_back(tok);
            if (parts.size() < 7) continue;

            DrumNote& dn = dp.notes[noteNo];
            dn.enabled   = true;
            std::strncpy(dn.name, parts[0].c_str(), sizeof(dn.name) - 1);
            // 旧 INI の bank/prog は HwPatch 参照だが、
            // 変換ツールで対応する Patch を作成した上で patchBank/patchProg を設定する。
            // ここでは暫定的に patchBank=bank, patchProg=prog とする。
            dn.patchBank = static_cast<uint8_t>(std::stoi(parts[2]));
            dn.patchProg = static_cast<uint8_t>(std::stoi(parts[3]));
            // Note[:FineTune] の分解
            auto colon = parts[4].find(':');
            if (colon != std::string::npos) {
                dn.playNote = static_cast<uint8_t>(std::stoi(parts[4].substr(0, colon)));
                dn.fineTune = static_cast<int16_t>(std::stoi(parts[4].substr(colon + 1)));
            } else if (!parts[4].empty() && parts[4][0] == '#') {
                // #XXXX = raw fnum
                dn.playNote = 255;
                dn.fineTune = static_cast<int16_t>(std::stoi(parts[4].substr(1), nullptr, 16));
            } else {
                dn.playNote = static_cast<uint8_t>(std::stoi(parts[4]));
            }
            dn.pan      = static_cast<int8_t>(std::stoi(parts[5]));
            dn.gateTime = static_cast<uint16_t>(std::stoi(parts[6]));
            if (parts.size() > 7) {
                dn.fixedCh = static_cast<int8_t>(std::stoi(parts[7]));
            }

            FITOM_LOG_DEBUG("DrumBank legacy: note=" << noteNo
                << " '" << dn.name << "' patchBank=" << (int)dn.patchBank
                << " patchProg=" << (int)dn.patchProg);
        }
    }
    bank.set(0, dp);
    FITOM_LOG_INFO("DrumBank (legacy) loaded: " << bank.name);
    return true;
}

// ================================================================
//  SCC Wave Bank JSON I/O
// ================================================================
#include "fitom/SccWaveData.h"
#include <cmath>
#include <sstream>

bool PatchManager::loadSccWaveBankJson(const std::filesystem::path& path, int bankNo)
{
    reportProgress("Loading SCC Wave Bank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }
    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = sccWaveReg_.getOrCreate(bankNo);
        if (j.contains("name")) bank.name = j["name"].get<std::string>();
        bank.filename = path.string();

        if (j.contains("waves") && j["waves"].is_array()) {
            for (const auto& wj : j["waves"]) {
                int waveNo = wj.value("wave_no", -1);
                if (waveNo < 0 || waveNo >= SCC_MAX_WAVES) continue;

                SccWave wave;
                wave.waveNo = static_cast<uint8_t>(waveNo);
                if (wj.contains("name")) {
                    std::string n = wj["name"].get<std::string>();
                    std::strncpy(wave.name, n.c_str(), sizeof(wave.name) - 1);
                }

                const auto& data = wj["data"];
                if (data.is_array()) {
                    // 配列形式: [100, -100, ...]
                    for (int i = 0; i < SCC_WAVE_SIZE && i < (int)data.size(); ++i) {
                        wave.data[i] = static_cast<int8_t>(
                            std::clamp(data[i].get<int>(), -128, 127));
                    }
                } else if (data.is_string()) {
                    // 16進文字列形式: "64AB..."
                    std::string hex = data.get<std::string>();
                    for (int i = 0; i < SCC_WAVE_SIZE && i * 2 + 1 < (int)hex.size(); ++i) {
                        uint8_t byte = static_cast<uint8_t>(
                            std::stoi(hex.substr(i * 2, 2), nullptr, 16));
                        wave.data[i] = static_cast<int8_t>(byte);
                    }
                }
                bank.setWave(static_cast<uint8_t>(waveNo), wave);
            }
        }
        FITOM_LOG_INFO("SCC Wave Bank loaded: " << bank.name
            << " (" << path.filename().string() << ")");
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("SCC Wave Bank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::saveSccWaveBankJson(const std::filesystem::path& path, int bankNo) const
{
    if (!sccWaveReg_.hasBank(bankNo)) return false;
    auto& reg  = const_cast<SccWaveRegistry&>(sccWaveReg_);
    auto& bank = reg.getOrCreate(bankNo);

    json waves = json::array();
    for (int i = 0; i < SCC_MAX_WAVES; ++i) {
        const auto& wave = bank.getWave(static_cast<uint8_t>(i));
        // 全 0 は省略
        bool allZero = true;
        for (int8_t b : wave.data) if (b != 0) { allZero = false; break; }
        if (allZero && i > 1) continue;

        json arr = json::array();
        for (int8_t b : wave.data) arr.push_back(static_cast<int>(b));
        waves.push_back(json{
            {"wave_no", i},
            {"name",    wave.name},
            {"data",    arr}
        });
    }
    json out = {{"name", bank.name}, {"waves", waves}};
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << out.dump(2);
    return true;
}

// ================================================================
//  PCM Bank JSON I/O (PcmBankData.h)
// ================================================================
#include "fitom/PcmBankData.h"

// PcmBank::loadBinary の実装 (ヘッダに宣言、cpp に実体)
bool PcmBank::loadBinary(const std::filesystem::path& basePath)
{
    std::filesystem::path binPath = this->binPath;
    if (binPath.is_relative() && !basePath.empty())
        binPath = basePath / binPath;

    std::ifstream f(binPath, std::ios::binary);
    if (!f) {
        FITOM_LOG_ERR("PcmBank: cannot open bin: " << binPath.string());
        return false;
    }
    binData.assign(std::istreambuf_iterator<char>(f),
                   std::istreambuf_iterator<char>());
    FITOM_LOG_INFO("PcmBank: loaded " << binData.size()
        << " bytes from " << binPath.string());
    return true;
}

bool PatchManager::loadPcmBankJson(const std::filesystem::path& path, int bankNo)
{
    reportProgress("Loading PCM Bank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }

    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = pcmReg_.getOrCreate(bankNo);
        bank.name       = j.value("name",   "");
        bank.codec      = j.value("codec",  "adpcm-b");
        bank.sampleRate = j.value("sample_rate", 0u);
        bank.boundary   = j.value("boundary",    256u);
        bank.binPath    = j.value("bin_file",    "");

        const std::filesystem::path baseDir = path.parent_path();

        // adpcm_json フィールドがあれば adpcm_packer 出力 JSON を参照
        if (j.contains("adpcm_json") && j["adpcm_json"].is_string()) {
            std::filesystem::path ajPath = j["adpcm_json"].get<std::string>();
            if (ajPath.is_relative()) ajPath = baseDir / ajPath;

            std::ifstream aj(ajPath);
            if (!aj) {
                FITOM_LOG_WARN("PCM Bank: cannot open adpcm_json: " << ajPath.string()
                    << " — falling back to entries[]");
            } else {
                json aj_j = json::parse(aj, nullptr, true, true);
                // adpcm_packer の出力 JSON から entries を構築
                // entry_no は出現順で 0 から割り当てる
                if (aj_j.contains("entries") && aj_j["entries"].is_array()) {
                    uint8_t entryNo = 0;
                    for (const auto& ej : aj_j["entries"]) {
                        if (entryNo >= PCM_MAX_ENTRIES) break;
                        PcmEntry e;
                        e.entryNo    = entryNo;
                        std::string n = ej.value("name", "");
                        std::strncpy(e.name, n.c_str(), sizeof(e.name) - 1);
                        e.startOffset = ej.value("offset",      0u);
                        uint32_t paddedSize = ej.value("padded_size", 0u);
                        e.size        = ej.value("size",         0u);
                        e.paddedSize  = paddedSize;
                        e.endOffset   = e.startOffset + paddedSize - 1;
                        bank.setEntry(entryNo, e);
                        FITOM_LOG_DEBUG("PCM entry[" << (int)entryNo
                            << "]: '" << e.name
                            << "' offset=0x" << std::hex << e.startOffset
                            << " size=" << std::dec << e.paddedSize);
                        ++entryNo;
                    }
                }
                // codec / sample_rate は adpcm_json から上書きする (一致確認)
                if (aj_j.contains("codec")) {
                    std::string ac = aj_j["codec"].get<std::string>();
                    if (ac != bank.codec) {
                        FITOM_LOG_WARN("PCM Bank: codec mismatch: pcmbank='"
                            << bank.codec << "' adpcm_json='" << ac << "'");
                    }
                }
                if (aj_j.contains("sample_rate") && bank.sampleRate == 0) {
                    bank.sampleRate = aj_j["sample_rate"].get<uint32_t>();
                }
            }
        }

        // entries[] が直接記述されている場合はそちらを使う (adpcm_json より優先)
        if (j.contains("entries") && j["entries"].is_array()) {
            for (const auto& ej : j["entries"]) {
                int no = ej.value("entry_no", -1);
                if (no < 0 || no >= PCM_MAX_ENTRIES) continue;
                PcmEntry e;
                e.entryNo     = static_cast<uint8_t>(no);
                std::string n = ej.value("name", "");
                std::strncpy(e.name, n.c_str(), sizeof(e.name) - 1);
                e.startOffset = ej.value("start_offset", 0u);
                e.endOffset   = ej.value("end_offset",   0u);
                e.size        = ej.value("size",         0u);
                e.paddedSize  = ej.value("padded_size",  0u);
                bank.setEntry(static_cast<uint8_t>(no), e);
            }
        }

        // バイナリを読み込む
        bank.loadBinary(baseDir);

        FITOM_LOG_INFO("PCM Bank loaded: '" << bank.name
            << "' codec=" << bank.codec
            << " sr=" << bank.sampleRate
            << " (" << bank.binData.size() << " bytes)");
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("PCM Bank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::savePcmBankJson(const std::filesystem::path& path, int bankNo) const
{
    const PcmBank* bank = pcmReg_.find(bankNo);
    if (!bank) return false;

    json entries = json::array();
    for (int i = 0; i < PCM_MAX_ENTRIES; ++i) {
        const auto& e = bank->getEntry(static_cast<uint8_t>(i));
        if (!e.isValid()) continue;
        entries.push_back(json{
            {"entry_no",    i},
            {"name",        e.name},
            {"start_offset", e.startOffset},
            {"end_offset",  e.endOffset},
            {"size",        e.size},
            {"padded_size", e.paddedSize}
        });
    }
    json out = {
        {"name",        bank->name},
        {"codec",       bank->codec},
        {"sample_rate", bank->sampleRate},
        {"boundary",    bank->boundary},
        {"bin_file",    bank->binPath},
        {"entries",     entries}
    };
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << out.dump(2);
    return true;
}

} // namespace fitom
