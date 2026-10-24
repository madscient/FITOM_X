// tests/test_master_volume.cpp
// マスターボリュームを、HWプラグインの任意関数 HWPlugin_SetMasterVolume で
// デバイスごとに渡すことの回帰テスト。チップドライバには掛けない。
// 検証用HWプラグインが受け取った値は、テスト専用のエクスポート
// StubHWPlugin_GetMasterVolume で読み出す。

#include <catch2/catch_test_macros.hpp>

#include "stub_cfitom.h"
#include "fitom/HWPort.h"
#include "fitom/PluginLoader.h"
#include <fitom/IHWPlugin.h>

#include <cstdint>

using namespace fitom;

namespace {

int stubMasterVolume(const char* chip)
{
    using PFN = int (FITOM_HWP_CALL*)(const char*, int);
    // CFITOMが既にロードしている同じDLLを開くので、同じモジュールの状態を読む。
    static PluginLoader loader = PluginLoader::load(FITOM_TEST_HWPLUGIN_PARTS);
    auto fn = loader.symRequired<PFN>("StubHWPlugin_GetMasterVolume");
    return fn(chip, 0);
}

// 共有フィクスチャの状態を、テストの終わりに必ず元へ戻す。
struct RestoreMasterVolume {
    ~RestoreMasterVolume() { stub_cfitom::instance().sendMasterVolume(stub_cfitom::kMasterVolume); }
};

} // namespace

TEST_CASE("Master volume: the profile value is passed to every HW plugin device at init",
          "[master_volume]")
{
    stub_cfitom::instance();
    CHECK(stubMasterVolume("OPM")  == stub_cfitom::kMasterVolume);
    CHECK(stubMasterVolume("OPL2") == stub_cfitom::kMasterVolume);
}

TEST_CASE("Master volume: a change from the GUI reaches every HW plugin device",
          "[master_volume]")
{
    CFITOM& fitom = stub_cfitom::instance();
    RestoreMasterVolume restore;

    fitom.sendMasterVolume(64);
    CHECK(stubMasterVolume("OPM")  == 64);
    CHECK(stubMasterVolume("OPL2") == 64);
    CHECK(fitom.getMasterVolume() == 64);
}

TEST_CASE("Master volume: a Universal Real Time SysEx reaches every HW plugin device",
          "[master_volume]")
{
    CFITOM& fitom = stub_cfitom::instance();
    RestoreMasterVolume restore;

    const uint8_t sysex[] = { 0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x00, 0x50, 0xF7 };
    fitom.receiveMpuByte(0, sysex, sizeof(sysex), 0);
    fitom.pollingCallback();
    CHECK(stubMasterVolume("OPM")  == 0x50);
    CHECK(stubMasterVolume("OPL2") == 0x50);
}

TEST_CASE("Master volume: a plugin that does not export HWPlugin_SetMasterVolume is skipped",
          "[master_volume]")
{
    namespace fs = std::filesystem;
    nlohmann::json profile = {
        {"profile_name", "no master volume"},
        {"hw_plugins",   nlohmann::json::array({ {{"name", "stub"}, {"dll", FITOM_TEST_HWPLUGIN_NO_PARTS}} })},
        {"devices",      nlohmann::json::array({ stub_cfitom::stubDevice("OPM") })}
    };
    fs::path p = fs::temp_directory_path() / "fitom_test_master_volume_absent.profile.json";
    stub_cfitom::writeJson(p, profile);

    FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));
    auto* port = dynamic_cast<HWPort*>(cfg.getDevicePort(0));
    REQUIRE(port != nullptr);
    port->setMasterVolume(10);   // 呼べる関数が無いので何もしない(落ちない)
}
