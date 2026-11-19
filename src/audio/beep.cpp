#include "audio/beep.hpp"

#include <spdlog/spdlog.h>

#include <vector>

#include "audio/codec.hpp"
#include "audio/tone.hpp"

namespace cpzbm::audio {
namespace {

constexpr int kLeadInMs  = 60;   // the PipeWire sink is suspended when idle
constexpr int kToneMs    = 25;
constexpr float kAmp     = 0.35f;
constexpr uint64_t kHoldMs = 1200;  // keep the stream while the user keeps pressing

int toneHz(int direction) { return direction > 0 ? 990 : direction < 0 ? 660 : 440; }

}  // namespace

bool Beep::play(int direction, uint64_t now_ms)
{
    const bool starting = !player_.active();
    if (!player_.start(/*quiet=*/true)) {
        if (!warned_) {
            spdlog::warn("Audio: volume feedback unavailable: {}", player_.error());
            warned_ = true;
        }
        return false;
    }

    std::vector<int16_t> pcm;
    if (starting) pcm.assign(static_cast<std::size_t>(kSampleRate) * kLeadInMs / 1000, 0);
    std::vector<int16_t> tone;
    renderTone(tone, toneHz(direction), kToneMs, kSampleRate, kAmp);
    pcm.insert(pcm.end(), tone.begin(), tone.end());

    player_.clear();  // do not let a burst of presses queue up behind stale tones
    player_.write(pcm.data(), pcm.size());
    started_    = true;
    stop_at_ms_ = now_ms + kHoldMs;
    return true;
}

void Beep::tick(uint64_t now_ms)
{
    if (!started_ || now_ms < stop_at_ms_) return;
    if (player_.queued() > 0) {
        stop_at_ms_ = now_ms + 100;  // still draining
        return;
    }
    player_.stop();
    started_ = false;
}

void Beep::stop()
{
    player_.stop();
    started_ = false;
}

}  // namespace cpzbm::audio
