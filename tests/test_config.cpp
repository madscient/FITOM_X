// tests/test_config.cpp
// FITOMConfig のユニットテスト (Catch2 v3)

#include <catch2/catch_test_macros.hpp>
#include "fitom/Config.h"
#include "fitom/PatchManager.h"
#include "fitom/Sf2BankRegistry.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <tuple>
#include <vector>

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

TEST_CASE("FITOMConfig: banks as a string resolves to an external bank-set file, "
          "embedded as if written inline", "[config]")
{
    // banks: "xxx.json" は、参照先JSONオブジェクト(hw_banks/sw_banks/...を
    // 持つオブジェクト)をそのまま"banks"の値として展開する外部参照。
    // パス解決はプロファイル自身のディレクトリが基点(banks.*[].fileと同じ)。
    fs::path dir = fs::temp_directory_path() / "fitom_test_banks_external_ref";
    fs::create_directories(dir);

    json hwbank = {
        {"name", "external bankset test"},
        {"patches", json::array()}
    };
    fs::path hwbankPath = dir / "child.hwbank.json";
    { std::ofstream f(hwbankPath); f << hwbank.dump(2); }

    json bankset = {
        {"hw_banks", json::array({
            {{"group", "OPN"}, {"bank", 0}, {"file", "child.hwbank.json"}}
        })}
    };
    fs::path banksetPath = dir / "common.bankset.json";
    { std::ofstream f(banksetPath); f << bankset.dump(2); }

    json profile = {
        {"profile_name", "external banks ref test"},
        {"devices",      json::array()},
        {"banks",        "common.bankset.json"}
    };
    fs::path profilePath = dir / "external_banks.profile.json";
    { std::ofstream f(profilePath); f << profile.dump(2); }

    fitom::FITOMConfig cfg;
    fitom::PatchManager pm;
    REQUIRE(cfg.loadProfile(profilePath, &pm));

    uint8_t voicePatchType = fitom::FITOMConfig::stringToVoicePatchType("OPN");
    uint32_t group = fitom::FITOMConfig::voicePatchTypeToVoiceGroup(voicePatchType);
    const auto* bank = pm.hwRegistry().find(group, 0);
    REQUIRE(bank != nullptr);
    CHECK(bank->name == "external bankset test");
}

TEST_CASE("FITOMConfig: bank_overrides replaces a matching hw_banks entry (same "
          "group+bank) from an external banks reference, and adds a non-matching one",
          "[config]")
{
    // banksが外部参照(共通プリセットバンクセット)の場合でも、bank_overrides
    // (プロファイル側にインラインで書く)で一部のバンクだけを差し替えたり
    // 追加したりできることを確認する。識別キーはhw_banksでは"group"+"bank"。
    fs::path dir = fs::temp_directory_path() / "fitom_test_bank_overrides_hw";
    fs::create_directories(dir);

    auto writeHwBank = [](const fs::path& p, const std::string& name) {
        json hwbank = {{"name", name}, {"patches", json::array()}};
        std::ofstream f(p);
        f << hwbank.dump(2);
    };
    writeHwBank(dir / "opn_common.hwbank.json",  "common OPN bank0");
    writeHwBank(dir / "opm_common.hwbank.json",  "common OPM bank0");
    writeHwBank(dir / "opn_override.hwbank.json","overridden OPN bank0");
    writeHwBank(dir / "opn_added.hwbank.json",   "added OPN bank1");

    json bankset = {
        {"hw_banks", json::array({
            {{"group", "OPN"}, {"bank", 0}, {"file", "opn_common.hwbank.json"}},
            {{"group", "OPM"}, {"bank", 0}, {"file", "opm_common.hwbank.json"}}
        })}
    };
    { std::ofstream f(dir / "common.bankset.json"); f << bankset.dump(2); }

    json profile = {
        {"profile_name", "bank_overrides hw test"},
        {"devices",      json::array()},
        {"banks",        "common.bankset.json"},
        {"bank_overrides", {
            {"hw_banks", json::array({
                {{"group", "OPN"}, {"bank", 0}, {"file", "opn_override.hwbank.json"}},
                {{"group", "OPN"}, {"bank", 1}, {"file", "opn_added.hwbank.json"}}
            })}
        }}
    };
    fs::path profilePath = dir / "bank_overrides_hw.profile.json";
    { std::ofstream f(profilePath); f << profile.dump(2); }

    fitom::FITOMConfig cfg;
    fitom::PatchManager pm;
    REQUIRE(cfg.loadProfile(profilePath, &pm));

    uint8_t opnType  = fitom::FITOMConfig::stringToVoicePatchType("OPN");
    uint8_t opmType  = fitom::FITOMConfig::stringToVoicePatchType("OPM");
    uint32_t opnGroup = fitom::FITOMConfig::voicePatchTypeToVoiceGroup(opnType);
    uint32_t opmGroup = fitom::FITOMConfig::voicePatchTypeToVoiceGroup(opmType);

    // group+bankが一致するOPN bank0は上書きされている
    const auto* opnBank0 = pm.hwRegistry().find(opnGroup, 0);
    REQUIRE(opnBank0 != nullptr);
    CHECK(opnBank0->name == "overridden OPN bank0");

    // 一致しないOPM bank0は共通セットのまま変化しない
    const auto* opmBank0 = pm.hwRegistry().find(opmGroup, 0);
    REQUIRE(opmBank0 != nullptr);
    CHECK(opmBank0->name == "common OPM bank0");

    // 共通セットに無いOPN bank1は追加としてロードされている
    const auto* opnBank1 = pm.hwRegistry().find(opnGroup, 1);
    REQUIRE(opnBank1 != nullptr);
    CHECK(opnBank1->name == "added OPN bank1");
}

TEST_CASE("FITOMConfig: bank_overrides matches drum_banks by 'prog' (not 'bank', "
          "which drum_banks entries don't have)", "[config]")
{
    // drum_banksはbankフィールドを持たない(常に固定バンク0)ため、
    // bank_overridesの識別キーもprogを使う(bankEntryKeyMatches参照)。
    fs::path dir = fs::temp_directory_path() / "fitom_test_bank_overrides_drum";
    fs::create_directories(dir);

    auto writeKit = [](const fs::path& p, const std::string& name) {
        json kit = {{"name", name}, {"notes", json::array()}};
        std::ofstream f(p);
        f << kit.dump(2);
    };
    writeKit(dir / "kit0_common.drumkit.json",   "common kit prog0");
    writeKit(dir / "kit0_override.drumkit.json", "overridden kit prog0");
    writeKit(dir / "kit1_added.drumkit.json",    "added kit prog1");

    json profile = {
        {"profile_name", "bank_overrides drum test"},
        {"devices",      json::array()},
        {"banks", {
            {"drum_banks", json::array({
                {{"prog", 0}, {"file", "kit0_common.drumkit.json"}}
            })}
        }},
        {"bank_overrides", {
            {"drum_banks", json::array({
                {{"prog", 0}, {"file", "kit0_override.drumkit.json"}},
                {{"prog", 1}, {"file", "kit1_added.drumkit.json"}}
            })}
        }}
    };
    fs::path profilePath = dir / "bank_overrides_drum.profile.json";
    { std::ofstream f(profilePath); f << profile.dump(2); }

    fitom::FITOMConfig cfg;
    fitom::PatchManager pm;
    REQUIRE(cfg.loadProfile(profilePath, &pm));

    const auto* kit0 = pm.resolveDrum(0, 0);
    REQUIRE(kit0 != nullptr);
    CHECK(std::string(kit0->name) == "overridden kit prog0");

    const auto* kit1 = pm.resolveDrum(0, 1);
    REQUIRE(kit1 != nullptr);
    CHECK(std::string(kit1->name) == "added kit prog1");
}

