#pragma once

// Codec2 wrapper (libcodec2 1.2 from Debian, dynamically linked - LGPL-2.1).
// Audio is 8 kHz mono s16. A "packet" bundles several codec frames so the
// fixed LoRa per-packet overhead (preamble + header) is amortized.

#include <cstdint>
#include <vector>

#include "audio/codec_mode.hpp"

struct CODEC2;

namespace cpzbm::audio {

inline constexpr int kSampleRate = 8000;

class Codec {
public:
    explicit Codec(Mode mode);
    ~Codec();
    Codec(const Codec&)            = delete;
    Codec& operator=(const Codec&) = delete;

    bool ok() const { return c2_ != nullptr; }
    Mode mode() const { return mode_; }
    int samplesPerFrame() const { return samples_; }
    int bytesPerFrame() const { return bytes_; }

    // pcm.size() must be samplesPerFrame(); appends bytesPerFrame() to out.
    void encode(const int16_t* pcm, std::vector<uint8_t>* out);
    // bits must hold bytesPerFrame(); writes samplesPerFrame() samples.
    void decode(const uint8_t* bits, int16_t* pcm);

private:
    Mode mode_;
    CODEC2* c2_ = nullptr;
    int samples_ = 0;
    int bytes_   = 0;
};

}  // namespace cpzbm::audio
