#pragma once
// apps/fitom_render/WavWriter.h
// ステレオのWAVファイルを書く。データ長は close() でヘッダへ書き戻す。

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace fitom::render {

enum class WavFormat { S16, S24, F32 };

// "s16" / "s24" / "f32"。それ以外は nullopt。
std::optional<WavFormat> parseWavFormat(const std::string& s);

class WavWriter {
public:
    // 開けなければ std::runtime_error を投げる。
    WavWriter(const std::filesystem::path& path, uint32_t sampleRate, WavFormat format);
    ~WavWriter();

    // L/R インターリーブの float を frames フレーム分書く。
    // S16/S24 は [-1, 1] にクリップして量子化する。F32 はクリップしない。
    // RIFF の上限(4GiB)を超える場合は std::runtime_error を投げる。
    void write(const float* interleaved, uint32_t frames);

    // ヘッダのデータ長を確定してファイルを閉じる。失敗すれば std::runtime_error を投げる。
    void close();

    uint64_t framesWritten() const { return frames_; }

private:
    void writeHeader();

    std::ofstream f_;
    uint32_t      sampleRate_;
    WavFormat     format_;
    uint32_t      bytesPerSample_;
    uint64_t      frames_ = 0;
    bool          closed_ = false;
};

} // namespace fitom::render