// FITOMBridge::resolveChannelHwPatch()のkind="drum"対応(2026年7月、
// リズムチャンネルの外部パッチエディタ起動)は、各progが自分自身の
// 読み込み元ファイル(DrumPatch::filename)を正しく保持していることに
// 依存する。DrumPatchBank::filename(バンク単位、複数progが共有し
// 最後に読んだファイルで上書きされる)と混同していないことを検証する
// (fitom_testsはfitom_core単体のみリンクしFITOMBridgeを含まないため、
// ここではPatchManager::loadDrumKitJson()が設定するprog単位の
// DrumPatch::filenameを直接検証する)。
TEST_CASE("PatchManager: loadDrumKitJson() records each prog's own source file "
          "in DrumPatch::filename (not just the bank-level, last-writer-wins one)",
          "[config][drumkit]")
{
    fs::path dir = fs::temp_directory_path() / "fitom_test_drumkit_filename";
    fs::create_directories(dir);

    auto writeKit = [](const fs::path& p, const std::string& name) {
        json kit = {{"name", name}, {"notes", json::array()}};
        std::ofstream f(p);
        f << kit.dump(2);
    };
    const fs::path kit0Path = dir / "kit0.drumkit.json";
    const fs::path kit1Path = dir / "kit1.drumkit.json";
    writeKit(kit0Path, "kit prog0");
    writeKit(kit1Path, "kit prog1");

    fitom::PatchManager pm;
    REQUIRE(pm.loadDrumKitJson(kit0Path, 0));
    REQUIRE(pm.loadDrumKitJson(kit1Path, 1));

    const auto* kit0 = pm.resolveDrum(0, 0);
    const auto* kit1 = pm.resolveDrum(0, 1);
    REQUIRE(kit0 != nullptr);
    REQUIRE(kit1 != nullptr);
    CHECK(kit0->filename == kit0Path.string());
    CHECK(kit1->filename == kit1Path.string());
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
    const auto* sampleBank = pm.sampleRegistry().find(VOICE_PATCH_ADPCMA, 7);
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

TEST_CASE("SampleZoneBankRegistry: AWM hw_banks and PCM pcm_banks sharing the same "
          "bank number don't collide", "[config][pcm][samplezone]")
{
    // 実プロファイル(unified.bankset.json)でAWMのバンク0/1(hw_banks経由)と
    // ADPCM-B/ADPCM-Aのバンク0/1(pcm_banks経由の自動合成)が番号衝突し、
    // 後から読み込まれるpcm_banks側がAWMの登録を上書きしてパッチピッカーの
    // AWMバンク一覧が常に空になる不具合があった(2026年7月)。
    // SampleZoneBankRegistryのキーにvoicePatchTypeを含めることで、番号が
    // 同じでもチップ族が違えば独立して共存できることを確認する。
    fs::path dir = fs::temp_directory_path() / "fitom_test_samplezone_collision";
    fs::create_directories(dir);

    json awmBank = {
        {"name", "AWM test bank"},
        {"patches", json::array({
            {{"prog", 0}, {"name", "awm patch0"},
             {"zones", json::array({ {{"wave_index", 10}} })}}
        })}
    };
    fs::path awmBankPath = dir / "awm.samplezonebank.json";
    { std::ofstream f(awmBankPath); f << awmBank.dump(2); }

    json pcmbank = {
        {"name",  "adpcma test bank"},
        {"codec", "adpcm-a"},
        {"entries", json::array({
            {{"entry_no", 0}, {"name", "kick"},
             {"start_offset", 0}, {"end_offset", 255}, {"size", 200}, {"padded_size", 256}}
        })}
    };
    fs::path pcmbankPath = dir / "adpcma.pcmbank.json";
    { std::ofstream f(pcmbankPath); f << pcmbank.dump(2); }

    json profile = {
        {"profile_name", "samplezone collision test"},
        {"devices",      json::array()},
        {"banks", {
            {"hw_banks", json::array({
                {{"group", "AWM"}, {"bank", 0}, {"file", "awm.samplezonebank.json"}}
            })},
            {"pcm_banks", json::array({
                {{"group", "ADPCMA"}, {"bank", 0}, {"file", "adpcma.pcmbank.json"}}
            })}
        }}
    };
    fs::path profilePath = dir / "collision.profile.json";
    { std::ofstream f(profilePath); f << profile.dump(2); }

    fitom::FITOMConfig cfg;
    fitom::PatchManager pm;
    REQUIRE(cfg.loadProfile(profilePath, &pm));

    // 両チップ族ともバンク0が独立して見える(どちらかがどちらかを
    // 上書きしていない)
    CHECK(pm.sampleRegistry().listBankNumbers(VOICE_PATCH_AWM) == std::vector<int>{0});
    CHECK(pm.sampleRegistry().listBankNumbers(VOICE_PATCH_ADPCMA) == std::vector<int>{0});

    const auto* awm = pm.sampleRegistry().find(VOICE_PATCH_AWM, 0);
    REQUIRE(awm != nullptr);
    CHECK(awm->name == "AWM test bank");
    CHECK(awm->get(0).isValid());
    CHECK(std::string(awm->get(0).name) == "awm patch0");

    const auto* adpcma = pm.sampleRegistry().find(VOICE_PATCH_ADPCMA, 0);
    REQUIRE(adpcma != nullptr);
    CHECK(adpcma->name == "adpcma test bank");
    CHECK(adpcma->get(0).isValid());
    CHECK(std::string(adpcma->get(0).name) == "kick");
}

TEST_CASE("PatchManager: OPLL-family ROM voice accessors for the patch picker",
          "[patchdata][opll]")
{
    // パッチピッカーでOPLLビルトインROM音色(バンク0固定)が常に空欄に
    // なる不具合(AWMと同種、HwBankRegistryに一切現れないバンクをGUIが
    // 列挙できていなかった)の指摘を受けて追加(2026年8月)。
    fitom::PatchManager pm;

    const auto* opll  = pm.getOpllRomPatches(VOICE_PATCH_OPLL);
    const auto* opllx = pm.getOpllRomPatches(VOICE_PATCH_OPLLX);
    const auto* opllp = pm.getOpllRomPatches(VOICE_PATCH_OPLLP);
    const auto* vrc7  = pm.getOpllRomPatches(VOICE_PATCH_VRC7);
    REQUIRE(opll  != nullptr);
    REQUIRE(opllx != nullptr);
    REQUIRE(opllp != nullptr);
    REQUIRE(vrc7  != nullptr);
    // OPLL系以外はnullptr(通常のHwBankRegistry経由のカテゴリ)
    CHECK(pm.getOpllRomPatches(VOICE_PATCH_OPN2) == nullptr);

    // 添字0は無音として意図的に予約(名前は空文字)
    CHECK(std::string((*opll)[0].name).empty());
    // 添字1以降は有効な名前を持つ
    CHECK((*opll)[1].isValid());
    CHECK(!std::string((*opll)[1].name).empty());

    // 【重要】各エントリのidはvariantSel<<4|instIndexでエンコードされて
    // いる(パッチピッカーがProgram Change値として送るべき値そのもの。
    // 配列添字[instIndexのみ]をそのまま送ると、OPLL以外のカテゴリで
    // 誤ったチップの音色を選んでしまう実装バグが見つかったため、この
    // エンコードを直接検証する)。
    CHECK((*opll)[3].id  == 0x03);
    CHECK((*opllx)[3].id == 0x13);
    CHECK((*opllp)[3].id == 0x23);
    CHECK((*vrc7)[3].id  == 0x33);

    // hwProgからの直接解決(PatchManager::resolveOpllRomVoice()と同じ
    // デコード規則)は、どのCC#0カテゴリを経由したかに一切依存しない。
    SECTION("getOpllRomPatchByProgはvoicePatchTypeを介さず直接解決する") {
        const fitom::HwPatch* p = pm.getOpllRomPatchByProg(0x13); // OPLLX inst3
        REQUIRE(p != nullptr);
        CHECK(p == &(*opllx)[3]);

        CHECK(pm.getOpllRomPatchByProg(0x00) == nullptr); // instIndex=0(無音)
        CHECK(pm.getOpllRomPatchByProg(0x10) == nullptr); // OPLLX inst0(無音)
        CHECK(pm.getOpllRomPatchByProg(0x41) == nullptr); // variantSel=4(未定義)
    }
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

    CHECK(pm.sampleRegistry().find(VOICE_PATCH_ADPCMB, 3) == nullptr);
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
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_OPNB, false, spec));
    REQUIRE(spec.size() == 4);
    CHECK(spec[0].deviceType == DEVICE_OPNB);
    CHECK(spec[1].deviceType == DEVICE_SSG);
    CHECK(spec[2].deviceType == DEVICE_ADPCMA);
    CHECK(spec[3].deviceType == DEVICE_ADPCMB);

    std::vector<fitom::FITOMConfig::SubDeviceSpec> specB;
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_2610B, false, specB));
    REQUIRE(specB.size() == 4);
    CHECK(specB[0].deviceType == DEVICE_2610B);
    CHECK(specB[3].deviceType == DEVICE_ADPCMB);
}

