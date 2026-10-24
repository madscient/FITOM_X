// apps/fitom_render/OfflineRenderer.cpp

#include "OfflineRenderer.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace fitom::render {

namespace {

constexpr uint64_t kMaxChunkFrames = 4096;

class Clock {
public:
    Clock(uint32_t rate, const OfflineTarget& t, const SampleSink& sink)
        : rate_(rate), target_(t), sink_(sink), buf_(kMaxChunkFrames * 2) {}

    void advanceTo(uint64_t frame) {
        while (pos_ < frame) {
            const uint64_t nextTick = (ticks_ + 1) * rate_ / 1000;
            const uint64_t end = std::min({frame, nextTick, pos_ + kMaxChunkFrames});
            const auto frames = static_cast<uint32_t>(end - pos_);
            if (!target_.render(buf_.data(), frames))
                throw std::runtime_error("rendering failed");
            sink_(buf_.data(), frames);
            pos_ = end;
            if (pos_ == nextTick) {
                ++ticks_;
                target_.tick();
            }
        }
    }
    uint64_t pos() const { return pos_; }

private:
    uint64_t             rate_;
    const OfflineTarget& target_;
    const SampleSink&    sink_;
    std::vector<float>   buf_;
    uint64_t             pos_   = 0;
    uint64_t             ticks_ = 0;
};

} // namespace

uint64_t renderSong(const SmfSong& song, uint32_t sampleRate, uint64_t tailUs,
                    const OfflineTarget& target, const SampleSink& sink)
{
    if (sampleRate < 1000) throw std::invalid_argument("sample rate must be at least 1000 Hz");
    const auto toFrame = [sampleRate](uint64_t us) { return us * sampleRate / 1000000; };

    Clock clock(sampleRate, target, sink);
    for (const auto& e : song.events) {
        clock.advanceTo(toFrame(e.timeUs));
        target.sendMidi(e.bytes.data(), e.bytes.size());
    }
    clock.advanceTo(toFrame(song.endTimeUs + tailUs));
    return clock.pos();
}

} // namespace fitom::render
