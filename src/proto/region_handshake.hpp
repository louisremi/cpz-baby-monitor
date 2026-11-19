#pragma once

// Paired band switch, as a pure state machine (no radio, no clock) so it can
// be unit-tested. Both devices must end up on the same band:
//
//   requester: sends Region(r) on the current band every kRetryMs until it
//              hears RegionAck(r), then retunes. After kMaxRetries without an
//              ack it retunes anyway: the peer may already have switched (our
//              ack was the lost packet), and a stuck requester is worse.
//   responder: acks every Region(r) it hears, and retunes only after
//              kResponderHoldMs so that it is still on the old band to ack the
//              requester's retries if the first ack was lost.

#include <cstdint>
#include <optional>

#include "radio/radio_config.hpp"

namespace cpzbm {

class RegionHandshake {
public:
    static constexpr uint64_t kRetryMs         = 700;
    static constexpr int kMaxRetries           = 8;
    // Two retry periods plus margin: the responder hears at least one retry
    // after its first ack before leaving the band.
    static constexpr uint64_t kResponderHoldMs = 2 * kRetryMs + 100;

    // What the controller must do this tick.
    struct Action {
        std::optional<Region> send_request;  // send Region(r) on the current band
        std::optional<Region> send_ack;      // send RegionAck(r)
        std::optional<Region> retune;        // switch the radio to r now
    };

    explicit RegionHandshake(Region current) : current_(current) {}

    Region current() const { return current_; }
    bool busy() const { return requesting_ || responding_; }

    // User asked for band r. Returns false if nothing to do / already busy.
    // When unpaired the caller retunes directly instead.
    bool request(Region r, uint64_t now_ms)
    {
        if (r == current_ || busy()) return false;
        requesting_ = true;
        target_     = r;
        retries_    = 0;
        next_ms_    = now_ms;
        return true;
    }

    Action onRegion(Region r, uint64_t now_ms)
    {
        Action a;
        a.send_ack = r;
        if (r != current_ && !responding_ && !requesting_) {
            responding_ = true;
            target_     = r;
            next_ms_    = now_ms + kResponderHoldMs;
        }
        return a;
    }

    Action onAck(Region r)
    {
        Action a;
        if (requesting_ && r == target_) {
            requesting_ = false;
            a.retune    = finish();
        }
        return a;
    }

    // tx_busy: the radio still has queued/in-flight packets (an ack).
    Action tick(uint64_t now_ms, bool tx_busy)
    {
        Action a;
        if (requesting_ && now_ms >= next_ms_) {
            if (retries_++ >= kMaxRetries) {
                requesting_ = false;
                a.retune    = finish();
            } else {
                a.send_request = target_;
                next_ms_       = now_ms + kRetryMs;
            }
        }
        if (responding_ && now_ms >= next_ms_ && !tx_busy) {
            responding_ = false;
            a.retune    = finish();
        }
        return a;
    }

    // True when the last switch happened without the peer's ack.
    bool lastSwitchUnacked() const { return unacked_; }

private:
    Region finish()
    {
        unacked_ = retries_ > kMaxRetries;
        current_ = target_;
        return target_;
    }

    Region current_;
    Region target_   = Region::US915;
    bool requesting_ = false;
    bool responding_ = false;
    bool unacked_    = false;
    int retries_     = 0;
    uint64_t next_ms_ = 0;
};

}  // namespace cpzbm