// OPL4 = OPL3(FM部、4OP+2OP) + AWM(PCM部)。FM部の2サブデバイスは物理的に
// port1/port2の2バンクを両方使うため usesExtraPort=true。AWM部はこの2ポート
// モデルには乗らない3つ目のレジスタバンク(アドレス0x200以降、
// CFITOM::resolveHighBankPort()がOffsetPort(port,0x200)で差し替える)を
// 使うため usesExtraPort=false のままで正しい(2026年7月、ユーザー指摘で
// 発覚したAWM部とFM部のレジスタアドレス衝突バグの回帰防止)。
TEST_CASE("FITOMConfig: DEVICE_OPL4 composite spec is FM-4OP+FM-2OP+AWM+RHYTHM "
          "only when rhythm_mode is enabled for that instance, "
          "AWM does not share the FM subdevices' extraPort", "[config]")
{
    std::vector<fitom::FITOMConfig::SubDeviceSpec> spec;
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_OPL4, true, spec));
    REQUIRE(spec.size() == 4);
    CHECK(spec[0].deviceType == DEVICE_OPL3);
    CHECK(spec[0].usesExtraPort == true);
    CHECK(spec[1].deviceType == DEVICE_OPL3_2);
    CHECK(spec[1].usesExtraPort == true);
    CHECK(spec[1].rhythmCapable == true);
    CHECK(spec[2].deviceType == DEVICE_OPL4AWM);
    CHECK(spec[2].usesExtraPort == false);
    CHECK(spec[3].deviceType == DEVICE_OPL_RHY);
    CHECK(spec[3].usesExtraPort == false);

    // rhythm_mode:false (既定値) では COPLRhythm はFM本体側のch6-8と
    // レジスタを奪い合うだけでDVA上の調整が無いため、リズムサブデバイス
    // 自体を生成しない(2026年7月修正)。
    spec.clear();
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_OPL4, false, spec));
    REQUIRE(spec.size() == 3);
    CHECK(spec[0].deviceType == DEVICE_OPL3);
    CHECK(spec[1].deviceType == DEVICE_OPL3_2);
    CHECK(spec[2].deviceType == DEVICE_OPL4AWM);
}

TEST_CASE("FITOMConfig: DEVICE_OPL/DEVICE_OPL2/DEVICE_OPL3 composite specs "
          "add a DEVICE_OPL_RHY sub-device sharing the primary port only when "
          "rhythm_mode is enabled for that instance (2026年7月修正)", "[config]")
{
    std::vector<fitom::FITOMConfig::SubDeviceSpec> spec;

    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_OPL, true, spec));
    REQUIRE(spec.size() == 2);
    CHECK(spec[0].deviceType == DEVICE_OPL);
    CHECK(spec[0].rhythmCapable == true);
    CHECK(spec[1].deviceType == DEVICE_OPL_RHY);
    CHECK(spec[1].usesExtraPort == false);
    CHECK(spec[1].rhythmCapable == false);

    // rhythm_mode:false ならリズムサブデバイスは生成されず、FM本体のみ
    // (9ch フル、ch6-8も通常の楽音chのまま)。
    spec.clear();
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_OPL, false, spec));
    REQUIRE(spec.size() == 1);
    CHECK(spec[0].deviceType == DEVICE_OPL);

    // Y8950(MSX-AUDIO)はADPCM-Bを内蔵するため、OPL/OPL2と異なりFM本体+
    // ADPCM-Bの2つが常に生成され、rhythm_mode:trueの場合のみさらに
    // リズムサブデバイスが加わる(2026年8月修正)。
    spec.clear();
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_Y8950, true, spec));
    REQUIRE(spec.size() == 3);
    CHECK(spec[0].deviceType == DEVICE_Y8950);
    CHECK(spec[1].deviceType == DEVICE_ADPCMB_Y8950);
    CHECK(spec[2].deviceType == DEVICE_OPL_RHY);

    spec.clear();
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_Y8950, false, spec));
    REQUIRE(spec.size() == 2);
    CHECK(spec[0].deviceType == DEVICE_Y8950);
    CHECK(spec[1].deviceType == DEVICE_ADPCMB_Y8950);

    spec.clear();
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_OPL2, true, spec));
    REQUIRE(spec.size() == 2);
    CHECK(spec[0].deviceType == DEVICE_OPL2);
    CHECK(spec[0].rhythmCapable == true);
    CHECK(spec[1].deviceType == DEVICE_OPL_RHY);

    spec.clear();
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_OPL2, false, spec));
    REQUIRE(spec.size() == 1);

    spec.clear();
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_OPL3, true, spec));
    REQUIRE(spec.size() == 3);
    CHECK(spec[0].deviceType == DEVICE_OPL3);
    CHECK(spec[1].deviceType == DEVICE_OPL3_2);
    CHECK(spec[1].rhythmCapable == true);
    CHECK(spec[2].deviceType == DEVICE_OPL_RHY);
    CHECK(spec[2].usesExtraPort == false);

    spec.clear();
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_OPL3, false, spec));
    REQUIRE(spec.size() == 2);
    CHECK(spec[0].deviceType == DEVICE_OPL3);
    CHECK(spec[1].deviceType == DEVICE_OPL3_2);
}

