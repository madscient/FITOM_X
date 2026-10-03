// tests/test_part_gain.cpp
// プロファイルの part_gains (部位ごとのゲイン) の読み込み・適用・書き戻しのテスト。
// HW プラグインには検証用プラグイン (tests/stub_hwplugin) を使う。
//
// 検証用プラグインの既定値: OPNA は FM=(1.0, 1.0) / SSG=(0.25, 0.25)、
// OPL3 は AB=(1.0, 1.0) / CD=(0.0, 0.0)。

#include <catch2/catch_test_macros.hpp>

#include "fitom/Config.h"
#include "fitom/HWPort.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path writeTempJson(const std::string& name, const json& j)
{
    fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p);
    f << j.dump(2);
    return p;
}

json readJson(const fs::path& p)
{
    std::ifstream f(p);
    return json::parse(f);
}

// HWPlugin_Open に渡る params。HWPort::getPhysicalChipName() が
// "Stub/<chip>" (index>0 なら " #<index>" が付く) を返す形にしてある。
json params(const char* chip, int index)
{
    return { {"type", "FMHWIF"}, {"engine", "Stub"}, {"chip", chip}, {"index", index} };
}

json device(const char* label, const char* chip, int index)
{
    json d = params(chip, index);
    d["if"]     = "HW";
    d["plugin"] = "stub";
    d["label"]  = label;
    return d;
}

json makeProfile(const char* dll, const json& devices, const json& partGains = nullptr,
                 bool autoDevices = false)
{
    json plugin = { {"name", "stub"}, {"dll", dll} };
    if (autoDevices) plugin["auto_devices"] = true;
    json p = {
        {"profile_name", "part gain test"},
        {"hw_plugins",   json::array({ plugin })},
        {"devices",      devices}
    };
    if (!partGains.is_null()) p["part_gains"] = partGains;
    return p;
}

// 同種デバイス自動束ねで代表エントリへ吸収されたデバイスのポートも拾う。
fitom::HWPort* findPort(const fitom::FITOMConfig& cfg, const std::string& physicalName)
{
    auto match = [&](fitom::IPort* p) -> fitom::HWPort* {
        auto* hw = dynamic_cast<fitom::HWPort*>(p);
        return (hw && hw->getPhysicalChipName() == physicalName) ? hw : nullptr;
    };
    for (int i = 0; i < cfg.getDeviceCount(); ++i) {
        if (auto* hw = match(cfg.getDevicePort(i))) return hw;
        for (int k = 0; k < cfg.getDeviceSpanGroupCount(i); ++k) {
            if (auto* hw = match(cfg.getDeviceSpanGroupPrimary(i, k))) return hw;
        }
    }
    return nullptr;
}

struct Gain {
    float l = -1.f;
    float r = -1.f;
    bool operator==(const Gain& o) const { return l == o.l && r == o.r; }
};

Gain gainOf(const fitom::HWPort* port, const char* part)
{
    Gain g;
    REQUIRE(port != nullptr);
    REQUIRE(port->getPartGain(part, g.l, g.r) == HW_OK);
    return g;
}

} // namespace

TEST_CASE("part_gains: saved gains are applied to the matching device at load", "[part_gain]")
{
    // ラベルは意図的に重複させる。ラベルではなく params で1台に決まること。
    json profile = makeProfile(FITOM_TEST_HWPLUGIN_PARTS,
        json::array({ device("OPNA", "OPNA", 0), device("OPNA", "OPNA", 1),
                      device("OPL3", "OPL3", 0) }),
        json::array({
            { {"plugin", "stub"}, {"params", params("OPNA", 1)},
              {"gains", { {"SSG", {0.5, 0.75}} }} }
        }));
    fs::path p = writeTempJson("fitom_test_part_gain_apply.profile.json", profile);

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));

    CHECK(gainOf(findPort(cfg, "Stub/OPNA #1"), "SSG") == Gain{0.5f, 0.75f});
    CHECK(gainOf(findPort(cfg, "Stub/OPNA #1"), "FM")  == Gain{1.0f, 1.0f});
    CHECK(gainOf(findPort(cfg, "Stub/OPNA"),    "SSG") == Gain{0.25f, 0.25f});
    CHECK(gainOf(findPort(cfg, "Stub/OPL3"),    "CD")  == Gain{0.0f, 0.0f});
}

