#pragma once
// apps/fitom_render/SmfReader.h
// Standard MIDI File (format 0/1) を読み、全トラックを時刻順に並べた
// MIDIバイト列の列にする。

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fitom::render {

struct SmfEvent {
    uint64_t             timeUs = 0;   // 曲の先頭からの時刻(マイクロ秒)
    std::vector<uint8_t> bytes;        // MidiProcessor::receiveByte() へそのまま渡せる列
};

struct SmfSong {
    std::vector<SmfEvent> events;      // timeUs の昇順
    uint64_t              endTimeUs = 0;   // 最後のイベント(End of Track を含む)の時刻
};

// 読めない・壊れているファイルは std::runtime_error を投げる。
// 同じ時刻のイベントは、トラック番号順、トラック内の出現順に並ぶ。
// チャンネルメッセージとSysExだけを出力し、メタイベントは出力しない
// (テンポは時刻の計算に使う)。ポート指定のメタイベントは読まない。
SmfSong readSmf(const std::filesystem::path& path);
SmfSong parseSmf(const std::vector<uint8_t>& data);

} // namespace fitom::render
