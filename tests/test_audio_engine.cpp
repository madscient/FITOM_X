// tests/test_audio_engine.cpp
// AudioEngine の基本動作テスト

#include <catch2/catch_test_macros.hpp>

#ifdef FITOM_HAS_RTAUDIO
#include "fitom/AudioEngine.h"

TEST_CASE("AudioEngine: enumerateDevices does not throw", "[audio]")
{
    std::vector<std::string> devices;
    REQUIRE_NOTHROW(devices = fitom::AudioEngine::enumerateDevices("auto"));
    // デバイスが 0 件でもクラッシュしないことを確認
    SUCCEED("enumerateDevices returned " + std::to_string(devices.size()) + " devices");
}

#else
TEST_CASE("AudioEngine: not available (RtAudio not found)", "[audio]")
{
    SUCCEED("RtAudio not found — AudioEngine tests skipped");
}
#endif
