// fitom/gui/bridge/FITOMBridge.cpp
// GUI ↔ FITOM コア ブリッジ実装(UIフレームワーク非依存)

#include "FITOMBridge.h"
#include "fitom/CFITOM.h"
#include "fitom/Config.h"
#include "fitom/PatchManager.h"
#include "fitom/Log.h"
#include "fitom/FnumUtils.h"

#include <nlohmann/json.hpp>
#include <filesystem>

namespace fs = std::filesystem;
using json   = nlohmann::json;

FITOMBridge& FITOMBridge::instance() {
    static FITOMBridge inst;
    return inst;
}

// ================================================================
//  初期化
// ================================================================

bool FITOMBridge::init(const std::string& systemConfPath,
                        const std::string& profilePath)
{
    if (initialized_) return true;

    // Boost.Log 初期化
    fitom::Log::init("info");

    FITOM_LOG_INFO("FITOMBridge initializing...");

    // システム設定とプロファイルをロード
    auto config   = std::make_unique<fitom::FITOMConfig>();
    auto patchMgr = std::make_unique<fitom::PatchManager>();

    if (!systemConfPath.empty()) {
        config->loadSystemConf(fs::path(systemConfPath));
    }
    if (!profilePath.empty()) {
        config->loadProfile(fs::path(profilePath));
        currentProfile_ = profilePath;
    }

    // タイマースレッドは MFC タイマーから onTimer() を呼ぶため不要
    // CFITOM を初期化
    int ret = fitom::CFITOM::instance().init(
        std::move(config), std::move(patchMgr));
    if (ret != 0) {
        FITOM_LOG_ERR("FITOMBridge: CFITOM init failed (" << ret << ")");
        return false;
    }

    // ステータスコールバックを転送
    fitom::CFITOM::instance().setStatusCallback([this](const std::string& msg) {
        if (statusCb_) statusCb_(msg);
    });

    initialized_ = true;
    FITOM_LOG_INFO("FITOMBridge initialized");
    return true;
}

void FITOMBridge::exit() {
    if (!initialized_) return;
    fitom::CFITOM::instance().exit();
    initialized_ = false;
    FITOM_LOG_INFO("FITOMBridge exited");
}

// ================================================================
//  プロファイル切り替え
// ================================================================

bool FITOMBridge::loadProfile(const std::string& path)
{
    auto& fitom = fitom::CFITOM::instance();
    fitom.allNoteOff();

    auto& cfg = fitom.getConfig();
    bool ok = cfg.loadProfile(fs::path(path));
    if (ok) {
        currentProfile_ = path;
        FITOM_LOG_INFO("Profile loaded: " << path);
    }
    return ok;
}

std::string FITOMBridge::currentProfilePath() const {
    return currentProfile_;
}

// ================================================================
//  デバイス情報
// ================================================================

std::vector<FITOMDeviceInfo> FITOMBridge::getDevices() const
{
    std::vector<FITOMDeviceInfo> result;
    if (!initialized_) return result;

    auto& cfg = fitom::CFITOM::instance().getConfig();
    int n = cfg.getDeviceCount();
    for (int i = 0; i < n; ++i) {
        FITOMDeviceInfo info;
        info.index       = i;
        info.label       = cfg.getDeviceLabel(i);
        info.deviceType  = cfg.getDeviceType(i);
        // 実機/エミュレータの区別は、HWプラグイン(IHWPlugin)経由に
        // 一元化された現行アーキテクチャでは、この階層からは判定でき
        // ない(どちらであるかはロードされたHWプラグインDLLの内部
        // 実装に委ねられている)。旧`FmEnginePort`型による判定は
        // 同型が廃止されたため撤去した。判定手段が必要になった場合は
        // IHWPlugin側にケイパビリティ問い合わせAPIを追加検討する。
        info.isEmulator  = false;

        auto* dev = cfg.getDevice(i);
        if (dev) {
            info.descriptor = dev->getDescriptor();
            info.chCount    = dev->getChCount();
        } else {
            info.descriptor = "N/A";
            info.chCount    = 0;
        }
        result.push_back(info);
    }
    return result;
}

std::vector<FITOMMidiInfo> FITOMBridge::getMidiInputs() const
{
    std::vector<FITOMMidiInfo> result;
    if (!initialized_) return result;

    auto& cfg = fitom::CFITOM::instance().getConfig();
    int n = cfg.getMidiInputCount();
    for (int i = 0; i < n; ++i) {
        FITOMMidiInfo info;
        info.index = i;
        info.name  = cfg.getMidiInputName(i);
        result.push_back(info);
    }
    return result;
}

// ================================================================
//  パッチ一覧
// ================================================================

