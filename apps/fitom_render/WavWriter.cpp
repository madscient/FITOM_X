// apps/fitom_render/WavWriter.cpp

#include "WavWriter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace fitom::render {

namespace {

constexpr uint16_t kChannels = 2;

void put16(std::vector<uint8_t>& b, uint16_t v)
{
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t>& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void putTag(std::vector<uint8_t>& b, const char* tag)
{
    b.insert(b.end(), tag, tag + 4);
}

} // namespace

std::optional<WavFormat> parseWavFormat(const std::string& s)
{
    if (s == "s16") return WavFormat::S16;
    if (s == "s24") return WavFormat::S24;
    if (s == "f32") return WavFormat::F32;
    return std::nullopt;
}

WavWriter::WavWriter(const std::filesystem::path& path, uint32_t sampleRate, WavFormat format)
    : f_(path, std::ios::binary | std::ios::trunc)
    , sampleRate_(sampleRate)
    , format_(format)
    , bytesPerSample_(format == WavFormat::S16 ? 2u : format == WavFormat::S24 ? 3u : 4u)
{
    if (!f_) throw std::runtime_error("cannot open " + path.string() + " for writing");
    writeHeader();
}

WavWriter::~WavWriter()
{
    try { close(); } catch (...) {}
}

// 非PCM(IEEE float)は fmt を18バイト(cbSize つき)にし、fact チャンクを置く(WAVEの規定)。
void WavWriter::writeHeader()
{
    const bool     isFloat    = (format_ == WavFormat::F32);
    const uint32_t fmtLen     = isFloat ? 18u : 16u;
    const uint32_t factLen    = isFloat ? 12u : 0u;
    const uint32_t blockAlign = kChannels * bytesPerSample_;
    const uint64_t dataBytes  = frames_ * blockAlign;
    const uint64_t riffSize   = 4 + (8 + fmtLen) + factLen + 8 + dataBytes;

    std::vector<uint8_t> h;
    putTag(h, "RIFF");
    put32(h, static_cast<uint32_t>(riffSize));
    putTag(h, "WAVE");
    putTag(h, "fmt ");
    put32(h, fmtLen);
    put16(h, isFloat ? 3 : 1);
    put16(h, kChannels);
    put32(h, sampleRate_);
    put32(h, sampleRate_ * blockAlign);
    put16(h, static_cast<uint16_t>(blockAlign));
    put16(h, static_cast<uint16_t>(bytesPerSample_ * 8));
    if (isFloat) {
        put16(h, 0);
        putTag(h, "fact");
        put32(h, 4);
        put32(h, static_cast<uint32_t>(frames_));
    }
    putTag(h, "data");
    put32(h, static_cast<uint32_t>(dataBytes));

    f_.seekp(0);
    f_.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
}

void WavWriter::write(const float* interleaved, uint32_t frames)
{
    if (closed_) throw std::runtime_error("WAV: write after close");
    const uint64_t blockAlign = kChannels * bytesPerSample_;
    // ヘッダ(最大58バイト)を含めて RIFF のサイズが 32bit に収まること。
    if ((frames_ + frames) * blockAlign > std::numeric_limits<uint32_t>::max() - 64)
        throw std::runtime_error("WAV: output exceeds the 4 GiB limit of the format");

    std::vector<uint8_t> buf;
    buf.reserve(static_cast<std::size_t>(frames) * blockAlign);
    for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * kChannels; ++i) {
        const float x = interleaved[i];
        switch (format_) {
        case WavFormat::S16: {
            const long v = std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f);
            put16(buf, static_cast<uint16_t>(static_cast<int16_t>(v)));
            break;
        }
        case WavFormat::S24: {
            const long v = std::lround(static_cast<double>(std::clamp(x, -1.0f, 1.0f)) * 8388607.0);
            const uint32_t u = static_cast<uint32_t>(v);
            buf.push_back(static_cast<uint8_t>(u));
            buf.push_back(static_cast<uint8_t>(u >> 8));
            buf.push_back(static_cast<uint8_t>(u >> 16));
            break;
        }
        case WavFormat::F32: {
            uint32_t u;
            std::memcpy(&u, &x, sizeof(u));
            put32(buf, u);
            break;
        }
        }
    }
    f_.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (!f_) throw std::runtime_error("WAV: write failed");
    frames_ += frames;
}

void WavWriter::close()
{
    if (closed_) return;
    closed_ = true;
    writeHeader();
    f_.close();
    if (f_.fail()) throw std::runtime_error("WAV: failed to finalize the file");
}

} // namespace fitom::render
