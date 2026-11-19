#pragma once

// When the panel should go dark. Pure: no clock, no IO, so the host tests can
// drive it with synthetic timestamps. The UI owns the hardware side (writing
// the backlight and skipping rendering) and calls back into this whenever a
// key, a link event or an external brightness change is seen.
//
// Media keys (volume, mute, brightness) deliberately do not count as activity:
// adjusting the volume at night must not light the screen up.

#include <cstdint>

namespace cpzbm {

enum class ScreenPowerState { On, Off };

class ScreenPowerPolicy {
public:
    // timeout_ms == 0 keeps the screen on forever.
    explicit ScreenPowerPolicy(uint64_t timeout_ms) : timeout_ms_(timeout_ms) {}

    uint64_t timeoutMs() const { return timeout_ms_; }
    ScreenPowerState state() const { return state_; }
    uint64_t lastActivityMs() const { return last_activity_ms_; }

    // Back to On, and the idle clock restarts.
    void wake(uint64_t now_ms)
    {
        state_             = ScreenPowerState::On;
        last_activity_ms_  = now_ms;
    }

    // Someone else (the launcher's own DarkTime) darkened the panel: adopt it
    // without writing to the backlight.
    void adoptOff()
    {
        state_ = ScreenPowerState::Off;
    }

    // A user interaction that keeps the screen alive (only meaningful while On).
    void noteActivity(uint64_t now_ms) { last_activity_ms_ = now_ms; }

    // True while On and the timeout has elapsed.
    bool expired(uint64_t now_ms) const
    {
        return state_ == ScreenPowerState::On && timeout_ms_ > 0 && now_ms - last_activity_ms_ >= timeout_ms_;
    }

    uint64_t idleMs(uint64_t now_ms) const { return now_ms - last_activity_ms_; }

private:
    uint64_t timeout_ms_      = 0;
    uint64_t last_activity_ms_ = 0;
    ScreenPowerState state_    = ScreenPowerState::On;
};

}  // namespace cpzbm
