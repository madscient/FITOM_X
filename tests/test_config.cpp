// tests/test_config.cpp
// FITOMConfig のユニットテスト (Catch2 v3)

#include <catch2/catch_test_macros.hpp>
#include "fitom/Config.h"
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

TEST_CASE("FITOMConfig: audio_output parsed correctly", "[config]")
{
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
    CHECK(cfg.getAudioDevice()     == "Focusrite");
    CHECK(cfg.getAudioSampleRate() == 48000u);
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

TEST_CASE("FITOMConfig: system config defaults", "[config]")
{
    fitom::FITOMConfig cfg;
    CHECK(cfg.getMasterVolume() == 100);
    CHECK(cfg.getMasterPitch()  == 440.0);
    CHECK(cfg.getAudioSampleRate() == 48000u);
}
