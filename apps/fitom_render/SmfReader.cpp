// apps/fitom_render/SmfReader.cpp

#include "SmfReader.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace fitom::render {

namespace {

enum class Kind { Midi, Tempo, EndOfTrack };

struct RawEvent {
    uint64_t             tick = 0;
    Kind                 kind = Kind::Midi;
    uint32_t             tempo = 0;    // Kind::Tempo のときだけ有効(四分音符あたりのマイクロ秒)
    std::vector<uint8_t> bytes;
};

class Reader {
public:
    Reader(const std::vector<uint8_t>& d, std::size_t begin, std::size_t end)
        : d_(d), pos_(begin), end_(end) {}

    bool        atEnd() const { return pos_ >= end_; }
    std::size_t pos()   const { return pos_; }

    uint8_t u8() {
        need(1);
        return d_[pos_++];
    }
    uint16_t u16() {
        need(2);
        uint16_t v = static_cast<uint16_t>((d_[pos_] << 8) | d_[pos_ + 1]);
        pos_ += 2;
        return v;
    }
    uint32_t u32() {
        need(4);
        uint32_t v = (static_cast<uint32_t>(d_[pos_]) << 24) | (static_cast<uint32_t>(d_[pos_ + 1]) << 16)
                   | (static_cast<uint32_t>(d_[pos_ + 2]) << 8) | d_[pos_ + 3];
        pos_ += 4;
        return v;
    }
    // 可変長数値。SMFの規格上の上限は4バイト(0x0FFFFFFF)。
    uint32_t vlq() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t b = u8();
            v = (v << 7) | (b & 0x7F);
            if (!(b & 0x80)) return v;
        }
        throw std::runtime_error("SMF: variable-length quantity longer than 4 bytes");
    }
    std::vector<uint8_t> bytes(std::size_t n) {
        need(n);
        std::vector<uint8_t> v(d_.begin() + static_cast<std::ptrdiff_t>(pos_),
                               d_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return v;
    }
    void skip(std::size_t n) { need(n); pos_ += n; }

private:
    void need(std::size_t n) const {
        if (n > end_ - pos_) throw std::runtime_error("SMF: unexpected end of data");
    }
    const std::vector<uint8_t>& d_;
    std::size_t pos_;
    std::size_t end_;
};

