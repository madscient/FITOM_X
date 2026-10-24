// tests/test_offline_render.cpp
// HWプラグインのオフライン描画(HWPlugin_InitOffline/HWPlugin_Render)を
// HWPluginRegistry が扱うことのテスト。検証用プラグインの HWPlugin_Render は
// L に書き込み回数、R に 0.25 を返す(stub_hwplugin/StubHWPlugin.cpp 冒頭参照)。

#include <catch2/catch_test_macros.hpp>

#include "fitom/HWPort.h"
#include "fitom/PluginLoader.h"
#include <fitom/IHWPlugin.h>

#include <string>
#include <vector>

using namespace fitom;

namespace {

uint32_t stubOfflineRate()
{
    using PFN = uint32_t (FITOM_HWP_CALL*)();
    static PluginLoader loader = PluginLoader::load(FITOM_TEST_HWPLUGIN_PARTS);
    return loader.symRequired<PFN>("StubHWPlugin_GetOfflineRate")();
}

} // namespace

TEST_CASE("Offline registry initializes plugins with the requested sample rate",
          "[offline_render]")
{
    HWPluginRegistry reg;
    reg.setOfflineSampleRate(48000);
    reg.registerPlugin("emu", FITOM_TEST_HWPLUGIN_PARTS);

    CHECK(reg.failedPlugins().empty());
    CHECK(reg.registeredCount() == 1);
    CHECK(stubOfflineRate() == 48000);
}

TEST_CASE("Offline registry rejects a plugin without offline rendering", "[offline_render]")
{
    HWPluginRegistry reg;
    reg.setOfflineSampleRate(44100);
    reg.registerPlugin("chip", FITOM_TEST_HWPLUGIN_NO_PARTS);

    CHECK(reg.registeredCount() == 0);
    CHECK(reg.failedPlugins() == std::vector<std::string>{"chip"});
}

TEST_CASE("A plugin exporting HWPlugin_InitOffline without HWPlugin_Render fails to load",
          "[offline_render]")
{
    // オフラインでなくてもロード時点で落ちる(組の検査はロード時に行う)。
    HWPluginRegistry reg;
    reg.registerPlugin("broken", FITOM_TEST_HWPLUGIN_OFFLINE_HEAD_ONLY);

    CHECK(reg.registeredCount() == 0);
    CHECK(reg.failedPlugins() == std::vector<std::string>{"broken"});
}

TEST_CASE("Offline rendering sums every plugin and reflects writes made before the call",
          "[offline_render]")
{
    HWPluginRegistry reg;
    reg.setOfflineSampleRate(44100);
    // 同じDLLを2つの名前で登録する。モジュールの状態は共有なので、両者は同じ値を返す。
    reg.registerPlugin("a", FITOM_TEST_HWPLUGIN_PARTS);
    reg.registerPlugin("b", FITOM_TEST_HWPLUGIN_PARTS);
    REQUIRE(reg.registeredCount() == 2);

    HWPort port(reg.get("a"), R"({"chip":"OPM","index":0})");

    std::vector<float> buf(4 * 2, -1.0f);
    REQUIRE(reg.renderOffline(buf.data(), 4) == HW_OK);
    const float before = buf[0];
    CHECK(buf[1] == 0.5f);

    port.write(0x08, 0x00);
    REQUIRE(reg.renderOffline(buf.data(), 4) == HW_OK);
    for (int i = 0; i < 4; ++i) {
        CHECK(buf[i * 2 + 0] == before + 2.0f);
        CHECK(buf[i * 2 + 1] == 0.5f);
    }
}

TEST_CASE("Rendering is refused when the registry is not in offline mode", "[offline_render]")
{
    HWPluginRegistry reg;
    reg.registerPlugin("a", FITOM_TEST_HWPLUGIN_PARTS);
    std::vector<float> buf(2);
    CHECK(reg.renderOffline(buf.data(), 1) == HW_ERR_INVALID_ARG);
}
