#pragma once

// Pairing and link crypto, built only from libsodium 1.0.18 primitives (the
// version shipped on the CardputerZero).
//
// Pairing is CPace-style PAKE over ristretto255: both devices derive a
// generator from the 6-digit code and both session nonces, exchange y*G, and
// derive a shared secret. A wrong code yields different secrets, which the
// key-confirmation tags reveal; a passive eavesdropper learns nothing that
// allows an offline brute force of the 10^6 codes, so the code only has to
// resist online guessing (limited to 3 attempts per displayed code).
//
// Link packets are ChaCha20-Poly1305-IETF with the 7-byte header as
// associated data and a nonce built from (direction, counter), so both
// devices can share one key without nonce reuse.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "proto/wire.hpp"

namespace cpzbm::crypto {

using Key = std::array<uint8_t, 32>;

bool init();  // sodium_init(); safe to call more than once

// 6 ASCII digits, uniformly random.
std::string randomCode();
uint32_t randomU32();
wire::Nonce randomNonce();

// One side of a pairing handshake. "A" displays the code and answers;
// "B" types the code and initiates.
class PakeSession {
public:
    // Prepares the ephemeral share for this side. Returns false only on a
    // malformed code (not 6 digits).
    bool start(const std::string& code, uint32_t id_a, uint32_t id_b, const wire::Nonce& nonce_a,
               const wire::Nonce& nonce_b, bool is_a);
    const wire::Point& share() const { return y_pub_; }

    // Combine with the peer share. False if the peer share is invalid
    // (non-canonical or identity) - an active attacker, not a wrong code.
    bool finish(const wire::Point& peer_share);

    wire::Tag tagA() const;
    wire::Tag tagB() const;
    bool verifyPeerTag(const wire::Tag& tag) const;
    Key linkKey() const;
    uint16_t pairId() const;

private:
    std::array<uint8_t, 64> transcriptHash() const;
    wire::Tag tag(char who) const;

    bool is_a_ = false;
    std::array<uint8_t, 32> y_{};  // secret scalar
    wire::Point y_pub_{};
    wire::Point peer_{};
    std::array<uint8_t, 64> isk_{};
    uint32_t id_a_ = 0, id_b_ = 0;
    wire::Nonce nonce_a_{}, nonce_b_{};
    bool finished_ = false;
};

uint16_t pairIdFromKey(const Key& key);

// Seal a link message. direction is 0/1 and must differ between the two
// devices (the device with the lower id uses 0).
wire::Bytes seal(const Key& key, uint8_t direction, const wire::SecHeader& header, const wire::Bytes& plain);
// Open a sealed packet; returns the header and plaintext if authentic.
// The caller is responsible for the counter replay check.
struct Opened {
    wire::SecHeader header;
    wire::Bytes plain;
};
std::optional<Opened> open(const Key& key, uint8_t sender_direction, const wire::Bytes& packet);

}  // namespace cpzbm::crypto
