#include "proto/crypto.hpp"

#include <sodium.h>

#include <cstring>

namespace cpzbm::crypto {
namespace {

constexpr char kDomain[] = "CPZBM-CPace-v1";

void putU32(crypto_hash_sha512_state* st, uint32_t v)
{
    uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                    static_cast<uint8_t>(v >> 24)};
    crypto_hash_sha512_update(st, b, sizeof b);
}

std::array<uint8_t, 12> aeadNonce(uint8_t direction, uint32_t counter)
{
    std::array<uint8_t, 12> n{};
    n[0] = direction & 1U;
    for (int i = 0; i < 4; ++i) n[4 + i] = static_cast<uint8_t>(counter >> (8 * i));
    return n;
}

}  // namespace

bool init() { return sodium_init() >= 0; }

std::string randomCode()
{
    std::string code(6, '0');
    const uint32_t v = randombytes_uniform(1000000U);
    uint32_t x       = v;
    for (int i = 5; i >= 0; --i) {
        code[static_cast<std::size_t>(i)] = static_cast<char>('0' + x % 10);
        x /= 10;
    }
    return code;
}

uint32_t randomU32() { return randombytes_random(); }

wire::Nonce randomNonce()
{
    wire::Nonce n{};
    randombytes_buf(n.data(), n.size());
    return n;
}

bool PakeSession::start(const std::string& code, uint32_t id_a, uint32_t id_b, const wire::Nonce& nonce_a,
                        const wire::Nonce& nonce_b, bool is_a)
{
    if (code.size() != 6) return false;
    for (char c : code)
        if (c < '0' || c > '9') return false;
    is_a_    = is_a;
    id_a_    = id_a;
    id_b_    = id_b;
    nonce_a_ = nonce_a;
    nonce_b_ = nonce_b;
    finished_ = false;

    // Generator from the password and the session context.
    crypto_hash_sha512_state st;
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, reinterpret_cast<const uint8_t*>(kDomain), sizeof kDomain - 1);
    crypto_hash_sha512_update(&st, reinterpret_cast<const uint8_t*>(code.data()), code.size());
    putU32(&st, id_a);
    putU32(&st, id_b);
    crypto_hash_sha512_update(&st, nonce_a.data(), nonce_a.size());
    crypto_hash_sha512_update(&st, nonce_b.data(), nonce_b.size());
    uint8_t h[64];
    crypto_hash_sha512_final(&st, h);
    uint8_t g[crypto_core_ristretto255_BYTES];
    crypto_core_ristretto255_from_hash(g, h);

    for (;;) {
        crypto_core_ristretto255_scalar_random(y_.data());
        if (crypto_scalarmult_ristretto255(y_pub_.data(), y_.data(), g) == 0) break;
    }
    sodium_memzero(h, sizeof h);
    return true;
}

bool PakeSession::finish(const wire::Point& peer_share)
{
    if (!crypto_core_ristretto255_is_valid_point(peer_share.data())) return false;
    uint8_t k[crypto_scalarmult_ristretto255_BYTES];
    if (crypto_scalarmult_ristretto255(k, y_.data(), peer_share.data()) != 0) return false;  // identity
    peer_ = peer_share;

    const wire::Point& y_a = is_a_ ? y_pub_ : peer_;
    const wire::Point& y_b = is_a_ ? peer_ : y_pub_;
    crypto_hash_sha512_state st;
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, reinterpret_cast<const uint8_t*>(kDomain), sizeof kDomain - 1);
    putU32(&st, id_a_);
    putU32(&st, id_b_);
    crypto_hash_sha512_update(&st, nonce_a_.data(), nonce_a_.size());
    crypto_hash_sha512_update(&st, nonce_b_.data(), nonce_b_.size());
    crypto_hash_sha512_update(&st, y_a.data(), y_a.size());
    crypto_hash_sha512_update(&st, y_b.data(), y_b.size());
    crypto_hash_sha512_update(&st, k, sizeof k);
    crypto_hash_sha512_final(&st, isk_.data());
    sodium_memzero(k, sizeof k);
    sodium_memzero(y_.data(), y_.size());
    finished_ = true;
    return true;
}

wire::Tag PakeSession::tag(char who) const
{
    wire::Tag t{};
    const uint8_t label[2] = {static_cast<uint8_t>('T'), static_cast<uint8_t>(who)};
    crypto_generichash(t.data(), t.size(), label, sizeof label, isk_.data(), 32);
    return t;
}

wire::Tag PakeSession::tagA() const { return tag('A'); }
wire::Tag PakeSession::tagB() const { return tag('B'); }

bool PakeSession::verifyPeerTag(const wire::Tag& t) const
{
    if (!finished_) return false;
    const wire::Tag expected = is_a_ ? tagB() : tagA();
    return sodium_memcmp(expected.data(), t.data(), t.size()) == 0;
}

Key PakeSession::linkKey() const
{
    Key k{};
    const uint8_t label[4] = {'l', 'i', 'n', 'k'};
    crypto_generichash(k.data(), k.size(), label, sizeof label, isk_.data() + 32, 32);
    return k;
}

uint16_t PakeSession::pairId() const { return pairIdFromKey(linkKey()); }

uint16_t pairIdFromKey(const Key& key)
{
    uint8_t h[16];
    crypto_generichash(h, sizeof h, key.data(), key.size(), nullptr, 0);
    return static_cast<uint16_t>(h[0] | (h[1] << 8));
}

wire::Bytes seal(const Key& key, uint8_t direction, const wire::SecHeader& header, const wire::Bytes& plain)
{
    if (plain.size() > wire::kMaxSecPlain) return {};
    wire::Bytes out = wire::encodeSecHeader(header);
    const std::size_t hdr = out.size();
    out.resize(hdr + plain.size() + crypto_aead_chacha20poly1305_ietf_ABYTES);
    const auto nonce = aeadNonce(direction, header.counter);
    unsigned long long clen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(out.data() + hdr, &clen, plain.data(), plain.size(), out.data(), hdr,
                                              nullptr, nonce.data(), key.data());
    out.resize(hdr + clen);
    return out;
}

std::optional<Opened> open(const Key& key, uint8_t sender_direction, const wire::Bytes& packet)
{
    const auto header = wire::decodeSecHeader(packet);
    if (!header) return std::nullopt;
    const std::size_t hdr = wire::kSecHdrLen;
    Opened o;
    o.header = *header;
    o.plain.resize(packet.size() - hdr - crypto_aead_chacha20poly1305_ietf_ABYTES);
    const auto nonce = aeadNonce(sender_direction, header->counter);
    unsigned long long mlen = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(o.plain.data(), &mlen, nullptr, packet.data() + hdr,
                                                  packet.size() - hdr, packet.data(), hdr, nonce.data(),
                                                  key.data()) != 0)
        return std::nullopt;
    o.plain.resize(mlen);
    return o;
}

}  // namespace cpzbm::crypto
