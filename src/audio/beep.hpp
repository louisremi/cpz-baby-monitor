#pragma once

// Confirmation sound for the system volume keys. The CardputerZero panel shows a
// volume OSD, but our app draws over /dev/fb0, so the change is silent and
// invisible: a short tone at the current sink volume is the only feedback.
//
// It owns a separate short-lived playback stream, because the Baby unit has no
// playback stream at all and the Parent's decoded audio must not be disturbed
// (PipeWire mixes the two). The device starts on the first press and stops once
// the tone has drained, so an idle monitor pays nothing for it.

#include <cstdint>
#include <string>

#include "audio/audio_io.hpp"

namespace cpzbm::audio {

class Beep {
public:
    // direction: +1 volume up, -1 volume down, 0 mute.
    bool play(int direction, uint64_t now_ms);
    // Stops the stream once the tone has played and the hold-off expired.
    void tick(uint64_t now_ms);
    void stop();
    bool active() const { return started_; }

private:
    Playback player_;
    bool started_   = false;
    bool warned_    = false;
    uint64_t stop_at_ms_ = 0;
};

}  // namespace cpzbm::audio