TEST_CASE("FITOMConfig: DEVICE_OPLL composite spec adds DEVICE_OPLL_RHY only "
          "when rhythm_mode is enabled for that instance (2026年7月修正)", "[config]")
{
    std::vector<fitom::FITOMConfig::SubDeviceSpec> spec;

    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_OPLL, true, spec));
    REQUIRE(spec.size() == 2);
    CHECK(spec[0].deviceType == DEVICE_OPLL);
    CHECK(spec[0].rhythmCapable == true);
    CHECK(spec[1].deviceType == DEVICE_OPLL_RHY);

    spec.clear();
    REQUIRE(fitom::FITOMConfig::resolveCompositeSpec(DEVICE_OPLL, false, spec));
    REQUIRE(spec.size() == 1);
    CHECK(spec[0].deviceType == DEVICE_OPLL);
}

// ================================================================
//  リニアステレオ化 (CLinearPanDevice、stereo_pair:true の明示指定)
//  docs/chip-driver-architecture.md「3.1 リニアステレオ化」参照
// ================================================================

namespace {

// パン設定とI/F名だけを持つテスト用IPort。
// pairStereoDevices()はgetPanpot()/getInterfaceDesc()しか参照しない。
class StereoTestPort : public fitom::IPort {
public:
    StereoTestPort(int panpot, std::string iface)
        : panpot_(panpot), iface_(std::move(iface)) {}
    void    write(uint16_t, uint16_t) override {}
    uint8_t read(uint16_t)            override { return 0; }
    int         getPanpot()        override { return panpot_; }
    std::string getInterfaceDesc() override { return iface_; }
private:
    int         panpot_;
    std::string iface_;
};

// devices[]エントリを1つ作るヘルパー。compositeGroup<0 = 単独チップ。
// stereoSide=None は stereo_pair:true (pan由来のL/R判定) を表す。
fitom::DeviceEntry makeEntry(const std::string& label, uint32_t deviceType,
                             const std::shared_ptr<fitom::IPort>& port,
                             bool stereoPairRequested, int compositeGroup = -1,
                             fitom::StereoSide side = fitom::StereoSide::None)
{
    fitom::DeviceEntry e;
    e.label                = label;
    e.deviceType           = deviceType;
    e.port                 = port;
    e.stereoPairRequested  = stereoPairRequested;
    e.compositeGroup       = compositeGroup;
    e.stereoSide           = side;
    return e;
}

} // namespace

// stereo_pair:true が無ければ(pan=1/2に振り分けただけでは)発動しない。
// 自動検出しない仕様の明文化 + 回帰防止。
TEST_CASE("FITOMConfig::pairStereoDevices: pan=1/2 alone does NOT bundle without "
          "stereo_pair:true on both entries", "[config][stereo]")
{
    auto portL = std::make_shared<StereoTestPort>(1, "FitomEmuIF");
    auto portR = std::make_shared<StereoTestPort>(2, "FitomEmuIF");

    std::vector<fitom::DeviceEntry> devices;
    devices.push_back(makeEntry("VRC7", DEVICE_VRC7, portL, false));
    devices.push_back(makeEntry("VRC7", DEVICE_VRC7, portR, false));
    fitom::FITOMConfig::pairStereoDevices(devices);

    REQUIRE(devices.size() == 2);
    CHECK(devices[0].stereoPairPort == nullptr);
    CHECK(devices[1].stereoPairPort == nullptr);

    // 片側だけの指定でも発動しない
    std::vector<fitom::DeviceEntry> half;
    half.push_back(makeEntry("VRC7", DEVICE_VRC7, portL, true));
    half.push_back(makeEntry("VRC7", DEVICE_VRC7, portR, false));
    fitom::FITOMConfig::pairStereoDevices(half);
    REQUIRE(half.size() == 2);
    CHECK(half[0].stereoPairPort == nullptr);
}

TEST_CASE("FITOMConfig::pairStereoDevices: a single (non-composite) chip pair with "
          "stereo_pair:true merges R into L", "[config][stereo]")
{
    auto portL = std::make_shared<StereoTestPort>(1, "FitomEmuIF");
    auto portR = std::make_shared<StereoTestPort>(2, "FitomEmuIF");

    std::vector<fitom::DeviceEntry> devices;
    devices.push_back(makeEntry("VRC7", DEVICE_VRC7, portL, true));
    devices.push_back(makeEntry("VRC7", DEVICE_VRC7, portR, true));
    fitom::FITOMConfig::pairStereoDevices(devices);

    REQUIRE(devices.size() == 1);            // R側エントリは削除される
    CHECK(devices[0].port == portL);
    CHECK(devices[0].stereoPairPort == portR);
}

// composite展開されたチップ(OPLL[rhythm_mode:true] = FM本体 + 内蔵リズム)でも
// compositeGroup単位でまとめてペアリングされること。2026年8月まではここで
// stereo_pairが無視されており、リニアステレオ化が一切発動しなかった。
// 内蔵リズム(DEVICE_OPLL_RHY)はVOICE_PATCH_NONEのため、VoicePatchType基準の
// 旧実装ではR側だけが孤立して残ってしまう点も同時に回帰防止する。
TEST_CASE("FITOMConfig::pairStereoDevices: composite-expanded chips are paired as a "
          "whole compositeGroup, including VOICE_PATCH_NONE sub-devices",
          "[config][stereo]")
{
    auto portL = std::make_shared<StereoTestPort>(1, "FitomEmuIF");
    auto portR = std::make_shared<StereoTestPort>(2, "FitomEmuIF");

    std::vector<fitom::DeviceEntry> devices;
    devices.push_back(makeEntry("OPLL-FM",      DEVICE_OPLL,     portL, true, 0));
    devices.push_back(makeEntry("OPLL-RHYTHM",  DEVICE_OPLL_RHY, portL, true, 0));
    devices.push_back(makeEntry("OPLL-FM",      DEVICE_OPLL,     portR, true, 1));
    devices.push_back(makeEntry("OPLL-RHYTHM",  DEVICE_OPLL_RHY, portR, true, 1));
    fitom::FITOMConfig::pairStereoDevices(devices);

    REQUIRE(devices.size() == 2);            // R側グループは丸ごと削除される
    CHECK(devices[0].deviceType == DEVICE_OPLL);
    CHECK(devices[0].port == portL);
    CHECK(devices[0].stereoPairPort == portR);
    CHECK(devices[1].deviceType == DEVICE_OPLL_RHY);
    CHECK(devices[1].port == portL);
    CHECK(devices[1].stereoPairPort == portR);
}