TEST_CASE("part_gains: a changed gain is written back, a default one is not", "[part_gain]")
{
    json profile = makeProfile(FITOM_TEST_HWPLUGIN_PARTS,
        json::array({ device("OPNA", "OPNA", 0), device("OPNA", "OPNA", 1) }));
    fs::path p   = writeTempJson("fitom_test_part_gain_save.profile.json", profile);
    fs::path out = fs::temp_directory_path() / "fitom_test_part_gain_save.out.profile.json";

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));
    fitom::HWPort* opna0 = findPort(cfg, "Stub/OPNA");
    REQUIRE(opna0 != nullptr);

    SECTION("nothing changed: no part_gains key is written") {
        REQUIRE(cfg.saveProfile(out));
        CHECK_FALSE(readJson(out).contains("part_gains"));
    }

    SECTION("a changed part is saved under the device's plugin and params") {
        REQUIRE(cfg.setPartGain(opna0, "SSG", 0.37f, 0.5f) == HW_OK);
        CHECK(gainOf(opna0, "SSG") == Gain{0.37f, 0.5f});
        REQUIRE(cfg.saveProfile(out));

        json saved = readJson(out);
        json expected = json::array({
            { {"plugin", "stub"}, {"params", params("OPNA", 0)},
              {"gains", { {"SSG", {0.37, 0.5}} }} }
        });
        // 0.37f を double へ広げたままの値 (0.3700000047683716) ではなく、
        // 0.37 として書かれること。
        CHECK(saved["part_gains"] == expected);

        // 他のフィールドはロード時の内容のまま
        CHECK(saved["devices"]      == profile["devices"]);
        CHECK(saved["hw_plugins"]   == profile["hw_plugins"]);
        CHECK(saved["profile_name"] == profile["profile_name"]);

        // 書き戻したファイルを読み直すと、同じ float が同じデバイスに掛かる
        fitom::FITOMConfig reloaded;
        REQUIRE(reloaded.loadProfile(out));
        CHECK(gainOf(findPort(reloaded, "Stub/OPNA"),    "SSG") == Gain{0.37f, 0.5f});
        CHECK(gainOf(findPort(reloaded, "Stub/OPNA #1"), "SSG") == Gain{0.25f, 0.25f});
    }

    SECTION("setting a part back to the plugin default removes it from the file") {
        REQUIRE(cfg.setPartGain(opna0, "SSG", 0.5f, 0.5f) == HW_OK);
        REQUIRE(cfg.setPartGain(opna0, "FM",  0.8f, 0.8f) == HW_OK);
        REQUIRE(cfg.setPartGain(opna0, "SSG", 0.25f, 0.25f) == HW_OK);
        REQUIRE(cfg.saveProfile(out));
        json saved = readJson(out);
        REQUIRE(saved["part_gains"].size() == 1);
        CHECK(saved["part_gains"][0]["gains"] == json({ {"FM", {0.8, 0.8}} }));

        REQUIRE(cfg.setPartGain(opna0, "FM", 1.0f, 1.0f) == HW_OK);
        REQUIRE(cfg.saveProfile(out));
        CHECK_FALSE(readJson(out).contains("part_gains"));
    }
}

TEST_CASE("part_gains: entries for devices that are not open are kept", "[part_gain]")
{
    json absentDevice = { {"plugin", "stub"}, {"params", params("OPNA", 9)},
                          {"gains", { {"SSG", {0.5, 0.5}} }} };
    json absentPlugin = { {"plugin", "not_loaded"}, {"params", params("OPNA", 0)},
                          {"gains", { {"FM", {0.8, 0.8}} }} };
    json profile = makeProfile(FITOM_TEST_HWPLUGIN_PARTS,
        json::array({ device("OPNA", "OPNA", 0) }),
        json::array({ absentDevice, absentPlugin }));
    fs::path p   = writeTempJson("fitom_test_part_gain_keep.profile.json", profile);
    fs::path out = fs::temp_directory_path() / "fitom_test_part_gain_keep.out.profile.json";

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));

    // 開いているデバイスには、他のデバイス宛ての設定は掛からない
    fitom::HWPort* opna0 = findPort(cfg, "Stub/OPNA");
    CHECK(gainOf(opna0, "SSG") == Gain{0.25f, 0.25f});
    CHECK(gainOf(opna0, "FM")  == Gain{1.0f, 1.0f});

    REQUIRE(cfg.setPartGain(opna0, "SSG", 0.5f, 0.5f) == HW_OK);
    REQUIRE(cfg.saveProfile(out));

    json saved = readJson(out)["part_gains"];
    REQUIRE(saved.size() == 3);
    CHECK(saved[0] == absentDevice);
    CHECK(saved[1] == absentPlugin);
    CHECK(saved[2]["params"] == params("OPNA", 0));
}

