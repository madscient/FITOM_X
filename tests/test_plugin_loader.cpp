// tests/test_plugin_loader.cpp
// PluginLoader のユニットテスト

#include <catch2/catch_test_macros.hpp>
#include "fitom/PluginLoader.h"
#include <filesystem>

namespace fs = std::filesystem;

TEST_CASE("PluginLoader: throws on nonexistent DLL", "[plugin_loader]")
{
    REQUIRE_THROWS_AS(
        fitom::PluginLoader::load("nonexistent_does_not_exist.dll"),
        std::runtime_error);
}

TEST_CASE("PluginLoader: move semantics", "[plugin_loader]")
{
    // ロードできるものがないため、ムーブ後の状態のみ確認
    fitom::PluginLoader pl1;
    CHECK_FALSE(pl1.isLoaded());

    fitom::PluginLoader pl2 = std::move(pl1);
    CHECK_FALSE(pl2.isLoaded());
    CHECK_FALSE(pl1.isLoaded());
}
