#pragma once

// Short confirmation tone for the system volume keys. Rendering is pure so the
// shape is unit-testable; the player (audio/beep.*) owns the miniaudio device.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cpzbm::audio {

// Renders a ramped sine wave into `out`: `ms` milliseconds at `hz`, peak
// `amp * 32767`, with `ramp_ms` linear fade-in/out so it starts and ends at
// zero and cannot click. `out` is overwritten.
inline void renderTone(std::vector<int16_t>& out, int hz, int ms, int sample_rate, float amp, int ramp_ms = 3)
{
    const int n      = std::max(0, sample_rate * ms / 1000);
    const int ramp   = std::max(1, sample_rate * ramp_ms / 1000);
    const float peak = std::clamp(amp, 0.0f, 1.0f) * 32767.0f;
    out.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        float env = 1.0f;
        if (i < ramp) env = static_cast<float>(i) / static_cast<float>(ramp);
        else if (i >= n - ramp) env = static_cast<float>(n - 1 - i) / static_cast<float>(ramp);
        env = std::clamp(env, 0.0f, 1.0f);
        const float s = peak * env * std::sin(2.0f * 3.14159265358979f * static_cast<float>(hz) * static_cast<float>(i) /
                                              static_cast<float>(sample_rate));
        out[static_cast<std::size_t>(i)] = static_cast<int16_t>(std::lround(std::clamp(s, -32767.0f, 32767.0f)));
    }
}

}  // namespace cpzbm::audio
