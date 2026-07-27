// tests/test_config.cpp
// FITOMConfig のユニットテスト (Catch2 v3)

#include <catch2/catch_test_macros.hpp>
#include "fitom/Config.h"
#include "fitom/PatchManager.h"
#include "fitom/Sf2BankRegistry.h"
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
        })},
        {"swpatches", json::array({
            {{"entry_no", 0}, {"sw_bank", 2}, {"sw_prog", 5}}
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
    CHECK(p0.zones[0].swBank == 2);    // swpatches[]から反映
    CHECK(p0.zones[0].swProg == 5);

    const auto& p1 = sampleBank->get(1);
    REQUIRE(p1.isValid());
    CHECK(std::string(p1.name) == "snare");
    REQUIRE(p1.zones.size() == 1);
    CHECK(p1.zones[0].waveIndex == 1);
    CHECK(p1.zones[0].rootNote == 69); // root_note省略時のデフォルト(A4)
    CHECK(p1.zones[0].swBank == -1);   // swpatches[]未指定
    CHECK(p1.zones[0].swProg == -1);
}

TEST_CASE("SampleZonePatch::resolveZone: key/velocity range matching with fallback",
          "[patchdata][samplezone]")
{
    fitom::SampleZonePatch patch;
    fitom::SampleZone low;
    low.keyMin = 0; low.keyMax = 59; low.velMin = 0; low.velMax = 127;
    low.waveIndex = 100;
    fitom::SampleZone high;
    high.keyMin = 60; high.keyMax = 127; high.velMin = 0; high.velMax = 63;
    high.waveIndex = 101;
    fitom::SampleZone highLoud;
    highLoud.keyMin = 60; highLoud.keyMax = 127; highLoud.velMin = 64; highLoud.velMax = 127;
    highLoud.waveIndex = 102;
    patch.zones = { low, high, highLoud };

    SECTION("キー範囲による選択") {
        const auto* z = patch.resolveZone(40, 100);
        REQUIRE(z != nullptr);
        CHECK(z->waveIndex == 100);
    }

    SECTION("ベロシティレイヤーによる選択") {
        const auto* zSoft = patch.resolveZone(72, 30);
        REQUIRE(zSoft != nullptr);
        CHECK(zSoft->waveIndex == 101);

        const auto* zLoud = patch.resolveZone(72, 100);
        REQUIRE(zLoud != nullptr);
        CHECK(zLoud->waveIndex == 102);
    }

    SECTION("該当ゾーンが無い場合はzones[0]にフォールバック") {
        // note=200相当は呼び出し側の責務外だが、境界外velで検証する
        // (uint8_tの範囲内で「どのゾーンにも一致しない」ケースを作る)
        fitom::SampleZonePatch narrow;
        fitom::SampleZone only;
        only.keyMin = 10; only.keyMax = 10; only.velMin = 10; only.velMax = 10;
        only.waveIndex = 5;
        narrow.zones = { only };
        const auto* z = narrow.resolveZone(0, 0); // 一致しないがフォールバック
        REQUIRE(z != nullptr);
        CHECK(z->waveIndex == 5);
    }

    SECTION("zonesが空ならnullptr") {
        fitom::SampleZonePatch empty;
        CHECK(empty.resolveZone(60, 100) == nullptr);
    }
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

// ================================================================
//  SF2直行パス (docs/sf2-fluidsynth-integration.md参照、2026年7月新設)
// ================================================================

TEST_CASE("Sf2BankRegistry: resolves CC#32 to {soundfont_index, sf2_bank} and "
          "dedups soundfont files by first-seen order", "[config][sf2]")
{
    json arr = json::array({
        {{"bank", 0}, {"file", "orchestral.sf2"}, {"sf2_bank", 0}},
        {{"bank", 1}, {"file", "orchestral.sf2"}, {"sf2_bank", 8}},   // 同一fileを再利用
        {{"bank", 2}, {"file", "drums.sf2"},      {"sf2_bank", 128}},
    });

    fitom::Sf2BankRegistry reg;
    reg.load(arr, "/base/dir");

    REQUIRE_FALSE(reg.empty());
    REQUIRE(reg.soundfontFiles().size() == 2); // orchestral.sf2/drums.sf2の2ファイルのみ

    fitom::Sf2BankRegistry::Resolved r0, r1, r2;
    REQUIRE(reg.resolve(0, r0));
    REQUIRE(reg.resolve(1, r1));
    REQUIRE(reg.resolve(2, r2));

    CHECK(r0.soundfontIndex == r1.soundfontIndex); // 同一fileは同じindex
    CHECK(r0.soundfontIndex != r2.soundfontIndex);
    CHECK(r0.sf2Bank == 0);
    CHECK(r1.sf2Bank == 8);
    CHECK(r2.sf2Bank == 128); // パーカッションバンク(127を超える特別な値)

    fitom::Sf2BankRegistry::Resolved unresolved;
    CHECK_FALSE(reg.resolve(99, unresolved)); // 対応エントリなし
}

TEST_CASE("Sf2BankRegistry: invalid entries (out-of-range bank/sf2_bank, empty file) "
          "are skipped", "[config][sf2]")
{
    json arr = json::array({
        {{"bank", 128}, {"file", "a.sf2"}, {"sf2_bank", 0}},   // bank範囲外(>127)
        {{"bank", 0},   {"file", ""},      {"sf2_bank", 0}},   // file空
        {{"bank", 1},   {"file", "b.sf2"}, {"sf2_bank", 129}}, // sf2_bank範囲外(>128)
    });

    fitom::Sf2BankRegistry reg;
    reg.load(arr, "/base/dir");
    CHECK(reg.empty());
}

TEST_CASE("FITOMConfig: sf2_banks without a chip=\"SF2\" device fails to load "
          "(no dispatch destination)", "[config][sf2]")
{
    json profile = {
        {"profile_name", "test"},
        {"devices",      json::array()},
        {"banks", {
            {"sf2_banks", json::array({
                {{"bank", 0}, {"file", "a.sf2"}, {"sf2_bank", 0}}
            })}
        }}
    };
    fs::path p = writeTempJson("fitom_test_sf2_banks_no_device.profile.json", profile);

    fitom::FITOMConfig cfg;
    CHECK_FALSE(cfg.loadProfile(p));
}

TEST_CASE("FITOMConfig: sf2_channel_windows without a chip=\"SF2\" device fails to load",
          "[config][sf2]")
{
    json profile = {
        {"profile_name", "test"},
        {"devices",      json::array()},
        {"sf2_channel_windows", json::array({
            {{"mpu", 0}, {"ch", 12}, {"fluidsynth_chan", 0}}
        })}
    };
    fs::path p = writeTempJson("fitom_test_sf2_windows_no_device.profile.json", profile);

    fitom::FITOMConfig cfg;
    CHECK_FALSE(cfg.loadProfile(p));
}

TEST_CASE("FITOMConfig: more than one devices[] entry with chip=\"SF2\" fails to load "
          "(fluid_synth_t is shared across all MPUs, dispatch must be unambiguous)",
          "[config][sf2]")
{
    // pluginが未登録のため実際のHWPort生成は失敗するが、devices[]の記述
    // レベルでの矛盾(chip=\"SF2\"が2つ)は宣言のJSONを直接数えて検出する
    // ため、プラグインの実在有無とは無関係に検証できる。
    json profile = {
        {"profile_name", "test"},
        {"devices", json::array({
            {{"if", "HW"}, {"chip", "SF2"}, {"plugin", "NoSuchPlugin"}},
            {{"if", "HW"}, {"chip", "SF2"}, {"plugin", "NoSuchPlugin"}}
        })}
    };
    fs::path p = writeTempJson("fitom_test_sf2_duplicate_device.profile.json", profile);

    fitom::FITOMConfig cfg;
    CHECK_FALSE(cfg.loadProfile(p));
}

TEST_CASE("FITOMConfig: sf2_channel_windows validates duplicate (mpu,ch), duplicate "
          "fluidsynth_chan, out-of-range values, and >16 entries", "[config][sf2]")
{
    SECTION("同一(mpu,ch)の重複") {
        json profile = {
            {"profile_name", "test"},
            {"devices",      json::array()},
            {"sf2_channel_windows", json::array({
                {{"mpu", 0}, {"ch", 12}, {"fluidsynth_chan", 0}},
                {{"mpu", 0}, {"ch", 12}, {"fluidsynth_chan", 1}}
            })}
        };
        fs::path p = writeTempJson("fitom_test_sf2_win_dup_mpuch.profile.json", profile);
        fitom::FITOMConfig cfg;
        CHECK_FALSE(cfg.loadProfile(p));
    }

    SECTION("fluidsynth_chanの重複") {
        json profile = {
            {"profile_name", "test"},
            {"devices",      json::array()},
            {"sf2_channel_windows", json::array({
                {{"mpu", 0}, {"ch", 12}, {"fluidsynth_chan", 0}},
                {{"mpu", 0}, {"ch", 13}, {"fluidsynth_chan", 0}}
            })}
        };
        fs::path p = writeTempJson("fitom_test_sf2_win_dup_chan.profile.json", profile);
        fitom::FITOMConfig cfg;
        CHECK_FALSE(cfg.loadProfile(p));
    }

    SECTION("範囲外の値") {
        json profile = {
            {"profile_name", "test"},
            {"devices",      json::array()},
            {"sf2_channel_windows", json::array({
                {{"mpu", 0}, {"ch", 12}, {"fluidsynth_chan", 16}} // 0-15の範囲外
            })}
        };
        fs::path p = writeTempJson("fitom_test_sf2_win_out_of_range.profile.json", profile);
        fitom::FITOMConfig cfg;
        CHECK_FALSE(cfg.loadProfile(p));
    }

    SECTION("16エントリ超過") {
        json arr = json::array();
        for (int i = 0; i < 17; ++i) {
            arr.push_back({{"mpu", 0}, {"ch", i % 16}, {"fluidsynth_chan", i % 16}});
        }
        json profile = {
            {"profile_name", "test"},
            {"devices",      json::array()},
            {"sf2_channel_windows", arr}
        };
        fs::path p = writeTempJson("fitom_test_sf2_win_too_many.profile.json", profile);
        fitom::FITOMConfig cfg;
        CHECK_FALSE(cfg.loadProfile(p));
    }
}

TEST_CASE("FITOMConfig: valid sf2_channel_windows load into getSf2ChannelWindows() "
          "when a chip=\"SF2\" device is declared", "[config][sf2]")
{
    // pluginは未登録のため実際のISoundDevice/HWPort生成は行われないが、
    // devices[]記述レベルではchip=\"SF2\"が1つ存在するため、
    // sf2_channel_windowsの検証・保持自体は正常に完了する。
    json profile = {
        {"profile_name", "test"},
        {"devices", json::array({
            {{"if", "HW"}, {"chip", "SF2"}, {"plugin", "NoSuchPlugin"}}
        })},
        {"sf2_channel_windows", json::array({
            {{"mpu", 0}, {"ch", 12}, {"fluidsynth_chan", 0}},
            {{"mpu", 2}, {"ch", 9},  {"fluidsynth_chan", 2}}
        })}
    };
    fs::path p = writeTempJson("fitom_test_sf2_windows_valid.profile.json", profile);

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));

    const auto& windows = cfg.getSf2ChannelWindows();
    REQUIRE(windows.size() == 2);
    CHECK(windows[0].mpu == 0);
    CHECK(windows[0].ch == 12);
    CHECK(windows[0].fluidsynthChan == 0);
    CHECK(windows[1].mpu == 2);
    CHECK(windows[1].ch == 9);
    CHECK(windows[1].fluidsynthChan == 2);
}

TEST_CASE("FITOMConfig: sf2_banks parse into getSf2BankRegistry() when a chip=\"SF2\" "
          "device is declared", "[config][sf2]")
{
    json profile = {
        {"profile_name", "test"},
        {"devices", json::array({
            {{"if", "HW"}, {"chip", "SF2"}, {"plugin", "NoSuchPlugin"}}
        })},
        {"banks", {
            {"sf2_banks", json::array({
                {{"bank", 0}, {"file", "orchestral.sf2"}, {"sf2_bank", 0}},
                {{"bank", 2}, {"file", "drums.sf2"},      {"sf2_bank", 128}}
            })}
        }}
    };
    fs::path p = writeTempJson("fitom_test_sf2_banks_valid.profile.json", profile);

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));

    const auto& reg = cfg.getSf2BankRegistry();
    REQUIRE_FALSE(reg.empty());
    REQUIRE(reg.soundfontFiles().size() == 2);

    fitom::Sf2BankRegistry::Resolved r;
    REQUIRE(reg.resolve(2, r));
    CHECK(r.sf2Bank == 128);
}
