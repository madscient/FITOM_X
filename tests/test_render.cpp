// tests/test_render.cpp
// SMF→WAV 変換器(apps/fitom_render)の部品: SMFの読み込み、WAVの書き出し、
// 描画したサンプル数で時間を進める処理。

#include <catch2/catch_test_macros.hpp>

#include "SmfReader.h"
#include "WavWriter.h"
#include "OfflineRenderer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fitom::render;

namespace {

using Bytes = std::vector<uint8_t>;

void append(Bytes& b, std::initializer_list<int> v)
{
    for (int x : v) b.push_back(static_cast<uint8_t>(x));
}

void append32(Bytes& b, uint32_t v)
{
    append(b, {int(v >> 24) & 0xFF, int(v >> 16) & 0xFF, int(v >> 8) & 0xFF, int(v) & 0xFF});
}

Bytes smf(int format, uint16_t division, const std::vector<Bytes>& tracks)
{
    Bytes b;
    append(b, {'M', 'T', 'h', 'd'});
    append32(b, 6);
    append(b, {0, format, 0, static_cast<int>(tracks.size()), division >> 8, division & 0xFF});
    for (const auto& t : tracks) {
        append(b, {'M', 'T', 'r', 'k'});
        append32(b, static_cast<uint32_t>(t.size()));
        b.insert(b.end(), t.begin(), t.end());
    }
    return b;
}

const Bytes kEndOfTrack = {0x00, 0xFF, 0x2F, 0x00};

Bytes track(std::initializer_list<int> v)
{
    Bytes b;
    append(b, v);
    b.insert(b.end(), kEndOfTrack.begin(), kEndOfTrack.end());
    return b;
}

Bytes readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint32_t le32(const Bytes& b, std::size_t at)
{
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

uint16_t le16(const Bytes& b, std::size_t at)
{
    return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
}

} // namespace

// ─── SMF ────────────────────────────────────────────────────────────────────

TEST_CASE("SMF format 0 converts ticks to microseconds at the default tempo", "[render][smf]")
{
    // 480 tick/四分音符、テンポ指定なし(120BPM): 480 tick = 500000us
    auto song = parseSmf(smf(0, 480, {track({0x00, 0x90, 0x3C, 0x64,
                                              0x83, 0x60, 0x80, 0x3C, 0x00})}));
    REQUIRE(song.events.size() == 2);
    CHECK(song.events[0].timeUs == 0);
    CHECK(song.events[0].bytes == Bytes{0x90, 0x3C, 0x64});
    CHECK(song.events[1].timeUs == 500000);
    CHECK(song.events[1].bytes == Bytes{0x80, 0x3C, 0x00});
    CHECK(song.endTimeUs == 500000);
}

TEST_CASE("SMF tempo changes apply from their tick onward", "[render][smf]")
{
    // tick 0 で 250000us/四分音符、tick 480 で 1000000us/四分音符
    auto song = parseSmf(smf(0, 480, {track({
        0x00, 0xFF, 0x51, 0x03, 0x03, 0xD0, 0x90,
        0x83, 0x60, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40,
        0x00, 0x90, 0x3C, 0x64,
        0x83, 0x60, 0x80, 0x3C, 0x00})}));
    REQUIRE(song.events.size() == 2);
    CHECK(song.events[0].timeUs == 250000);
    CHECK(song.events[1].timeUs == 1250000);
}

TEST_CASE("SMF running status expands to full messages", "[render][smf]")
{
    auto song = parseSmf(smf(0, 96, {track({0x00, 0x90, 0x3C, 0x64,
                                             0x00, 0x40, 0x64,
                                             0x00, 0xC1, 0x05,
                                             0x00, 0x07})}));
    REQUIRE(song.events.size() == 4);
    CHECK(song.events[1].bytes == Bytes{0x90, 0x40, 0x64});
    CHECK(song.events[2].bytes == Bytes{0xC1, 0x05});
    CHECK(song.events[3].bytes == Bytes{0xC1, 0x07});
}

TEST_CASE("SMF meta events cancel running status", "[render][smf]")
{
    CHECK_THROWS_AS(parseSmf(smf(0, 96, {track({0x00, 0x90, 0x3C, 0x64,
                                                 0x00, 0xFF, 0x01, 0x00,
                                                 0x00, 0x3C, 0x00})})),
                    std::runtime_error);
}

TEST_CASE("SMF format 1 merges tracks by time, then by track order", "[render][smf]")
{
    auto song = parseSmf(smf(1, 96, {
        track({0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}),   // 500000us/四分音符
        track({0x60, 0x91, 0x40, 0x64}),                      // tick 96
        track({0x30, 0x92, 0x41, 0x64,                        // tick 48
               0x30, 0x92, 0x42, 0x64})                       // tick 96
    }));
    REQUIRE(song.events.size() == 3);
    CHECK(song.events[0].bytes[0] == 0x92);
    CHECK(song.events[0].timeUs == 250000);
    CHECK(song.events[1].bytes == Bytes{0x91, 0x40, 0x64});
    CHECK(song.events[2].bytes == Bytes{0x92, 0x42, 0x64});
    CHECK(song.events[1].timeUs == 500000);
    CHECK(song.events[2].timeUs == 500000);
}

TEST_CASE("SMF SysEx events are passed through with the leading F0", "[render][smf]")
{
    auto song = parseSmf(smf(0, 96, {track({0x00, 0xF0, 0x05, 0x7E, 0x7F, 0x09, 0x01, 0xF7,
                                             0x00, 0xF7, 0x02, 0x7F, 0xF7})}));
    REQUIRE(song.events.size() == 2);
    CHECK(song.events[0].bytes == Bytes{0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});
    CHECK(song.events[1].bytes == Bytes{0x7F, 0xF7});
}

TEST_CASE("SMF SMPTE division ignores tempo", "[render][smf]")
{
    // 25fps × 40 tick/フレーム = 1 tick あたり 1ms
    const uint16_t division = static_cast<uint16_t>((static_cast<uint8_t>(-25) << 8) | 40);
    auto song = parseSmf(smf(0, division, {track({0x00, 0xFF, 0x51, 0x03, 0x03, 0xD0, 0x90,
                                                   0x64, 0x90, 0x3C, 0x64})}));
    REQUIRE(song.events.size() == 1);
    CHECK(song.events[0].timeUs == 100000);
}

TEST_CASE("SMF MIDI Port meta event sets the port of later events in its own track", "[render][smf]")
{
    auto song = parseSmf(smf(1, 96, {
        track({0x00, 0x90, 0x3C, 0x64,                        // FF 21 の前: 0
               0x00, 0xFF, 0x21, 0x01, 0x02,
               0x00, 0x90, 0x3D, 0x64,                        // 2
               0x00, 0xF0, 0x02, 0x7E, 0xF7,                  // SysEx も 2
               0x00, 0xFF, 0x21, 0x02, 0x03, 0x00,            // 長さが1でなければ読まない
               0x00, 0x90, 0x3E, 0x64}),                      // 2 のまま
        track({0x00, 0x91, 0x40, 0x64})                       // 他のトラックの FF 21 は効かない
    }));
    REQUIRE(song.events.size() == 5);
    CHECK(song.events[0].port == 0);
    CHECK(song.events[1].port == 2);
    CHECK(song.events[2].bytes == Bytes{0xF0, 0x7E, 0xF7});
    CHECK(song.events[2].port == 2);
    CHECK(song.events[3].port == 2);
    CHECK(song.events[4].bytes == Bytes{0x91, 0x40, 0x64});
    CHECK(song.events[4].port == 0);
}

TEST_CASE("SMF events on ports outside the range are removed, and tempo still applies",
          "[render][smf]")
{
    auto song = parseSmf(smf(1, 96, {
        track({0x00, 0xFF, 0x21, 0x01, 0x04,
               0x00, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40,      // 1000000us/四分音符
               0x00, 0x90, 0x3C, 0x64,
               0x00, 0x90, 0x3D, 0x64}),
        track({0x00, 0xFF, 0x21, 0x01, 0x07,
               0x00, 0x91, 0x3C, 0x64}),
        track({0x00, 0xFF, 0x21, 0x01, 0x03,
               0x60, 0x92, 0x3C, 0x64})                       // tick 96
    }));
    const auto removed = removeEventsOutsidePorts(song, 4);
    CHECK(removed == std::map<uint8_t, std::size_t>{{4, 2}, {7, 1}});
    REQUIRE(song.events.size() == 1);
    CHECK(song.events[0].bytes == Bytes{0x92, 0x3C, 0x64});
    CHECK(song.events[0].port == 3);
    CHECK(song.events[0].timeUs == 1000000);
}

TEST_CASE("SMF rejects format 2 and truncated data", "[render][smf]")
{
    CHECK_THROWS_AS(parseSmf(smf(2, 96, {track({})})), std::runtime_error);

    Bytes broken = smf(0, 96, {track({0x00, 0x90, 0x3C, 0x64})});
    broken.resize(broken.size() - 6);   // MTrk の長さはそのまま、中身だけ欠ける
    CHECK_THROWS_AS(parseSmf(broken), std::runtime_error);

    CHECK_THROWS_AS(parseSmf(Bytes{'R', 'I', 'F', 'F'}), std::runtime_error);
}

// ─── WAV ────────────────────────────────────────────────────────────────────

TEST_CASE("WAV s16 writes a PCM header and clips samples", "[render][wav]")
{
    const auto path = std::filesystem::temp_directory_path() / "fitom_test_render_s16.wav";
    {
        WavWriter w(path, 22050, WavFormat::S16);
        const float s[] = {1.0f, -2.0f, 0.5f, 0.0f};
        w.write(s, 2);
        w.close();
    }
    const Bytes b = readFile(path);
    REQUIRE(b.size() == 44 + 8);
    CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
    CHECK(le32(b, 4) == b.size() - 8);
    CHECK(std::memcmp(b.data() + 8, "WAVEfmt ", 8) == 0);
    CHECK(le16(b, 20) == 1);        // PCM
    CHECK(le16(b, 22) == 2);        // ステレオ
    CHECK(le32(b, 24) == 22050);
    CHECK(le32(b, 28) == 22050 * 4);
    CHECK(le16(b, 32) == 4);
    CHECK(le16(b, 34) == 16);
    CHECK(std::memcmp(b.data() + 36, "data", 4) == 0);
    CHECK(le32(b, 40) == 8);
    CHECK(static_cast<int16_t>(le16(b, 44)) == 32767);
    CHECK(static_cast<int16_t>(le16(b, 46)) == -32767);
    CHECK(static_cast<int16_t>(le16(b, 48)) == 16384);
    CHECK(static_cast<int16_t>(le16(b, 50)) == 0);
}

TEST_CASE("WAV s24 packs three bytes per sample", "[render][wav]")
{
    const auto path = std::filesystem::temp_directory_path() / "fitom_test_render_s24.wav";
    {
        WavWriter w(path, 48000, WavFormat::S24);
        const float s[] = {1.0f, -1.0f};
        w.write(s, 1);
    }   // close() はデストラクタでも行われる
    const Bytes b = readFile(path);
    REQUIRE(b.size() == 44 + 6);
    CHECK(le16(b, 34) == 24);
    CHECK(le32(b, 40) == 6);
    CHECK(Bytes(b.begin() + 44, b.end()) == Bytes{0xFF, 0xFF, 0x7F, 0x01, 0x00, 0x80});
}

TEST_CASE("WAV f32 uses the IEEE float tag, a fact chunk, and keeps out-of-range values",
          "[render][wav]")
{
    const auto path = std::filesystem::temp_directory_path() / "fitom_test_render_f32.wav";
    {
        WavWriter w(path, 44100, WavFormat::F32);
        const float s[] = {1.5f, -0.25f, 0.0f, 0.0f, 0.0f, 0.0f};
        w.write(s, 3);
        w.close();
    }
    const Bytes b = readFile(path);
    // RIFF(12) + fmt(8+18) + fact(12) + data(8) + 3フレーム × 8バイト
    REQUIRE(b.size() == 58 + 24);
    CHECK(le32(b, 16) == 18);
    CHECK(le16(b, 20) == 3);
    CHECK(le16(b, 34) == 32);
    CHECK(std::memcmp(b.data() + 38, "fact", 4) == 0);
    CHECK(le32(b, 46) == 3);
    CHECK(std::memcmp(b.data() + 50, "data", 4) == 0);
    CHECK(le32(b, 54) == 24);
    float first;
    std::memcpy(&first, b.data() + 58, 4);
    CHECK(first == 1.5f);
}

TEST_CASE("WAV format names", "[render][wav]")
{
    CHECK(parseWavFormat("s16") == WavFormat::S16);
    CHECK(parseWavFormat("s24") == WavFormat::S24);
    CHECK(parseWavFormat("f32") == WavFormat::F32);
    CHECK_FALSE(parseWavFormat("S16").has_value());
}

// ─── 時間の進め方 ───────────────────────────────────────────────────────────

namespace {

struct Recorder {
    std::vector<std::string> log;
    uint64_t                 frames = 0;
    OfflineTarget            target;
    SampleSink               sink;

