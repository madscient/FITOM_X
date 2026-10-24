#pragma once
// tests/stub_cfitom.h
// CFITOM(シングルトン)を検証用HWプラグイン(tests/stub_hwplugin)で初期化する
// テスト共通のフィクスチャ。CFITOMは1プロセスで1回しか初期化できないので、
// CFITOMを使うテストは全てこのプロファイルを共有する。
//
// 構成: OPM・OPL2を1台ずつ(どちらも検証用プラグイン)、master_volume=90。
// 検証用プラグインはレジスタ書き込みを記録しないので、判定はデバイスの
// チャンネル状態(ChState)か、検証用プラグインのテスト専用エクスポートで行う。

#include <catch2/catch_test_macros.hpp>

#include "fitom/CFITOM.h"
#include "fitom/Config.h"
#include "fitom/PatchManager.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <memory>

namespace stub_cfitom {

constexpr int     kOpmDevice    = 0;
constexpr int     kOpl2Device   = 1;
constexpr uint8_t kBank         = 1;
constexpr uint8_t kMasterVolume = 90;

inline void writeJson(const std::filesystem::path& p, const nlohmann::json& j)
{
    std::ofstream f(p);
    f << j.dump(2);
}

inline nlohmann::json stubDevice(const char* chip)
{
    return { {"if", "HW"}, {"plugin", "stub"}, {"type", "FMHWIF"}, {"engine", "Stub"},
             {"chip", chip}, {"index", 0}, {"label", chip}, {"clock", 3579545} };
}

// 音色バンク: OPM bank1 prog0=ソフトウェアLFOつき(sw bank0/prog0)、prog1=LFOなし。
// OPL2 bank1 prog0。
inline fitom::CFITOM& instance()
{
    using nlohmann::json;
    namespace fs = std::filesystem;

    static bool initialized = false;
    fitom::CFITOM& fitom = fitom::CFITOM::instance();
    if (initialized) return fitom;

    fs::path dir = fs::temp_directory_path() / "fitom_test_stub_cfitom";
    fs::create_directories(dir);

    writeJson(dir / "opm.hwbank.json", {
        {"name", "OPM stub"},
        {"patches", json::array({
            {{"prog", 0}, {"name", "OPM A (LFO)"}, {"sw_bank", 0}, {"sw_prog", 0}},
            {{"prog", 1}, {"name", "OPM B"}}
        })}
    });
    writeJson(dir / "opl2.hwbank.json", {
        {"name", "OPL2 stub"},
        {"patches", json::array({ {{"prog", 0}, {"name", "OPL2 C"}} })}
    });
    writeJson(dir / "lfo.swbank.json", {
        {"name", "LFO"},
        {"patches", json::array({
            {{"prog", 0}, {"name", "vibrato"},
             {"sw", {{"LFR", 64}, {"LFD", 0}, {"LFI", 0}, {"LWF", 6}, {"LFM", 0},
                     {"depth_cents", 50}}}}
        })}
    });

    json profile = {
        {"profile_name",  "stub CFITOM"},
        {"master_volume", kMasterVolume},
        {"hw_plugins",    json::array({ {{"name", "stub"}, {"dll", FITOM_TEST_HWPLUGIN_PARTS}} })},
        {"devices",       json::array({ stubDevice("OPM"), stubDevice("OPL2") })},
        {"banks", {
            {"hw_banks", json::array({
                {{"group", "OPM"},  {"bank", kBank}, {"file", "opm.hwbank.json"}},
                {{"group", "OPL2"}, {"bank", kBank}, {"file", "opl2.hwbank.json"}}
            })},
            {"sw_banks", json::array({ {{"bank", 0}, {"file", "lfo.swbank.json"}} })}
        }}
    };
    fs::path profilePath = dir / "stub_cfitom.profile.json";
    writeJson(profilePath, profile);

    auto cfg = std::make_unique<fitom::FITOMConfig>();
    auto pm  = std::make_unique<fitom::PatchManager>();
    REQUIRE(cfg->loadProfile(profilePath, pm.get()));
    REQUIRE(cfg->getDeviceCount() == 2);
    fitom.init(std::move(cfg), std::move(pm));
    initialized = true;
    return fitom;
}

} // namespace stub_cfitom
