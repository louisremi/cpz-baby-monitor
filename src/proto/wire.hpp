#pragma once

// On-air packet formats. Every packet starts with one byte: protocol version
// in the high nibble, packet type in the low nibble. Types below 8 are
// plaintext pairing/discovery messages; types >= 8 are encrypted link
// messages sharing a 7-byte header that is also the AEAD associated data.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "audio/codec_mode.hpp"

namespace cpzbm::wire {

inline constexpr uint8_t kVersion = 1;

enum class Type : uint8_t {
    Beacon      = 1,
    PairReq     = 2,
    PairResp    = 3,
    PairConfirm = 4,
    PairNack    = 5,
    // Encrypted:
    Audio     = 8,
    Heartbeat = 9,
    Keepalive = 10,
    Role      = 11,
    RoleAck   = 12,
    Unpair    = 13,
    Region    = 14,  // body: 1 byte cpzbm::Region; peer switches after RegionAck
    RegionAck = 15,
};

inline constexpr bool isEncrypted(Type t) { return static_cast<uint8_t>(t) >= 8; }

inline constexpr std::size_t kNameLen     = 16;
inline constexpr std::size_t kNonceLen    = 8;
inline constexpr std::size_t kPointLen    = 32;
inline constexpr std::size_t kTagLen      = 16;
inline constexpr std::size_t kSecHdrLen   = 7;   // byte0 + pair_id(2) + counter(4)
inline constexpr std::size_t kAeadTagLen  = 16;
inline constexpr std::size_t kMaxPacket   = 255;
inline constexpr std::size_t kMaxSecPlain = kMaxPacket - kSecHdrLen - kAeadTagLen;

using Name    = std::array<char, kNameLen>;  // not NUL-terminated when full
using Nonce   = std::array<uint8_t, kNonceLen>;
using Point   = std::array<uint8_t, kPointLen>;
using Tag     = std::array<uint8_t, kTagLen>;
using Bytes   = std::vector<uint8_t>;

struct Beacon {
    uint32_t dev_id = 0;
    Nonce session{};
    Name name{};
};

struct PairReq {
    uint32_t to   = 0;
    uint32_t from = 0;
    Nonce nonce{};
    Point y{};
    Name name{};
};

struct PairResp {
    uint32_t to   = 0;
    uint32_t from = 0;
    Nonce nonce{};
    Point y{};
    Tag tag{};
};

struct PairConfirm {
    uint32_t to   = 0;
    uint32_t from = 0;
    Tag tag{};
};

struct PairNack {
    uint32_t to   = 0;
    uint32_t from = 0;
};

struct SecHeader {
    Type type        = Type::Keepalive;
    uint16_t pair_id = 0;
    uint32_t counter = 0;
};

// Plaintext bodies of encrypted messages.
struct Audio {
    // Random per baby-pipeline start. A new value tells the parent that the
    // sequence numbers restarted, so its jitter buffer must resynchronise.
    uint8_t stream   = 0;
    uint16_t seq     = 0;
    audio::Mode mode = audio::Mode::M1600;
    uint8_t nframes  = 0;
    Bytes frames;  // nframes * bytes_per_frame
};

inline constexpr uint8_t kBatteryUnknown = 255;

struct Heartbeat {
    int8_t level_dbfs  = -127;  // loudest block since the previous heartbeat
    uint8_t battery    = kBatteryUnknown;  // percent
    bool always_stream = false;            // baby is in "always stream" test mode
};

std::optional<Type> peekType(const Bytes& pkt);

Bytes encode(const Beacon& m);
Bytes encode(const PairReq& m);
Bytes encode(const PairResp& m);
Bytes encode(const PairConfirm& m);
Bytes encode(const PairNack& m);
std::optional<Beacon> decodeBeacon(const Bytes& pkt);
std::optional<PairReq> decodePairReq(const Bytes& pkt);
std::optional<PairResp> decodePairResp(const Bytes& pkt);
std::optional<PairConfirm> decodePairConfirm(const Bytes& pkt);
std::optional<PairNack> decodePairNack(const Bytes& pkt);

Bytes encodeSecHeader(const SecHeader& h);
std::optional<SecHeader> decodeSecHeader(const Bytes& pkt);

Bytes encode(const Audio& m);
std::optional<Audio> decodeAudio(const Bytes& plain);
Bytes encode(const Heartbeat& m);
std::optional<Heartbeat> decodeHeartbeat(const Bytes& plain);

Name makeName(const char* text);

}  // namespace cpzbm::wire
