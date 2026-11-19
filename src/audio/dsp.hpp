#pragma once

// Pure audio helpers (no I/O) so they can be unit-tested on the host.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace cpzbm::audio {

// RMS level of a block in dBFS, clamped to [-100, 0].
inline float rmsDbfs(const int16_t* pcm, std::size_t n)
{
    if (n == 0) return -100.0f;
    double acc = 0;
    for (std::size_t i = 0; i < n; ++i) acc += static_cast<double>(pcm[i]) * pcm[i];
    const double rms = std::sqrt(acc / static_cast<double>(n)) / 32768.0;
    if (rms <= 1e-5) return -100.0f;
    const double db = 20.0 * std::log10(rms);
    return static_cast<float>(db > 0 ? 0 : db);
}

// Peak absolute amplitude 0..1 (for waveform columns).
inline float peak(const int16_t* pcm, std::size_t n)
{
    int m = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const int a = pcm[i] < 0 ? -static_cast<int>(pcm[i]) : pcm[i];
        if (a > m) m = a;
    }
    return static_cast<float>(m) / 32768.0f;
}

// Slow automatic gain control for the (quiet) built-in mic: Codec2 needs a
// healthy input level to model speech/crying well. Gain rises slowly toward
// a peak target and drops quickly on loud input so it never clips for long.
class Agc {
public:
    Agc(float target_peak = 0.5f, float max_gain = 100.0f) : target_(target_peak), max_gain_(max_gain) {}
    void process(int16_t* pcm, std::size_t n)
    {
        const float p = peak(pcm, n) * gain_;
        if (p > target_) {
            gain_ = std::max(1.0f, gain_ * std::max(0.5f, target_ / p));  // fast attack
        } else if (p > 0.0005f * gain_) {
            gain_ = std::min(max_gain_, gain_ * 1.02f);  // ~+4 dB/s at 25 blocks/s
        }
        for (std::size_t i = 0; i < n; ++i) {
            const float v = pcm[i] * gain_;
            pcm[i]        = static_cast<int16_t>(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v);
        }
    }
    float gainDb() const { return 20.0f * std::log10(gain_); }

private:
    float target_, max_gain_;
    float gain_ = 8.0f;
};

// Voice-operated gate: opens when a block exceeds the threshold, stays open
// for `hangover_blocks` after the last loud block.
class VoxGate {
public:
    VoxGate(float threshold_dbfs, int hangover_blocks) : threshold_(threshold_dbfs), hangover_(hangover_blocks) {}
    // Returns true while the gate is open (including this block).
    bool update(float level_dbfs)
    {
        if (level_dbfs >= threshold_) {
            remaining_ = hangover_;
            return true;
        }
        if (remaining_ > 0) {
            --remaining_;
            return true;
        }
        return false;
    }
    void setThreshold(float t) { threshold_ = t; }
    float threshold() const { return threshold_; }

private:
    float threshold_;
    int hangover_;
    int remaining_ = 0;
};

// Groups encoded codec frames into packets and decides what to send, given
// the VOX gate state per frame:
//  - while the gate is closed, the last complete packet is kept as pre-roll;
//  - on the frame where the gate opens, the pre-roll is released first, so
//    the start of the sound (just before it crossed the threshold) is heard;
//  - while the gate is open, every complete packet is released.
// Pure: no I/O, so the gating behaviour is unit-tested.
class Packetizer {
public:
    Packetizer(int frames_per_packet, int bytes_per_frame)
        : frames_per_packet_(frames_per_packet), bytes_per_frame_(bytes_per_frame)
    {
    }

    // Adds one encoded frame. Appends 0..2 packets to `out` (pre-roll first).
    void push(const uint8_t* frame, bool gate_open, std::vector<std::vector<uint8_t>>* out)
    {
        if (gate_open && !was_open_ && !preroll_.empty()) {
            out->push_back(std::move(preroll_));
            preroll_.clear();
        }
        was_open_ = gate_open;
        current_.insert(current_.end(), frame, frame + bytes_per_frame_);
        if (++frames_ < frames_per_packet_) return;
        if (gate_open)
            out->push_back(std::move(current_));
        else
            preroll_ = std::move(current_);
        current_.clear();
        frames_ = 0;
    }

private:
    int frames_per_packet_;
    int bytes_per_frame_;
    int frames_    = 0;
    bool was_open_ = false;
    std::vector<uint8_t> current_;
    std::vector<uint8_t> preroll_;
};

// Reorders decoded audio packets by 16-bit sequence number and releases them
// once `depth` packets are buffered (or the oldest has waited too long).
// Missing packets are reported as gaps so the player can insert silence.
class JitterBuffer {
public:
    explicit JitterBuffer(std::size_t depth) : depth_(depth) {}

    // Returns false for duplicates / packets older than the play head.
    // `stream` identifies one run of the sender's sequence numbers: a new
    // value (sender restarted, seq back to 0) resynchronises the buffer.
    // A jump of more than kMaxGap packets also does, as a safety net.
    static constexpr int kMaxGap = 64;
    bool push(uint8_t stream, uint16_t seq, std::vector<int16_t> pcm)
    {
        if (started_ && stream != stream_) reset();
        if (started_) {
            const int16_t diff = static_cast<int16_t>(seq - next_);
            if (diff < 0) return false;
            if (diff > kMaxGap) reset();
        }
        if (!started_) {
            next_    = seq;
            stream_  = stream;
            started_ = true;
        }
        const auto key = static_cast<uint16_t>(seq - next_);
        if (pending_.count(key)) return false;
        pending_.emplace(key, std::move(pcm));
        return true;
    }

    // Pops the next block if available. `gap` is set when a packet was lost.
    // `force` releases data even if fewer than `depth` packets are queued
    // (used when the stream stalls, e.g. at the end of a burst).
    bool pop(std::vector<int16_t>* out, bool* gap, bool force = false)
    {
        *gap = false;
        if (pending_.empty()) return false;
        if (!force && pending_.size() < depth_) return false;
        auto it = pending_.begin();
        if (it->first != 0) {
            *gap = true;
            advance(1);
            return true;
        }
        *out = std::move(it->second);
        pending_.erase(it);
        advance(1);
        return true;
    }

    std::size_t size() const { return pending_.size(); }
    void reset()
    {
        pending_.clear();
        started_ = false;
    }

private:
    void advance(uint16_t n)
    {
        next_ = static_cast<uint16_t>(next_ + n);
        std::map<uint16_t, std::vector<int16_t>> shifted;
        for (auto& kv : pending_) shifted.emplace(static_cast<uint16_t>(kv.first - n), std::move(kv.second));
        pending_.swap(shifted);
    }

    std::size_t depth_;
    bool started_   = false;
    uint8_t stream_ = 0;
    uint16_t next_  = 0;
    std::map<uint16_t, std::vector<int16_t>> pending_;
};

}  // namespace cpzbm::audio