std::vector<RawEvent> parseTrack(Reader& r)
{
    std::vector<RawEvent> out;
    uint64_t tick = 0;
    uint8_t running = 0;

    while (!r.atEnd()) {
        tick += r.vlq();
        uint8_t b = r.u8();

        if (b == 0xFF) {
            uint8_t type = r.u8();
            uint32_t len = r.vlq();
            std::vector<uint8_t> data = r.bytes(len);
            running = 0;
            if (type == 0x51 && len == 3) {
                RawEvent e;
                e.tick  = tick;
                e.kind  = Kind::Tempo;
                e.tempo = (static_cast<uint32_t>(data[0]) << 16) | (data[1] << 8) | data[2];
                out.push_back(std::move(e));
            } else if (type == 0x2F) {
                RawEvent e;
                e.tick = tick;
                e.kind = Kind::EndOfTrack;
                out.push_back(std::move(e));
                break;
            }
            continue;
        }
        if (b == 0xF0 || b == 0xF7) {
            // F0 は先頭の F0 を補って渡す。F7 はエスケープ(分割されたSysExの続きを含む)で、
            // 中身をそのまま渡す。
            uint32_t len = r.vlq();
            RawEvent e;
            e.tick = tick;
            if (b == 0xF0) e.bytes.push_back(0xF0);
            std::vector<uint8_t> data = r.bytes(len);
            e.bytes.insert(e.bytes.end(), data.begin(), data.end());
            running = 0;
            out.push_back(std::move(e));
            continue;
        }

        uint8_t status;
        uint8_t first;
        if (b & 0x80) {
            if (b >= 0xF0)
                throw std::runtime_error("SMF: invalid status byte in track");
            status  = b;
            running = b;
            first   = r.u8();
        } else {
            if (running == 0)
                throw std::runtime_error("SMF: running status without a preceding status byte");
            status = running;
            first  = b;
        }
        RawEvent e;
        e.tick = tick;
        e.bytes.push_back(status);
        e.bytes.push_back(first);
        const uint8_t type = status & 0xF0;
        if (type != 0xC0 && type != 0xD0) e.bytes.push_back(r.u8());
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace

SmfSong parseSmf(const std::vector<uint8_t>& data)
{
    Reader head(data, 0, data.size());
    if (data.size() < 14 || head.u32() != 0x4D546864)   // "MThd"
        throw std::runtime_error("SMF: missing MThd header");
    const uint32_t headerLen = head.u32();
    if (headerLen < 6) throw std::runtime_error("SMF: MThd header too short");
    const uint16_t format   = head.u16();
    head.u16();   // トラック数。実際に在る MTrk を全部読むので使わない
    const uint16_t division = head.u16();
    head.skip(headerLen - 6);
    if (format > 1) throw std::runtime_error("SMF: format 2 is not supported");
    if (division == 0) throw std::runtime_error("SMF: division is 0");

    // 全トラックを1列に並べ、tick で安定ソートする(同 tick ではトラック順・出現順が残る)。
    std::vector<RawEvent> all;
    std::size_t pos = head.pos();
    while (data.size() - pos >= 8) {
        Reader chunk(data, pos, data.size());
        const uint32_t id  = chunk.u32();
        const uint32_t len = chunk.u32();
        const std::size_t body = chunk.pos();
        if (len > data.size() - body) throw std::runtime_error("SMF: chunk exceeds file size");
        if (id == 0x4D54726B) {   // "MTrk"
            Reader track(data, body, body + len);
            std::vector<RawEvent> ev = parseTrack(track);
            std::move(ev.begin(), ev.end(), std::back_inserter(all));
        }
        pos = body + len;
    }
    std::stable_sort(all.begin(), all.end(),
                     [](const RawEvent& a, const RawEvent& b) { return a.tick < b.tick; });

    SmfSong song;
    if (division & 0x8000) {
        // SMPTE: 上位バイトは -fps(2の補数)、下位バイトは1フレームあたりの tick。
        // 29 は 30 フレームのドロップフレーム(29.97fps)。テンポは使わない。
        const int fps = -static_cast<int8_t>(division >> 8);
        const int tpf = division & 0xFF;
        if (fps <= 0 || tpf == 0) throw std::runtime_error("SMF: invalid SMPTE division");
        const double ticksPerSec = (fps == 29 ? 30000.0 / 1001.0 : fps) * tpf;
        for (auto& e : all) {
            const uint64_t t = static_cast<uint64_t>(std::llround(e.tick * 1e6 / ticksPerSec));
            song.endTimeUs = std::max(song.endTimeUs, t);
            if (e.kind == Kind::Midi) song.events.push_back({t, std::move(e.bytes)});
        }
        return song;
    }

    // 時刻を「マイクロ秒 × 分解能」の整数で積算し、切り捨て誤差を溜めない。
    const uint64_t ppqn = division;
    uint64_t tempo     = 500000;   // 規定値: 120 BPM
    uint64_t lastTick  = 0;
    uint64_t accum     = 0;
    for (auto& e : all) {
        accum   += (e.tick - lastTick) * tempo;
        lastTick = e.tick;
        const uint64_t t = accum / ppqn;
        song.endTimeUs = std::max(song.endTimeUs, t);
        switch (e.kind) {
        case Kind::Tempo:      if (e.tempo != 0) tempo = e.tempo; break;
        case Kind::EndOfTrack: break;
        case Kind::Midi:       song.events.push_back({t, std::move(e.bytes)}); break;
        }
    }
    return song;
}

SmfSong readSmf(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path.string());
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseSmf(data);
}

} // namespace fitom::render
