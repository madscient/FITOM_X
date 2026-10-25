#pragma once
// apps/fitom_render/SmfReader.h
// Standard MIDI File (format 0/1) を読み、全トラックを時刻順に並べた
// MIDIバイト列の列にする。

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fitom::render {

struct SmfEvent {
    uint64_t             timeUs = 0;   // 曲の先頭からの時刻(マイクロ秒)
    std::vector<uint8_t> bytes;        // MidiProcessor::receiveByte() へそのまま渡せる列
    uint8_t              port = 0;     // そのトラックで直前の MIDI Port(FF 21)の値。無ければ 0
};

struct SmfSong {
    std::vector<SmfEvent> events;      // timeUs の昇順
    uint64_t              endTimeUs = 0;   // 最後のイベント(End of Track を含む)の時刻
};

// 読めない・壊れているファイルは std::runtime_error を投げる。
// 同じ時刻のイベントは、トラック番号順、トラック内の出現順に並ぶ。
// チャンネルメッセージとSysExだけを出力し、メタイベントは出力しない
// (テンポは時刻の計算に、MIDI Port はイベントの port に使う)。
// Device Name(FF 09)は読まない。
SmfSong readSmf(const std::filesystem::path& path);
SmfSong parseSmf(const std::vector<uint8_t>& data);

// port が portCount 以上のイベントを取り除き、取り除いた数を port ごとに返す。
std::map<uint8_t, std::size_t> removeEventsOutsidePorts(SmfSong& song, uint8_t portCount);

} // namespace fitom::render