std::vector<FITOMPatchInfo> FITOMBridge::getPatches(int bankNo) const
{
    std::vector<FITOMPatchInfo> result;
    if (!initialized_) return result;

    auto& pm = fitom::CFITOM::instance().getPatchManager();
    const auto* bank = pm.findPatchBank(bankNo);
    if (!bank) return result;

    for (int i = 0; i < fitom::BANK_PROG_SIZE; ++i) {
        const auto& p = bank->get(i);
        if (!p.isValid()) continue;
        FITOMPatchInfo info;
        info.bank       = bankNo;
        info.prog       = i;
        info.name       = p.name;
        info.layerCount = p.activeLayerCount();
        result.push_back(info);
    }
    return result;
}

std::vector<std::string> FITOMBridge::getPatchBankNames() const
{
    // TODO: PatchManager に bank 名列挙を追加
    return {};
}

// ================================================================
//  音色エディタ連携
// ================================================================

std::string FITOMBridge::getHwPatchJson(int bankNo, int prog) const
{
    if (!initialized_) return "{}";
    auto& pm = fitom::CFITOM::instance().getPatchManager();
    // TODO: HwBankRegistry から HwPatch を取得して JSON 変換
    return "{}";
}

bool FITOMBridge::setHwPatchJson(int bankNo, int prog, const std::string& js)
{
    if (!initialized_) return false;
    try {
        json j = json::parse(js);
        // TODO: json → HwPatch 変換 → HwBankRegistry::set()
        return true;
    } catch (...) { return false; }
}

std::string FITOMBridge::getSwPatchJson(int bankNo, int prog) const
{
    return "{}"; // TODO
}

bool FITOMBridge::setSwPatchJson(int bankNo, int prog, const std::string& js)
{
    return false; // TODO
}

// ================================================================
//  マスターボリューム / ピッチ
// ================================================================

void    FITOMBridge::setMasterVolume(uint8_t vol) {
    if (initialized_) fitom::CFITOM::instance().setMasterVolume(vol);
}
uint8_t FITOMBridge::getMasterVolume() const {
    return initialized_ ? fitom::CFITOM::instance().getMasterVolume() : 100;
}

void   FITOMBridge::setMasterPitch(double hz) {
    fitom::FnumRegistry::instance().setMasterPitch(hz);
}
double FITOMBridge::getMasterPitch() const {
    return fitom::FnumRegistry::instance().getMasterPitch();
}

// ================================================================
//  直接操作
// ================================================================

void FITOMBridge::allNoteOff()   { if (initialized_) fitom::CFITOM::instance().allNoteOff(); }
void FITOMBridge::resetAllCtrl() { if (initialized_) fitom::CFITOM::instance().resetAllCtrl(); }

// ================================================================
//  タイマーコールバック (MFC WM_TIMER から毎 1ms 呼ぶ)
// ================================================================

void FITOMBridge::onTimer(uint32_t tick) {
    if (initialized_) fitom::CFITOM::instance().timerCallback(tick);
}

// ================================================================
//  オーディオデバイス列挙
// ================================================================

std::vector<std::string> FITOMBridge::enumerateAudioDevices() const {
    // オーディオデバイス列挙は fitom_fmhwif DLL が担当。
    // fitom_core は RtAudio に依存しないため、ここでは空リストを返す。
    // GUI はプロファイルの audio_api 設定で API を選択する。
    return {};
}

// ================================================================
//  バンクファイル I/O
// ================================================================

bool FITOMBridge::loadHwBankFile(const std::string& path, int bankNo) {
    if (!initialized_) return false;
    auto& pm = fitom::CFITOM::instance().getPatchManager();
    return pm.loadHwBankJson(fs::path(path), 0, bankNo);
}

bool FITOMBridge::saveHwBankFile(const std::string& path, int bankNo) const {
    if (!initialized_) return false;
    auto& pm = fitom::CFITOM::instance().getPatchManager();
    return pm.saveHwBankJson(fs::path(path), 0, bankNo);
}

bool FITOMBridge::loadPatchBankFile(const std::string& path, int bankNo) {
    if (!initialized_) return false;
    auto& pm = fitom::CFITOM::instance().getPatchManager();
    return pm.loadPatchBankJson(fs::path(path), bankNo);
}

bool FITOMBridge::savePatchBankFile(const std::string& path, int bankNo) const {
    if (!initialized_) return false;
    auto& pm = fitom::CFITOM::instance().getPatchManager();
    return pm.savePatchBankJson(fs::path(path), bankNo);
}

void FITOMBridge::setStatusCallback(StatusCb cb) {
    statusCb_ = std::move(cb);
}
