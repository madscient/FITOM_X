// tests/test_hwport.cpp
// HWPluginInstance / HWPort のテスト。HW プラグインには、音を出さない
// 検証用プラグイン (tests/stub_hwplugin) を使う。DLL のパスはビルド時に
// tests/CMakeLists.txt がコンパイル定義として渡す。

#include <catch2/catch_test_macros.hpp>

#include "fitom/HWPort.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::shared_ptr<fitom::HWPluginInstance> loadStub(const char* path)
{
    REQUIRE(fs::exists(path));
    auto plugin = fitom::HWPluginInstance::load(path);
    REQUIRE(plugin->init("") == HW_OK);
    return plugin;
}

std::vector<std::string> partNames(const fitom::HWPort& port)
{
    std::vector<std::string> names;
    for (const auto& p : port.getParts()) names.push_back(p.name);
    return names;
}

} // namespace

TEST_CASE("HWPort: part gains are relayed to a plugin that exports the group", "[hwport]")
{
    auto plugin = loadStub(FITOM_TEST_HWPLUGIN_PARTS);
    REQUIRE(plugin->GetPartCount != nullptr);

    fitom::HWPort opna(plugin, R"({"chip":"OPNA"})");
    fitom::HWPort opl3(plugin, R"({"chip":"OPL3"})");
    fitom::HWPort opm (plugin, R"({"chip":"OPM"})");

    SECTION("parts are enumerated by name") {
        CHECK(partNames(opna) == std::vector<std::string>{"FM", "SSG"});
        CHECK(partNames(opl3) == std::vector<std::string>{"AB", "CD"});
        CHECK(opm.getParts().empty());
    }

    SECTION("defaults captured at open survive a later change") {
        REQUIRE(opna.getParts().size() == 2);
        REQUIRE(opna.setPartGain("SSG", 0.5f, 0.75f) == HW_OK);
        const auto& ssg = opna.getParts()[1];
        CHECK(ssg.name == "SSG");
        CHECK(ssg.defaultL == 0.25f);
        CHECK(ssg.defaultR == 0.25f);

        REQUIRE(opl3.getParts().size() == 2);
        CHECK(opl3.getParts()[1].name == "CD");
        CHECK(opl3.getParts()[1].defaultL == 0.0f);
        CHECK(opl3.getParts()[1].defaultR == 0.0f);
    }

    SECTION("defaults come from the plugin, not from a 1.0 fallback") {
        float l = -1.f, r = -1.f;
        REQUIRE(opna.getPartGain("SSG", l, r) == HW_OK);
        CHECK(l == 0.25f);
        CHECK(r == 0.25f);

        l = r = -1.f;
        REQUIRE(opl3.getPartGain("CD", l, r) == HW_OK);
        CHECK(l == 0.0f);
        CHECK(r == 0.0f);
    }

    SECTION("a gain set through the port is read back, L and R independently") {
        REQUIRE(opna.setPartGain("SSG", 0.5f, 0.75f) == HW_OK);

        float l = -1.f, r = -1.f;
        REQUIRE(opna.getPartGain("SSG", l, r) == HW_OK);
        CHECK(l == 0.5f);
        CHECK(r == 0.75f);

        // 同じデバイスの別の部位と、別のデバイスには波及しない
        l = r = -1.f;
        REQUIRE(opna.getPartGain("FM", l, r) == HW_OK);
        CHECK(l == 1.0f);
        CHECK(r == 1.0f);

        l = r = -1.f;
        REQUIRE(opl3.getPartGain("AB", l, r) == HW_OK);
        CHECK(l == 1.0f);
        CHECK(r == 1.0f);
    }

    SECTION("a name the device does not have is rejected") {
        float l = -1.f, r = -1.f;
        CHECK(opna.setPartGain("AB", 0.5f, 0.5f)  == HW_ERR_INVALID_ARG);  // OPL3 の部位
        CHECK(opna.setPartGain("ssg", 0.5f, 0.5f) == HW_ERR_INVALID_ARG);  // 大文字小文字を区別
        CHECK(opna.getPartGain("AB", l, r)        == HW_ERR_INVALID_ARG);
        CHECK(opm.setPartGain("FM", 0.5f, 0.5f)   == HW_ERR_INVALID_ARG);  // 部位を持たないチップ

        REQUIRE(opna.getPartGain("SSG", l, r) == HW_OK);
        CHECK(l == 0.25f);
        CHECK(r == 0.25f);
    }
}

TEST_CASE("HWPort: a plugin without the part gain group has no parts", "[hwport]")
{
    auto plugin = loadStub(FITOM_TEST_HWPLUGIN_NO_PARTS);
    CHECK(plugin->GetPartCount == nullptr);
    CHECK(plugin->GetPartName  == nullptr);
    CHECK(plugin->SetPartGain  == nullptr);
    CHECK(plugin->GetPartGain  == nullptr);

    // 同じ "OPNA" でも、組をエクスポートしないプラグインでは部位が無い
    fitom::HWPort opna(plugin, R"({"chip":"OPNA"})");
    CHECK(opna.getParts().empty());

    float l = -1.f, r = -1.f;
    CHECK(opna.setPartGain("SSG", 0.5f, 0.5f) == HW_ERR_INVALID_ARG);
    CHECK(opna.getPartGain("SSG", l, r)       == HW_ERR_INVALID_ARG);
    CHECK(l == -1.f);
    CHECK(r == -1.f);
}

TEST_CASE("HWPluginInstance: an incomplete part gain group fails to load", "[hwport]")
{
    REQUIRE(fs::exists(FITOM_TEST_HWPLUGIN_HEAD_ONLY));
    REQUIRE_THROWS_AS(
        fitom::HWPluginInstance::load(FITOM_TEST_HWPLUGIN_HEAD_ONLY),
        std::runtime_error);
}
