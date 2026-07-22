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

TEST_CASE("FITOMConfig: pcm_banks[].group auto-routes the bank number and "
          "synthesizes named patches from entries[]", "[config][pcm]")
{
    // *.pcmbank.jsonにgroupを指定すると、①CFITOM::initDevices()が
    // 対応デバイスへこのバンク番号を自動的に割り当てられるようになり
    // (PcmBankRegistry::findBankNoForVoicePatchType())、②entries[]の
    // 各サンプルからProgram Change経由で選択できるnamed patchが
    // sampleRegistry()側に自動合成される(ADPCM-B/ADPCM-A/PCMD8はAWMと
    // 同じSampleZonePatchスキーマを使うため、hwRegistry()ではない。
    // *.samplezonebank.jsonの手書きは不要)。
    fs::path dir = fs::temp_directory_path() / "fitom_test_pcmbank_group";
    fs::create_directories(dir);

    json pcmbank = {
        {"name",  "test adpcm-a bank"},
        {"codec", "adpcm-a"},
        {"entries", json::array({
            {{"entry_no", 0}, {"name", "kick"}, {"root_note", 60},
             {"start_offset", 0}, {"end_offset", 255}, {"size", 200}, {"padded_size", 256}},
            {{"entry_no", 1}, {"name", "snare"},
             {"start_offset", 256}, {"end_offset", 511}, {"size", 200}, {"padded_size", 256}}
        })}
    };
    fs::path pcmbankPath = dir / "test.pcmbank.json";
    { std::ofstream f(pcmbankPath); f << pcmbank.dump(2); }

    json profile = {
        {"profile_name", "pcm group test"},
        {"devices",      json::array()},
        {"banks", {
            {"pcm_banks", json::array({
                {{"group", "ADPCMA"}, {"bank", 7}, {"file", "test.pcmbank.json"}}
            })}
        }}
    };
    fs::path profilePath = dir / "pcmgroup.profile.json";
    { std::ofstream f(profilePath); f << profile.dump(2); }

    fitom::FITOMConfig cfg;
    fitom::PatchManager pm;
    REQUIRE(cfg.loadProfile(profilePath, &pm));

    // ① バンク番号がvoicePatchTypeから逆引きできる
    CHECK(pm.pcmRegistry().findBankNoForVoicePatchType(VOICE_PATCH_ADPCMA) == 7);

    // ② entries[]からnamed patchが自動合成され、sampleRegistry()経由で見える
    CHECK(pm.sampleRegistry().listBankNumbers(VOICE_PATCH_ADPCMA) == std::vector<int>{7});
    const auto* sampleBank = pm.sampleRegistry().find(7);
    REQUIRE(sampleBank != nullptr);
    CHECK(sampleBank->voicePatchType == VOICE_PATCH_ADPCMA);

    const auto& p0 = sampleBank->get(0);
    REQUIRE(p0.isValid());
    CHECK(std::string(p0.name) == "kick");
    REQUIRE(p0.zones.size() == 1);
    CHECK(p0.zones[0].waveIndex == 0);
    CHECK(p0.zones[0].rootNote == 60); // entries[]で明示指定した値

    const auto& p1 = sampleBank->get(1);
    REQUIRE(p1.isValid());
    CHECK(std::string(p1.name) == "snare");
    REQUIRE(p1.zones.size() == 1);
    CHECK(p1.zones[0].waveIndex == 1);
    CHECK(p1.zones[0].rootNote == 69); // root_note省略時のデフォルト(A4)
}

