#pragma once

// Link layer: discovery, PAKE pairing, encrypted messaging with replay
// protection, and role synchronisation. Hardware-free: time is passed in,
// packets go out through a callback, so two Links can be wired together in
// tests over a lossy fake channel.
//
// Pairing roles:
//   "A" (shows the code)  : beacons every second, answers one handshake at a
//                           time; a fresh code after 3 unsuccessful attempts.
//   "B" (enters the code) : scans beacons, user picks one and types the code.
// Messages: B->A PairReq, A->B PairResp(+tagA), B->A PairConfirm(tagB),
// A->B encrypted Keepalive as the final ack (B retransmits Confirm until then).

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "proto/crypto.hpp"
#include "proto/wire.hpp"

namespace cpzbm {

enum class Role : uint8_t { None = 0, Baby = 1, Parent = 2 };
inline Role oppositeRole(Role r) { return r == Role::Baby ? Role::Parent : r == Role::Parent ? Role::Baby : Role::None; }

struct Identity {
    uint32_t dev_id = 0;
    wire::Name name{};
};

struct Pairing {
    crypto::Key key{};
    uint16_t pair_id    = 0;
    uint32_t peer_id    = 0;
    wire::Name peer_name{};
    Role role           = Role::None;
    uint32_t tx_counter = 0;  // next counter to use
    uint32_t rx_counter = 0;  // highest counter accepted from the peer
};

struct DiscoveredPeer {
    uint32_t dev_id = 0;
    wire::Name name{};
    wire::Nonce session{};
    float rssi       = 0;
    uint64_t seen_ms = 0;
};

enum class PairPhase {
    Idle,
    Showing,      // A: displaying code, beaconing
    AwaitConfirm, // A: sent PairResp, waiting for PairConfirm
    Scanning,     // B: listening for beacons
    Requesting,   // B: sent PairReq, waiting for PairResp
    Confirming,   // B: sent PairConfirm, waiting for the encrypted ack
    Paired,
    Failed,
};

enum class PairError { None, WrongCode, Timeout, CodeExpired, Cancelled };

struct LinkEvent {
    enum class Kind { Paired, PairFailed, NewCode, RoleChanged, Message, Unpaired };
    LinkEvent() = default;
    explicit LinkEvent(Kind k, PairError e = PairError::None) : kind(k), error(e) {}
    Kind kind       = Kind::Message;
    PairError error = PairError::None;
    wire::Type type = wire::Type::Keepalive;  // for Message
    wire::Bytes body;                         // for Message
    float rssi = 0, snr = 0;
};

class Link {
public:
    using SendFn = std::function<void(wire::Bytes)>;
    using PersistFn = std::function<void(const Pairing&)>;

    // The tx counter is persisted every kCounterStride packets and jumps
    // forward by the stride on load, so a crash never reuses a nonce. The rx
    // counter is persisted every kRxSaveStride packets (~10 s of audio) and on
    // flush(): after a crash at most kRxSaveStride-1 old packets could be
    // replayed; after a clean exit, none.
    static constexpr uint32_t kCounterStride = 256;
    static constexpr uint32_t kRxSaveStride  = 32;

    Link(Identity self, SendFn send, PersistFn persist);

    // Restore a stored pairing (counters already advanced by the caller via
    // resumeCounters()).
    void restore(const Pairing& p);
    static Pairing resumeCounters(Pairing p);
    // Persist counters now (call on shutdown).
    void flush();

    void tick(uint64_t now_ms);
    void onPacket(const wire::Bytes& pkt, float rssi, float snr, uint64_t now_ms);

    // --- Pairing ---
    std::string startShowing(uint64_t now_ms);  // returns the 6-digit code
    void startScanning(uint64_t now_ms);
    bool submitCode(uint32_t peer_id, const std::string& code, uint64_t now_ms);
    void cancelPairing();
    PairPhase phase() const { return phase_; }
    PairError lastError() const { return last_error_; }
    const std::string& code() const { return code_; }
    std::vector<DiscoveredPeer> peers(uint64_t now_ms) const;

    // --- Paired operation ---
    bool paired() const { return paired_; }
    const Pairing& pairing() const { return pairing_; }
    void setRole(Role role, uint64_t now_ms);  // tells the peer to take the opposite role
    bool roleSynced() const { return !role_pending_; }
    bool sendSecure(wire::Type type, const wire::Bytes& body);
    void unpair();
    uint64_t lastRxMs() const { return last_rx_ms_; }
    float lastRssi() const { return last_rssi_; }
    float lastSnr() const { return last_snr_; }

    bool pollEvent(LinkEvent* out);

private:
    void beacon(uint64_t now_ms);
    void handlePairing(const wire::Bytes& pkt, wire::Type type, float rssi, uint64_t now_ms);
    void handleSecure(const wire::Bytes& pkt, float rssi, float snr, uint64_t now_ms);
    void becomePaired(const crypto::PakeSession& pake, uint32_t peer_id, const wire::Name& peer_name, uint64_t now_ms);
    // Showing side: the current handshake attempt failed (wrong code,
    // timeout or NACK). Counts as one online guess of the code.
    void abandonHandshake(uint64_t now_ms);
    void forgetPairing();
    void fail(PairError e);
    void newCode(uint64_t now_ms);
    uint8_t myDirection() const;
    void persist();
    void emit(LinkEvent e) { events_.push_back(std::move(e)); }

    Identity self_;
    SendFn send_;
    PersistFn persist_;
    std::deque<LinkEvent> events_;

    PairPhase phase_     = PairPhase::Idle;
    PairError last_error_ = PairError::None;
    uint64_t phase_deadline_ = 0;
    uint64_t next_tx_ms_     = 0;
    int retries_             = 0;

    // A side
    std::string code_;
    wire::Nonce session_{};
    int failed_attempts_ = 0;
    crypto::PakeSession pake_;
    wire::Nonce peer_nonce_{};
    uint32_t pending_peer_ = 0;
    wire::Bytes cached_resp_;

    // B side
    std::vector<DiscoveredPeer> discovered_;
    wire::Bytes cached_req_;
    wire::Bytes cached_confirm_;
    wire::Name pending_name_{};

    // Paired
    bool paired_ = false;
    Pairing pairing_;
    uint32_t persisted_tx_ = 0;
    uint32_t persisted_rx_ = 0;
    bool role_pending_     = false;
    uint64_t role_next_ms_ = 0;
    int role_retries_      = 0;
    uint64_t last_rx_ms_   = 0;
    float last_rssi_ = 0, last_snr_ = 0;
};

const char* pairErrorText(PairError e);

}  // namespace cpzbm
