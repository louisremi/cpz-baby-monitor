#include "proto/crypto.hpp"
#include "test_support.hpp"

using namespace cpzbm;

namespace {

struct Pair {
    crypto::PakeSession a, b;
    bool ok = false;
};

Pair run(const std::string& code_a, const std::string& code_b)
{
    Pair p;
    const auto na = crypto::randomNonce(), nb = crypto::randomNonce();
    CHECK(p.a.start(code_a, 100, 200, na, nb, true));
    CHECK(p.b.start(code_b, 100, 200, na, nb, false));
    CHECK(p.a.finish(p.b.share()));
    CHECK(p.b.finish(p.a.share()));
    p.ok = p.b.verifyPeerTag(p.a.tagA()) && p.a.verifyPeerTag(p.b.tagB());
    return p;
}

}  // namespace

int main()
{
    CHECK(crypto::init());

    for (int i = 0; i < 20; ++i) {
        const auto c = crypto::randomCode();
        CHECK(c.size() == 6);
        for (char ch : c) CHECK(ch >= '0' && ch <= '9');
    }

    // Same code: both sides agree on tags, key and pair id.
    auto good = run("123456", "123456");
    CHECK(good.ok);
    CHECK(good.a.linkKey() == good.b.linkKey());
    CHECK(good.a.pairId() == good.b.pairId());

    // Wrong code: tags fail and keys differ.
    auto bad = run("123456", "123457");
    CHECK(!bad.ok);
    CHECK(!bad.b.verifyPeerTag(bad.a.tagA()));
    CHECK(bad.a.linkKey() != bad.b.linkKey());

    // Malformed codes are refused.
    crypto::PakeSession s;
    CHECK(!s.start("12345", 1, 2, {}, {}, true));
    CHECK(!s.start("12345a", 1, 2, {}, {}, true));

    // Identity / invalid points are rejected (active attack, not a guess).
    CHECK(s.start("000000", 1, 2, {}, {}, true));
    wire::Point zero{};
    CHECK(!s.finish(zero));
    wire::Point junk;
    junk.fill(0xFF);
    CHECK(!s.finish(junk));

    // AEAD.
    const auto key = good.a.linkKey();
    wire::SecHeader h{wire::Type::Audio, good.a.pairId(), 42};
    wire::Bytes msg{1, 2, 3, 4, 5};
    auto pkt = crypto::seal(key, 0, h, msg);
    CHECK(pkt.size() == wire::kSecHdrLen + msg.size() + wire::kAeadTagLen);
    auto o = crypto::open(key, 0, pkt);
    CHECK(o && o->plain == msg && o->header.counter == 42 && o->header.type == wire::Type::Audio);
    // Wrong direction (nonce) fails.
    CHECK(!crypto::open(key, 1, pkt));
    // Tampered header (counter) fails: header is associated data.
    auto t1 = pkt;
    t1[3] ^= 1;
    CHECK(!crypto::open(key, 0, t1));
    // Tampered type byte fails.
    auto t2 = pkt;
    t2[0]   = static_cast<uint8_t>((wire::kVersion << 4) | static_cast<uint8_t>(wire::Type::Heartbeat));
    CHECK(!crypto::open(key, 0, t2));
    // Tampered body fails.
    auto t3 = pkt;
    t3[wire::kSecHdrLen] ^= 0x80;
    CHECK(!crypto::open(key, 0, t3));
    // Wrong key fails.
    CHECK(!crypto::open(bad.a.linkKey(), 0, pkt));
    // Empty body is allowed (keepalive).
    auto ka = crypto::seal(key, 1, {wire::Type::Keepalive, 1, 1}, {});
    CHECK(crypto::open(key, 1, ka) && crypto::open(key, 1, ka)->plain.empty());
    // Oversize plaintext refused.
    CHECK(crypto::seal(key, 0, h, wire::Bytes(wire::kMaxSecPlain + 1)).empty());

    std::puts("crypto_test OK");
    return 0;
}