// 別チップ同士は束ねない。OPLL系(OPLL/OPLLP/OPLLX/VRC7)は同一VOICE_GROUPだが、
// CLinearPanDeviceは物理的に同一のチップ2台を前提とするためdeviceType一致が必要。
TEST_CASE("FITOMConfig::pairStereoDevices: different chips / different interfaces "
          "are never paired", "[config][stereo]")
{
    auto emuL = std::make_shared<StereoTestPort>(1, "FitomEmuIF");
    auto emuR = std::make_shared<StereoTestPort>(2, "FitomEmuIF");
    auto hwR  = std::make_shared<StereoTestPort>(2, "FitomHwIF");

    // 同一I/F・同一pan構成でもdeviceTypeが違えばペアにならない
    std::vector<fitom::DeviceEntry> diffChip;
    diffChip.push_back(makeEntry("OPLLP", DEVICE_OPLLP, emuL, true));
    diffChip.push_back(makeEntry("OPLLX", DEVICE_OPLLX, emuR, true));
    fitom::FITOMConfig::pairStereoDevices(diffChip);
    REQUIRE(diffChip.size() == 2);
    CHECK(diffChip[0].stereoPairPort == nullptr);

    // 同一deviceTypeでもI/F(プラグイン)が違えばペアにならない
    std::vector<fitom::DeviceEntry> diffIface;
    diffIface.push_back(makeEntry("VRC7", DEVICE_VRC7, emuL, true));
    diffIface.push_back(makeEntry("VRC7", DEVICE_VRC7, hwR,  true));
    fitom::FITOMConfig::pairStereoDevices(diffIface);
    REQUIRE(diffIface.size() == 2);
    CHECK(diffIface[0].stereoPairPort == nullptr);
}

// OPL4のAWM/PCM部はチャンネルごとのパンポットをハードウェアで持つため、
// stereo_pairの伝播対象外(subDeviceAcceptsStereoPair()==false)。
TEST_CASE("FITOMConfig::subDeviceAcceptsStereoPair: only DEVICE_OPL4AWM is excluded "
          "(it has hardware panpot)", "[config][stereo]")
{
    CHECK_FALSE(fitom::FITOMConfig::subDeviceAcceptsStereoPair(DEVICE_OPL4AWM));
    // OPL4のFM部・その他のサブデバイスは引き続き対象
    CHECK(fitom::FITOMConfig::subDeviceAcceptsStereoPair(DEVICE_OPL3));
    CHECK(fitom::FITOMConfig::subDeviceAcceptsStereoPair(DEVICE_OPL3_2));
    CHECK(fitom::FITOMConfig::subDeviceAcceptsStereoPair(DEVICE_OPLL));
    CHECK(fitom::FITOMConfig::subDeviceAcceptsStereoPair(DEVICE_OPLL_RHY));
    CHECK(fitom::FITOMConfig::subDeviceAcceptsStereoPair(DEVICE_SSG));
}

// ペアリングはグループ全体ではなく「stereo_pair対象サブデバイスの部分集合」で
// 判定するため、構成の異なるcompositeグループ同士(OPL4のFM部 と OPL3)でも
// FM部だけをL/Rペアにできる。AWM部(stereo_pair対象外)は独立したモノラル
// デバイスとして残る。
TEST_CASE("FITOMConfig::pairStereoDevices: OPL4's FM sub-devices pair with an OPL3 "
          "while its AWM part stays an independent mono device", "[config][stereo]")
{
    auto portL = std::make_shared<StereoTestPort>(1, "FitomEmuIF"); // OPL4 (L)
    auto portR = std::make_shared<StereoTestPort>(2, "FitomEmuIF"); // OPL3 (R)

    std::vector<fitom::DeviceEntry> devices;
    // OPL4(L): FM 4OP + FM 2OP + AWM。AWMだけstereoPairRequested=false
    devices.push_back(makeEntry("OPL4-FM4OP", DEVICE_OPL3,     portL, true,  0));
    devices.push_back(makeEntry("OPL4-FM2OP", DEVICE_OPL3_2,   portL, true,  0));
    devices.push_back(makeEntry("OPL4-AWM",   DEVICE_OPL4AWM,  portL, false, 0));
    // OPL3(R): FM 4OP + FM 2OP
    devices.push_back(makeEntry("OPL3-FM4OP", DEVICE_OPL3,     portR, true,  1));
    devices.push_back(makeEntry("OPL3-FM2OP", DEVICE_OPL3_2,   portR, true,  1));
    fitom::FITOMConfig::pairStereoDevices(devices);

    REQUIRE(devices.size() == 3);            // R側のFM 2エントリのみ削除される
    CHECK(devices[0].deviceType == DEVICE_OPL3);
    CHECK(devices[0].stereoPairPort == portR);
    CHECK(devices[1].deviceType == DEVICE_OPL3_2);
    CHECK(devices[1].stereoPairPort == portR);
    // AWM部はステレオ化されず、L側の物理ポート上の独立デバイスのまま
    CHECK(devices[2].deviceType == DEVICE_OPL4AWM);
    CHECK(devices[2].port == portL);
    CHECK(devices[2].stereoPairPort == nullptr);
}

// OPL4同士をL/Rペアにした場合も、AWM部だけは両側とも独立したデバイスとして
// 残る(L側のAWMがR側のAWMを吸収しない)。
TEST_CASE("FITOMConfig::pairStereoDevices: an OPL4 L/R pair leaves both AWM parts "
          "as independent devices", "[config][stereo]")
{
    auto portL = std::make_shared<StereoTestPort>(1, "FitomEmuIF");
    auto portR = std::make_shared<StereoTestPort>(2, "FitomEmuIF");

    std::vector<fitom::DeviceEntry> devices;
    devices.push_back(makeEntry("OPL4-FM4OP", DEVICE_OPL3,    portL, true,  0));
    devices.push_back(makeEntry("OPL4-FM2OP", DEVICE_OPL3_2,  portL, true,  0));
    devices.push_back(makeEntry("OPL4-AWM",   DEVICE_OPL4AWM, portL, false, 0));
    devices.push_back(makeEntry("OPL4-FM4OP", DEVICE_OPL3,    portR, true,  1));
    devices.push_back(makeEntry("OPL4-FM2OP", DEVICE_OPL3_2,  portR, true,  1));
    devices.push_back(makeEntry("OPL4-AWM",   DEVICE_OPL4AWM, portR, false, 1));
    fitom::FITOMConfig::pairStereoDevices(devices);

    REQUIRE(devices.size() == 4);            // R側のFM 2エントリのみ削除
    CHECK(devices[0].stereoPairPort == portR);
    CHECK(devices[1].stereoPairPort == portR);
    CHECK(devices[2].deviceType == DEVICE_OPL4AWM);
    CHECK(devices[2].port == portL);
    CHECK(devices[2].stereoPairPort == nullptr);
    CHECK(devices[3].deviceType == DEVICE_OPL4AWM);
    CHECK(devices[3].port == portR);
    CHECK(devices[3].stereoPairPort == nullptr);
}

// L/Rペアが2組ある場合、それぞれ別のペアとして独立に束ねられること
// (2組目のL側が1組目のR側を横取りしない = 1つのL側につきR側は1つだけ)。
TEST_CASE("FITOMConfig::pairStereoDevices: two independent L/R pairs of the same "
          "chip each form their own stereo unit", "[config][stereo]")
{
    auto l0 = std::make_shared<StereoTestPort>(1, "FitomEmuIF");
    auto r0 = std::make_shared<StereoTestPort>(2, "FitomEmuIF");
    auto l1 = std::make_shared<StereoTestPort>(1, "FitomEmuIF");
    auto r1 = std::make_shared<StereoTestPort>(2, "FitomEmuIF");

    std::vector<fitom::DeviceEntry> devices;
    devices.push_back(makeEntry("VRC7#0", DEVICE_VRC7, l0, true));
    devices.push_back(makeEntry("VRC7#1", DEVICE_VRC7, l1, true));
    devices.push_back(makeEntry("VRC7#2", DEVICE_VRC7, r0, true));
    devices.push_back(makeEntry("VRC7#3", DEVICE_VRC7, r1, true));
    fitom::FITOMConfig::pairStereoDevices(devices);

    REQUIRE(devices.size() == 2);
    CHECK(devices[0].port == l0);
    CHECK(devices[0].stereoPairPort == r0);
    CHECK(devices[1].port == l1);
    CHECK(devices[1].stereoPairPort == r1);
}