TEST_CASE("FITOMConfig: pcm_banks[] without group keeps legacy behavior "
          "(no bank routing, no patch synthesis)", "[config][pcm]")
{
    // groupを省略した場合は後方互換のため、①バンク番号の自動解決対象には
    // ならず(findBankNoForVoicePatchTypeが-1)、②named patchの自動合成も
    // 行われない(波形データの登録のみ)。
    fs::path dir = fs::temp_directory_path() / "fitom_test_pcmbank_nogroup";
    fs::create_directories(dir);

    json pcmbank = {
        {"name",  "legacy adpcm-b bank"},
        {"codec", "adpcm-b"},
        {"entries", json::array({
            {{"entry_no", 0}, {"name", "kick"},
             {"start_offset", 0}, {"end_offset", 255}, {"size", 200}, {"padded_size", 256}}
        })}
    };
    fs::path pcmbankPath = dir / "legacy.pcmbank.json";
    { std::ofstream f(pcmbankPath); f << pcmbank.dump(2); }

    json profile = {
        {"profile_name", "pcm legacy test"},
        {"devices",      json::array()},
        {"banks", {
            {"pcm_banks", json::array({
                {{"bank", 3}, {"file", "legacy.pcmbank.json"}}
            })}
        }}
    };
    fs::path profilePath = dir / "pcmlegacy.profile.json";
    { std::ofstream f(profilePath); f << profile.dump(2); }

    fitom::FITOMConfig cfg;
    fitom::PatchManager pm;
    REQUIRE(cfg.loadProfile(profilePath, &pm));

    CHECK(pm.pcmRegistry().findBankNoForVoicePatchType(VOICE_PATCH_ADPCMB) == -1);
    const auto* bank = pm.pcmRegistry().find(3);
    REQUIRE(bank != nullptr);
    CHECK(bank->name == "legacy adpcm-b bank");

    CHECK(pm.sampleRegistry().find(3) == nullptr);
}

// OPNB(YM2610無印)はFM部がYM2612/YM2608と同じOPN2世代のコアを持つため、
// OPN(YM2203)ではなくOPN2側のVoicePatchTypeに分類され、mergeSpannableDevices()
// でOPN2/OPNA-FM/OPNBB-FMと束ねられる対象になるべき。(2026年7月、ステージング
// 環境からの指摘で発覚した誤分類の修正: 以前はOPN(0x10)側に誤分類されており、
// SSG/ADPCM-Aサブデバイスも自動生成されていなかった)
TEST_CASE("FITOMConfig: DEVICE_OPNB classifies as VOICE_PATCH_OPN2, not VOICE_PATCH_OPN",
          "[config]")
{
    CHECK(fitom::FITOMConfig::deviceTypeToVoicePatchType(DEVICE_OPNB) == VOICE_PATCH_OPN2);
    // 純粋なOPN(YM2203)は引き続きOPN側のまま
    CHECK(fitom::FITOMConfig::deviceTypeToVoicePatchType(DEVICE_OPN) == VOICE_PATCH_OPN);
}

// OPNB(YM2610無印)とOPNBB(YM2610B)は、SSG/ADPCM-A/ADPCM-Bのケーパビリティは
// 共通で、FMチャンネル数(COPNBの実効4ch vs COPNAの6ch)のみが異なる。
// そのためcomposite spec自体は両方ともFM+SSG+ADPCMA+ADPCMBの4サブデバイス
// になるべき(OPNBのFM部だけがbaseDeviceType==DEVICE_OPNBでCOPNBにルーティング
// される点が違い)。
TEST_CASE("FITOMConfig: DEVICE_OPNB and DEVICE_2610B composite specs are both "
          "FM+SSG+ADPCMA+ADPCMB", "[config]")
{
    std::vector<fitom::FITOMConfig::SubDeviceSpec> spec;
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_OPNB, spec));
    REQUIRE(spec.size() == 4);
    CHECK(spec[0].deviceType == DEVICE_OPNB);
    CHECK(spec[1].deviceType == DEVICE_SSG);
    CHECK(spec[2].deviceType == DEVICE_ADPCMA);
    CHECK(spec[3].deviceType == DEVICE_ADPCMB);

    std::vector<fitom::FITOMConfig::SubDeviceSpec> specB;
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_2610B, specB));
    REQUIRE(specB.size() == 4);
    CHECK(specB[0].deviceType == DEVICE_2610B);
    CHECK(specB[3].deviceType == DEVICE_ADPCMB);
}
