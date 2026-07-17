// tests/test_config.cpp
// FITOMConfig のユニットテスト (Catch2 v3)

#include <catch2/catch_test_macros.hpp>
#include "fitom/Config.h"
#include "fitom/PatchManager.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

// テスト用の一時ファイルを作成するヘルパー
static fs::path writeTempJson(const std::string& name, const json& j)
{
    fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p);
    f << j.dump(2);
    return p;
}

TEST_CASE("FITOMConfig: load minimal profile", "[config]")
{
    json profile = {
        {"profile_name", "test"},
        {"midi_inputs",  json::array({"MIDI Keyboard"})},
        {"devices",      json::array()}
    };
    fs::path p = writeTempJson("fitom_test_minimal.profile.json", profile);

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));
    CHECK(cfg.getMidiInputCount() == 1);
    CHECK(cfg.getMidiInputName(0) == "MIDI Keyboard");
    CHECK(cfg.getDeviceCount() == 0);
}

TEST_CASE("FITOMConfig: audio_output field is ignored (obsolete, removed feature)", "[config]")
{
    // audio_output は廃止済み (FmEngine直接制御パス自体が廃止されたため)。
    // 既存プロファイルにこのフィールドが残っていても単純に無視され、
    // エラーにはならないことを確認する (後方互換性)。
    // 音声出力は HW プラグイン (FitomEmuIF/FitomHwIF 等) 自身の責務であり、
    // FITOM_X本体は一切関与しない。
    json profile = {
        {"profile_name", "test"},
        {"audio_output", {
            {"device",      "Focusrite"},
            {"sample_rate", 48000}
        }},
        {"devices", json::array()}
    };
    fs::path p = writeTempJson("fitom_test_audio.profile.json", profile);

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));
    CHECK(cfg.getDeviceCount() == 0); // audio_outputの内容に関わらず正常に読み込める
}

TEST_CASE("FITOMConfig: channel_map field is ignored (obsolete, removed feature)", "[config]")
{
    // channel_map は廃止済み。既存プロファイルにこのフィールドが残っていても
    // 単純に無視され、エラーにはならないことを確認する (後方互換性)。
    // GM準拠の既定動作 (MIDI ch10固定リズム、ポリフォニーはデバイス依存で
    // 動的決定) は CFITOM/CInstCh 側の責務であり、Config自体はこの
    // フィールドを一切パースしない。
    json profile = {
        {"profile_name", "test"},
        {"devices", json::array()},
        {"channel_map", json::array({
            {{"midi_ch", 1},  {"device_index", 0}, {"poly", 4}},
            {{"midi_ch", 10}, {"device_index", 1}, {"poly", 1}}
        })}
    };
    fs::path p = writeTempJson("fitom_test_chmap.profile.json", profile);

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));
    CHECK(cfg.getDeviceCount() == 0); // channel_mapの内容に関わらず正常に読み込める
}

TEST_CASE("FITOMConfig: missing profile file returns false", "[config]")
{
    fitom::FITOMConfig cfg;
    CHECK_FALSE(cfg.loadProfile("/nonexistent/path/does_not_exist.json"));
}

TEST_CASE("FITOMConfig: banks.*[].file resolves relative to the profile's own directory, not CWD",
          "[config]")
{
    // プロファイルとバンクファイルを、プロセスのCWD (テストランナーの
    // 実行ディレクトリ) とは無関係な専用サブディレクトリに置く。
    // baseDirがCWDのままなら "hwbank.json" は見つからずロードに失敗し、
    // プロファイル自身のディレクトリを起点にして初めて成功する。
    fs::path dir = fs::temp_directory_path() / "fitom_test_profile_reldir";
    fs::create_directories(dir);

    json hwbank = {
        {"name", "reldir test bank"},
        {"patches", json::array()}
    };
    fs::path hwbankPath = dir / "child.hwbank.json";
    { std::ofstream f(hwbankPath); f << hwbank.dump(2); }

    json profile = {
        {"profile_name", "reldir test"},
        {"devices",      json::array()},
        {"banks", {
            {"hw_banks", json::array({
                {{"group", "OPN"}, {"bank", 0}, {"file", "child.hwbank.json"}}
            })}
        }}
    };
    fs::path profilePath = dir / "reldir.profile.json";
    { std::ofstream f(profilePath); f << profile.dump(2); }

    fitom::FITOMConfig cfg;
    fitom::PatchManager pm;
    REQUIRE(cfg.loadProfile(profilePath, &pm));

    uint8_t voicePatchType = fitom::FITOMConfig::stringToVoicePatchType("OPN");
    uint32_t group = fitom::FITOMConfig::voicePatchTypeToVoiceGroup(voicePatchType);
    const auto* bank = pm.hwRegistry().find(group, 0);
    REQUIRE(bank != nullptr);
    CHECK(bank->name == "reldir test bank");
}

TEST_CASE("FITOMConfig: system config defaults", "[config]")
{
    fitom::FITOMConfig cfg;
    CHECK(cfg.getMasterVolume() == 100);
    CHECK(cfg.getMasterPitch()  == 440.0);
}