// ================================================================
//  チップ内L/R分離方式 (stereo_pair:"L"/"R"、2026年8月新設)
// ================================================================

// stereo_pair:"L"/"R" はL/R役割をpanから独立して宣言する。プラグイン側の
// panは0(Stereo)のままでよいため、getPanpot()が1でも2でもないポートで
// ペアが成立すること、およびチップ内分離フラグが立つことを確認する。
TEST_CASE("FITOMConfig::pairStereoDevices: explicit \"L\"/\"R\" pairs regardless of "
          "the plugin-side pan value and marks the pair as chip-level",
          "[config][stereo]")
{
    // pan=0 (Stereo): 従来方式ならL/R役割を判定できない値
    auto portL = std::make_shared<StereoTestPort>(0, "FitomEmuIF");
    auto portR = std::make_shared<StereoTestPort>(0, "FitomEmuIF");

    std::vector<fitom::DeviceEntry> devices;
    devices.push_back(makeEntry("OPM-L", DEVICE_OPM, portL, true, -1, fitom::StereoSide::Left));
    devices.push_back(makeEntry("OPM-R", DEVICE_OPM, portR, true, -1, fitom::StereoSide::Right));
    fitom::FITOMConfig::pairStereoDevices(devices);

    REQUIRE(devices.size() == 1);
    CHECK(devices[0].port == portL);
    CHECK(devices[0].stereoPairPort == portR);
    CHECK(devices[0].stereoPairChipLevel == true);
}

// stereo_pair:true (pan由来) のペアはチップ内分離フラグを立てない
// (左右分離はプラグイン側のルーティングが行う従来方式)。
TEST_CASE("FITOMConfig::pairStereoDevices: pan-derived pairs are not chip-level",
          "[config][stereo]")
{
    auto portL = std::make_shared<StereoTestPort>(1, "FitomEmuIF");
    auto portR = std::make_shared<StereoTestPort>(2, "FitomEmuIF");

    std::vector<fitom::DeviceEntry> devices;
    devices.push_back(makeEntry("OPM", DEVICE_OPM, portL, true));
    devices.push_back(makeEntry("OPM", DEVICE_OPM, portR, true));
    fitom::FITOMConfig::pairStereoDevices(devices);

    REQUIRE(devices.size() == 1);
    CHECK(devices[0].stereoPairPort == portR);
    CHECK(devices[0].stereoPairChipLevel == false);
}

// 片側だけ "L"/"R"、もう片側は pan 由来という混在構成では、プラグイン側の
// ルーティングが絡んで意図が確定しないため従来方式(チップ内分離なし)に倒す。
TEST_CASE("FITOMConfig::pairStereoDevices: a mixed declaration (one explicit side, "
          "one pan-derived) falls back to the plugin-routed method",
          "[config][stereo]")
{
    auto portL = std::make_shared<StereoTestPort>(1, "FitomEmuIF"); // pan由来のL
    auto portR = std::make_shared<StereoTestPort>(0, "FitomEmuIF"); // 明示のR

    std::vector<fitom::DeviceEntry> devices;
    devices.push_back(makeEntry("OPM-L", DEVICE_OPM, portL, true));
    devices.push_back(makeEntry("OPM-R", DEVICE_OPM, portR, true, -1, fitom::StereoSide::Right));
    fitom::FITOMConfig::pairStereoDevices(devices);

    REQUIRE(devices.size() == 1);
    CHECK(devices[0].stereoPairPort == portR);
    CHECK(devices[0].stereoPairChipLevel == false);
}

// composite展開されたOPNA同士を "L"/"R" で束ねた場合、FM部・SSG部・ADPCM部・
// リズム部の全サブデバイスがまとめてペアになり、いずれもチップ内分離になる。
TEST_CASE("FITOMConfig::pairStereoDevices: explicit \"L\"/\"R\" works for "
          "composite-expanded chips as a whole group", "[config][stereo]")
{
    auto portL = std::make_shared<StereoTestPort>(0, "FitomEmuIF");
    auto portR = std::make_shared<StereoTestPort>(0, "FitomEmuIF");

    std::vector<fitom::DeviceEntry> devices;
    for (uint32_t dt : {DEVICE_OPNA, DEVICE_SSG, DEVICE_ADPCMB_OPNA, DEVICE_OPNA_RHY})
        devices.push_back(makeEntry("OPNA-L", dt, portL, true, 0, fitom::StereoSide::Left));
    for (uint32_t dt : {DEVICE_OPNA, DEVICE_SSG, DEVICE_ADPCMB_OPNA, DEVICE_OPNA_RHY})
        devices.push_back(makeEntry("OPNA-R", dt, portR, true, 1, fitom::StereoSide::Right));
    fitom::FITOMConfig::pairStereoDevices(devices);

    REQUIRE(devices.size() == 4);
    for (const auto& e : devices) {
        CHECK(e.port == portL);
        CHECK(e.stereoPairPort == portR);
        CHECK(e.stereoPairChipLevel == true);
    }
}

// チップ内L/R分離の可否は deviceType 側の索引で判定する。ユーザー指定の
// 対象チップ(OPM/OPZ/OPL3/OPL4[FM部]/OPN2/OPNA/OPNB/OPNBB)が漏れなく
// trueであること、モノラル出力チップがfalseであることの回帰防止。
TEST_CASE("FITOMConfig::deviceHasChipLevelPanpot: chips with per-channel L/R output "
          "bits are recognized, mono-output chips are not", "[config][stereo]")
{
    // 対象: OPM/OPZ系
    CHECK(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPM));
    CHECK(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPP));
    CHECK(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPZ));
    CHECK(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPZ2));
    // 対象: OPL3系 (OPL4のFM部はcomposite展開でDEVICE_OPL3/OPL3_2になる)
    CHECK(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPL3));
    CHECK(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPL3_2));
    // 対象: OPN2/OPNA/OPNB系とそのADPCMサブデバイス
    CHECK(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPN2));
    CHECK(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPNA));
    CHECK(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPNB));
    CHECK(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_2610B));
    CHECK(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_ADPCMA));
    CHECK(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_ADPCMB_OPNA));

    // 非対象: モノラル出力のチップ (updatePanpot()がno-op)
    CHECK_FALSE(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPLL));
    CHECK_FALSE(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_VRC7));
    CHECK_FALSE(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPL));
    CHECK_FALSE(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPL2));
    CHECK_FALSE(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_OPN));  // YM2203
    CHECK_FALSE(fitom::FITOMConfig::deviceHasChipLevelPanpot(DEVICE_SSG));
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

// ================================================================
//  SF2プリセット名解決 (phdrパース、docs/sf2-fluidsynth-integration.md ⑧節)
// ================================================================

