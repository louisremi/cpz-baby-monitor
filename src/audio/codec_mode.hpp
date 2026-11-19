#pragma once

// Codec2 modes used on the air. Values are libcodec2's CODEC2_MODE_* ids, so
// they can be passed straight to codec2_create() and sent as one byte.

#include <cstdint>
#include <optional>

namespace cpzbm::audio {

enum class Mode : uint8_t {
    M1600 = 2,  // CODEC2_MODE_1600: 40 ms / 64 bits per frame
    M700C = 8,  // CODEC2_MODE_700C: 40 ms / 28 bits (padded to 4 bytes)
};

inline const char* modeName(Mode m) { return m == Mode::M700C ? "700C" : "1600"; }

// The other mode: 1600 (better quality) <-> 700C (range fallback: half the
// payload, so shorter airtime per packet and a less busy channel).
inline Mode otherMode(Mode m) { return m == Mode::M700C ? Mode::M1600 : Mode::M700C; }

inline std::optional<Mode> modeFromByte(uint8_t b)
{
    if (b == static_cast<uint8_t>(Mode::M1600)) return Mode::M1600;
    if (b == static_cast<uint8_t>(Mode::M700C)) return Mode::M700C;
    return std::nullopt;
}

}  // namespace cpzbm::audio
