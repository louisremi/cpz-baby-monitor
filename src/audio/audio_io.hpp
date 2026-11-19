#pragma once

// Microphone capture and speaker playback through miniaudio's PulseAudio
// backend (served by pipewire-pulse on the CardputerZero, same as the
// official Recorder app). 8 kHz mono s16; resampling is done by the server.

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace cpzbm::audio {

class Capture {
public:
    Capture();
    ~Capture();
    bool start(int block_samples);
    void stop();
    // Pops one block of block_samples if available.
    bool read(std::vector<int16_t>* block);
    std::string error() const { return error_; }

    struct Impl;

private:
    Impl* impl_ = nullptr;
    std::string error_;
};

class Playback {
public:
    Playback();
    ~Playback();
    bool start();
    void stop();
    void write(const int16_t* pcm, std::size_t n);
    void setMuted(bool muted);
    bool muted() const;
    // Samples queued but not yet played.
    std::size_t queued() const;
    std::string error() const { return error_; }

    struct Impl;

private:
    Impl* impl_ = nullptr;
    std::string error_;
};

}  // namespace cpzbm::audio