namespace {

void appendU32LE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
void appendU16LE(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void appendFourCC(std::vector<uint8_t>& buf, const char* cc) {
    buf.insert(buf.end(), cc, cc + 4);
}
void appendPhdrRecord(std::vector<uint8_t>& buf, const std::string& name,
                       uint16_t preset, uint16_t bank) {
    char nameBuf[20] = {};
    std::memcpy(nameBuf, name.data(), std::min<size_t>(name.size(), 20));
    buf.insert(buf.end(), nameBuf, nameBuf + 20);
    appendU16LE(buf, preset);
    appendU16LE(buf, bank);
    appendU16LE(buf, 0); // wPresetBagNdx
    appendU32LE(buf, 0); // dwLibrary
    appendU32LE(buf, 0); // dwGenre
    appendU32LE(buf, 0); // dwMorphology
}

// 最小限のSoundFont2(RIFF/sfbk)ファイルを組み立てる。phdrサブチャンクのみが
// 実際のプリセット+終端センチネル(EOP)を持ち、INFO/sdtaは空のLISTチャンク
// として「pdta以外は読み飛ばす」経路もあわせて検証する。
// presets: {name, preset(program), bank} のリスト。
std::vector<uint8_t> buildMinimalSf2(
    const std::vector<std::tuple<std::string, uint16_t, uint16_t>>& presets)
{
    std::vector<uint8_t> phdrData;
    for (const auto& [name, preset, bank] : presets) {
        appendPhdrRecord(phdrData, name, preset, bank);
    }
    appendPhdrRecord(phdrData, "EOP", 0, 0); // 終端センチネル(実プリセットに含めない)

    std::vector<uint8_t> phdrChunk;
    appendFourCC(phdrChunk, "phdr");
    appendU32LE(phdrChunk, static_cast<uint32_t>(phdrData.size()));
    phdrChunk.insert(phdrChunk.end(), phdrData.begin(), phdrData.end());

    std::vector<uint8_t> pdtaList;
    appendFourCC(pdtaList, "pdta");
    pdtaList.insert(pdtaList.end(), phdrChunk.begin(), phdrChunk.end());

    std::vector<uint8_t> pdtaChunk;
    appendFourCC(pdtaChunk, "LIST");
    appendU32LE(pdtaChunk, static_cast<uint32_t>(pdtaList.size()));
    pdtaChunk.insert(pdtaChunk.end(), pdtaList.begin(), pdtaList.end());

    std::vector<uint8_t> infoList; appendFourCC(infoList, "INFO");
    std::vector<uint8_t> infoChunk;
    appendFourCC(infoChunk, "LIST");
    appendU32LE(infoChunk, static_cast<uint32_t>(infoList.size()));
    infoChunk.insert(infoChunk.end(), infoList.begin(), infoList.end());

    std::vector<uint8_t> sdtaList; appendFourCC(sdtaList, "sdta");
    std::vector<uint8_t> sdtaChunk;
    appendFourCC(sdtaChunk, "LIST");
    appendU32LE(sdtaChunk, static_cast<uint32_t>(sdtaList.size()));
    sdtaChunk.insert(sdtaChunk.end(), sdtaList.begin(), sdtaList.end());

    std::vector<uint8_t> riffData;
    appendFourCC(riffData, "sfbk");
    riffData.insert(riffData.end(), infoChunk.begin(), infoChunk.end());
    riffData.insert(riffData.end(), sdtaChunk.begin(), sdtaChunk.end());
    riffData.insert(riffData.end(), pdtaChunk.begin(), pdtaChunk.end());

    std::vector<uint8_t> file;
    appendFourCC(file, "RIFF");
    appendU32LE(file, static_cast<uint32_t>(riffData.size()));
    file.insert(file.end(), riffData.begin(), riffData.end());
    return file;
}

fs::path writeTempBinary(const std::string& name, const std::vector<uint8_t>& data) {
    fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return p;
}

} // namespace

TEST_CASE("parseSf2PresetHeaders: reads name/bank/preset from a minimal SF2 file, "
          "excluding the terminal sentinel record and skipping INFO/sdta", "[config][sf2]")
{
    auto data = buildMinimalSf2({
        {"Grand Piano", 0, 0},
        {"Standard Kit", 0, 128}
    });
    fs::path p = writeTempBinary("fitom_test_minimal.sf2", data);

    auto presets = fitom::parseSf2PresetHeaders(p);
    REQUIRE(presets.size() == 2); // 終端センチネル(EOP)は含まれない
    CHECK(presets[0].name == "Grand Piano");
    CHECK(presets[0].bank == 0);
    CHECK(presets[0].preset == 0);
    CHECK(presets[1].name == "Standard Kit");
    CHECK(presets[1].bank == 128);
    CHECK(presets[1].preset == 0);
}

TEST_CASE("parseSf2PresetHeaders: gracefully returns empty for a non-SF2/nonexistent file",
          "[config][sf2]")
{
    fs::path p = writeTempBinary("fitom_test_not_sf2.sf2",
        {'N', 'O', 'P', 'E', '1', '2', '3', '4'});
    CHECK(fitom::parseSf2PresetHeaders(p).empty());
    CHECK(fitom::parseSf2PresetHeaders("/nonexistent/does_not_exist.sf2").empty());
}

TEST_CASE("Sf2BankRegistry: resolvePresetName resolves via phdr, returns false for "
          "unknown bank/program", "[config][sf2]")
{
    auto data = buildMinimalSf2({
        {"Grand Piano", 0, 0},
        {"Standard Kit", 0, 128}
    });
    fs::path p = writeTempBinary("fitom_test_registry.sf2", data);

    json arr = json::array({
        {{"bank", 0}, {"file", p.filename().string()}, {"sf2_bank", 0}},
        {{"bank", 1}, {"file", p.filename().string()}, {"sf2_bank", 128}},
    });

    fitom::Sf2BankRegistry reg;
    reg.load(arr, p.parent_path());

    std::string name;
    REQUIRE(reg.resolvePresetName(0, 0, name));
    CHECK(name == "Grand Piano");

    REQUIRE(reg.resolvePresetName(1, 0, name));
    CHECK(name == "Standard Kit");

    CHECK_FALSE(reg.resolvePresetName(0, 99, name)); // sf2_bank=0内に存在しないprogram
    CHECK_FALSE(reg.resolvePresetName(99, 0, name)); // sf2_banksに無いCC#32値
}

TEST_CASE("FITOMConfig: getSf2BankRegistry().resolvePresetName() works end-to-end "
          "through loadProfile()", "[config][sf2]")
{
    fs::path dir = fs::temp_directory_path() / "fitom_test_sf2_presetname_e2e";
    fs::create_directories(dir);

    auto data = buildMinimalSf2({{"Grand Piano", 0, 0}});
    fs::path sf2Path = dir / "orchestral.sf2";
    { std::ofstream f(sf2Path, std::ios::binary);
      f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())); }

    json profile = {
        {"profile_name", "test"},
        {"devices", json::array({
            {{"if", "HW"}, {"chip", "SF2"}, {"plugin", "NoSuchPlugin"}}
        })},
        {"banks", {
            {"sf2_banks", json::array({
                {{"bank", 0}, {"file", "orchestral.sf2"}, {"sf2_bank", 0}}
            })}
        }}
    };
    fs::path profilePath = dir / "sf2_presetname.profile.json";
    { std::ofstream f(profilePath); f << profile.dump(2); }

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(profilePath));

    std::string name;
    REQUIRE(cfg.getSf2BankRegistry().resolvePresetName(0, 0, name));
    CHECK(name == "Grand Piano");
}