    Recorder() {
        target.sendMidi = [this](uint8_t port, const uint8_t* d, std::size_t) {
            log.push_back("midi" + std::to_string(d[1]) + "@" + std::to_string(frames)
                          + (port ? "p" + std::to_string(port) : ""));
        };
        target.tick   = [this]() { log.push_back("tick@" + std::to_string(frames)); };
        target.render = [](float* out, uint32_t n) {
            std::fill(out, out + n * 2, 0.0f);
            return true;
        };
        sink = [this](const float*, uint32_t n) { frames += n; };
    }
};

SmfEvent ev(uint64_t us, uint8_t note, uint8_t port = 0) { return {us, {0x90, note, 0x64}, port}; }

} // namespace

TEST_CASE("Offline renderer sends events on the 64-frame grid and 1ms ticks at their sample positions",
          "[render][offline]")
{
    SmfSong song;
    song.events    = {ev(0, 1), ev(1500, 2), ev(8000, 3, 2), ev(8125, 4)};
    song.endTimeUs = 8125;

    Recorder r;
    // 8000Hz: 1ms = 8フレーム、イベントの格子 = 64フレーム = 8ms。
    // 1500us(12フレーム)は64へ、8000us(64フレーム)はそのまま、8125us(65フレーム)は128へ送る。
    // 8000us のイベントはポート2で、そのまま渡る。
    // 曲の終わり 8125us + 余韻 8000us = 129フレーム
    const uint64_t total = renderSong(song, 8000, 8000, r.target, r.sink);
    CHECK(total == 129);
    CHECK(r.frames == 129);

    std::vector<std::string> expected{"midi1@0"};
    for (int k = 1; k <= 16; ++k) {
        expected.push_back("tick@" + std::to_string(k * 8));
        if (k == 8)  { expected.push_back("midi2@64"); expected.push_back("midi3@64p2"); }
        if (k == 16) expected.push_back("midi4@128");
    }
    CHECK(r.log == expected);
}

TEST_CASE("Offline renderer keeps 1000 ticks per second at a non-integer frames-per-ms rate",
          "[render][offline]")
{
    SmfSong song;
    song.endTimeUs = 1000000;

    Recorder r;
    const uint64_t total = renderSong(song, 44100, 0, r.target, r.sink);
    CHECK(total == 44100);
    CHECK(r.log.size() == 1000);
    CHECK(r.log.back() == "tick@44100");
}

TEST_CASE("Offline renderer stops when the plugin fails to render", "[render][offline]")
{
    SmfSong song;
    song.endTimeUs = 10000;

    Recorder r;
    r.target.render = [](float*, uint32_t) { return false; };
    CHECK_THROWS_AS(renderSong(song, 44100, 0, r.target, r.sink), std::runtime_error);
}