TEST_CASE("part_gains: applies to devices enumerated by auto_devices", "[part_gain]")
{
    // 検証用プラグインは {"chip":"OPNA","index":0} を1台だけ列挙する。
    json autoParams = { {"chip", "OPNA"}, {"index", 0} };
    json profile = makeProfile(FITOM_TEST_HWPLUGIN_PARTS, json::array(),
        json::array({
            { {"plugin", "stub"}, {"params", autoParams}, {"gains", { {"SSG", {0.5, 0.5}} }} }
        }),
        /*autoDevices=*/true);
    fs::path p = writeTempJson("fitom_test_part_gain_auto.profile.json", profile);

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));
    REQUIRE(cfg.getDeviceCount() > 0);

    auto* port = dynamic_cast<fitom::HWPort*>(cfg.getDevicePort(0));
    CHECK(gainOf(port, "SSG") == Gain{0.5f, 0.5f});
    CHECK(gainOf(port, "FM")  == Gain{1.0f, 1.0f});
}

TEST_CASE("part_gains: rejected settings are not recorded", "[part_gain]")
{
    json profile = makeProfile(FITOM_TEST_HWPLUGIN_PARTS,
        json::array({ device("OPNA", "OPNA", 0) }));
    fs::path p   = writeTempJson("fitom_test_part_gain_reject.profile.json", profile);
    fs::path out = fs::temp_directory_path() / "fitom_test_part_gain_reject.out.profile.json";

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));
    fitom::HWPort* opna0 = findPort(cfg, "Stub/OPNA");
    REQUIRE(opna0 != nullptr);

    CHECK(cfg.setPartGain(nullptr, "SSG", 0.5f, 0.5f) == HW_ERR_INVALID_ARG);
    CHECK(cfg.setPartGain(opna0, "NoSuchPart", 0.5f, 0.5f) == HW_ERR_INVALID_ARG);

    // このConfigが開いたものではないポートには触らない
    auto plugin = cfg.getHWPluginRegistry().get("stub");
    REQUIRE(plugin != nullptr);
    fitom::HWPort foreign(plugin, params("OPNA", 5).dump());
    CHECK(cfg.setPartGain(&foreign, "SSG", 0.5f, 0.5f) == HW_ERR_INVALID_ARG);
    CHECK(gainOf(&foreign, "SSG") == Gain{0.25f, 0.25f});

    REQUIRE(cfg.saveProfile(out));
    CHECK_FALSE(readJson(out).contains("part_gains"));
}

TEST_CASE("part_gains: malformed entries are ignored without failing the load", "[part_gain]")
{
    json profile = makeProfile(FITOM_TEST_HWPLUGIN_PARTS,
        json::array({ device("OPNA", "OPNA", 0) }),
        json::array({
            42,
            { {"plugin", "stub"} },
            { {"plugin", "stub"}, {"params", params("OPNA", 0)},
              {"gains", { {"SSG", 0.5}, {"FM", {0.5, 0.5}} }} }
        }));
    fs::path p = writeTempJson("fitom_test_part_gain_malformed.profile.json", profile);

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));

    fitom::HWPort* opna0 = findPort(cfg, "Stub/OPNA");
    CHECK(gainOf(opna0, "FM")  == Gain{0.5f, 0.5f});
    CHECK(gainOf(opna0, "SSG") == Gain{0.25f, 0.25f});   // [L, R] でない値は無視
}

TEST_CASE("part_gains: a plugin without the part gain group keeps the saved entry", "[part_gain]")
{
    json entry = { {"plugin", "stub"}, {"params", params("OPNA", 0)},
                   {"gains", { {"SSG", {0.5, 0.5}} }} };
    json profile = makeProfile(FITOM_TEST_HWPLUGIN_NO_PARTS,
        json::array({ device("OPNA", "OPNA", 0) }), json::array({ entry }));
    fs::path p   = writeTempJson("fitom_test_part_gain_noparts.profile.json", profile);
    fs::path out = fs::temp_directory_path() / "fitom_test_part_gain_noparts.out.profile.json";

    fitom::FITOMConfig cfg;
    REQUIRE(cfg.loadProfile(p));
    fitom::HWPort* opna0 = findPort(cfg, "Stub/OPNA");
    REQUIRE(opna0 != nullptr);
    CHECK(opna0->getParts().empty());

    REQUIRE(cfg.saveProfile(out));
    CHECK(readJson(out)["part_gains"] == json::array({ entry }));
}