TEST_CASE("Sf2BankRegistry: listBanks() enumerates configured banks in ascending order, "
          "listPresetsInBank()/listPresetsByIndex() enumerate presets sorted by program",
          "[config][sf2]")
{
    auto data = buildMinimalSf2({
        {"Grand Piano", 0, 0},
        {"Standard Kit", 0, 128},
        {"Bright Piano", 3, 0},
    });
    fs::path p = writeTempBinary("fitom_test_sf2_listbanks.sf2", data);

    json arr = json::array({
        {{"bank", 5}, {"file", p.filename().string()}, {"sf2_bank", 0}},
        {{"bank", 2}, {"file", p.filename().string()}, {"sf2_bank", 128}},
    });

    fitom::Sf2BankRegistry reg;
    reg.load(arr, p.parent_path());

    const auto& banks = reg.listBanks();
    REQUIRE(banks.size() == 2);
    // bank番号(CC#32インデックス)の昇順であること
    CHECK(banks[0].bank == 2);
    CHECK(banks[0].sf2Bank == 128);
    CHECK(banks[1].bank == 5);
    CHECK(banks[1].sf2Bank == 0);
    CHECK(banks[0].soundfontIndex == banks[1].soundfontIndex); // 同一fileを共有

    const auto presetsBank5 = reg.listPresetsInBank(5); // sf2_bank=0側
    REQUIRE(presetsBank5.size() == 2);
    CHECK(presetsBank5[0].preset == 0);
    CHECK(presetsBank5[0].name == "Grand Piano");
    CHECK(presetsBank5[1].preset == 3);
    CHECK(presetsBank5[1].name == "Bright Piano");

    const auto presetsBank2 = reg.listPresetsInBank(2); // sf2_bank=128側
    REQUIRE(presetsBank2.size() == 1);
    CHECK(presetsBank2[0].name == "Standard Kit");

    CHECK(reg.listPresetsInBank(99).empty()); // 未設定のCC#32値

    // soundfontIndex/sf2Bankを直接指定する版も同じ結果を返す
    const auto viaIndex = reg.listPresetsByIndex(banks[1].soundfontIndex, 0);
    REQUIRE(viaIndex.size() == 2);
    CHECK(viaIndex[0].name == "Grand Piano");
}

// パッチエディタ永続化用SysEx(sub-cmd 0x06、docs/manuals/midi-message-reference.md
// 8.1節)のマージロジック。null/{}=現状維持、"remove"=デフォルト値へリセット、
// オブジェクト=差分マージの3通りを検証する。
TEST_CASE("PatchManager::mergePatchFromJsonText: partial merge, skip, and \"remove\" "
          "sentinel on layers[]", "[config][patch][sysex]")
{
    fitom::PatchManager pm;

    fitom::Patch patch;
    patch.id = 1; // isValid()にはid != 0xFFFFFFFFuが必要
    std::strncpy(patch.name, "Test Patch", sizeof(patch.name) - 1);
    patch.layers[0].voicePatchType = 1;
    patch.layers[0].hwBank = 0;
    patch.layers[0].hwProg = 5;
    patch.layers[0].enabled = true;
    patch.layers[1].voicePatchType = 2;
    patch.layers[1].hwBank = 1;
    patch.layers[1].hwProg = 3;
    patch.layers[1].enabled = true;

    pm.getPatchBank(10).set(0, patch);
    fitom::PatchBank* bank = pm.findMutablePatchBank(10);
    REQUIRE(bank != nullptr);
    fitom::Patch& target = bank->patches[0];

    json j;
    j["poly"] = 4;
    j["layers"] = json::array({
        json{{"pan_offset", -10}}, // layer0: 部分マージ(hw_prog等は現状維持)
        nullptr,                   // layer1: 現状維持(null=スキップ)
        "remove"                   // layer2: 元々未設定だがremove指定(既定値のまま)
    });
    std::string err;
    REQUIRE(pm.mergePatchFromJsonText(j.dump(), target, &err));

    CHECK(target.poly == 4);
    CHECK(target.layers[0].panOffset == -10);
    CHECK(target.layers[0].hwProg == 5);      // 変更していないフィールドは維持される
    CHECK(target.layers[1].hwProg == 3);      // null指定なので変更なし
    CHECK(target.layers[1].enabled == true);
    CHECK(target.layers[2].enabled == false); // 既定値のまま

    // 既に設定済みのレイヤーを"remove"すると、デフォルト値へリセットされる
    json j2;
    j2["layers"] = json::array({ nullptr, "remove" });
    REQUIRE(pm.mergePatchFromJsonText(j2.dump(), target, &err));
    CHECK(target.layers[1].enabled == false);
    CHECK(target.layers[1].hwProg == 0);
    CHECK(target.layers[0].hwProg == 5); // layer0はnullなので維持
}

// パッチエディタ永続化用SysEx(sub-cmd 0x07)のマージロジック。ノート単位の
// 微調整(部分マージ)・削除("remove")・新規追加が"notes"オブジェクト
// (キー=ノート番号文字列)1つで表現できることを検証する。
TEST_CASE("PatchManager::mergeDrumPatchFromJsonText: partial merge, skip, and \"remove\" "
          "sentinel on notes{}", "[config][drum][sysex]")
{
    fitom::PatchManager pm;

    fitom::DrumPatch dp;
    dp.id = 1;
    std::strncpy(dp.name, "Test Kit", sizeof(dp.name) - 1);
    dp.notes[35].enabled   = true;
    dp.notes[35].patchBank = 0;
    dp.notes[35].patchProg = 0;
    dp.notes[35].playNote  = 24;
    dp.notes[42].enabled   = true;
    dp.notes[42].patchBank = 0;
    dp.notes[42].patchProg = 1;
    dp.notes[42].playNote  = 61;

    pm.drumRegistry().getOrCreate(20).set(0, dp);
    fitom::DrumPatchBank* bank = pm.drumRegistry().findMutable(20);
    REQUIRE(bank != nullptr);
    fitom::DrumPatch& target = bank->patches[0];

    json notes = json::object();
    notes["35"] = json{{"pan", 10}};   // 部分マージ(既存フィールドは維持)
    notes["42"] = "remove";            // 削除
    notes["46"] = json{{"patch_bank", 0}, {"patch_prog", 1},
                        {"play_note", 66}, {"enabled", true}}; // 新規追加

    json j;
    j["choke_groups"] = json::array({ json::array({42, 46}) });
    j["notes"] = notes;

    std::string err;
    REQUIRE(pm.mergeDrumPatchFromJsonText(j.dump(), target, &err));

    REQUIRE(target.chokeGroups.size() == 1);
    CHECK(target.chokeGroups[0] == std::vector<uint8_t>{42, 46});

    CHECK(target.notes[35].pan == 10);
    CHECK(target.notes[35].playNote == 24); // 変更していないフィールドは維持される

    CHECK(target.notes[42].enabled == false); // removeされ、既定値にリセットされた
    CHECK(target.notes[42].playNote == 60);   // DrumNoteの既定値(remove前は61だった)

    CHECK(target.notes[46].enabled == true);
    CHECK(target.notes[46].playNote == 66);
    CHECK(target.notes[46].patchProg == 1);
}
