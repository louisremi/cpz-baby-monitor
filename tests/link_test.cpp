// Two Links over an in-memory half-duplex channel with configurable loss.

#include <deque>
#include <memory>
#include <optional>
#include <cstdio>
#include <random>

#include "proto/link.hpp"
#include "store/store.hpp"
#include "test_support.hpp"

using namespace cpzbm;

namespace {

struct Net {
    std::mt19937 rng{1234};
    double loss = 0.0;
    std::deque<wire::Bytes> to_a, to_b;
    bool drop() { return std::uniform_real_distribution<double>(0, 1)(rng) < loss; }
};

struct Node {
    Identity id;
    std::shared_ptr<Link> link;
    std::optional<Pairing> saved;
    int persists = 0;
};

Node make(Net* net, bool is_a, uint32_t dev_id, const char* name)
{
    Node n;
    n.id.dev_id = dev_id;
    n.id.name   = wire::makeName(name);
    auto* q     = is_a ? &net->to_b : &net->to_a;
    n.link = std::make_shared<Link>(
        n.id,
        [net, q](wire::Bytes b) {
            if (!net->drop()) q->push_back(std::move(b));
        },
        nullptr);
    return n;
}

void pump(Net& net, Node& a, Node& b, uint64_t& now, uint64_t ms)
{
    for (uint64_t t = 0; t < ms; t += 10) {
        now += 10;
        a.link->tick(now);
        b.link->tick(now);
        while (!net.to_a.empty()) {
            auto p = std::move(net.to_a.front());
            net.to_a.pop_front();
            a.link->onPacket(p, -50, 9, now);
        }
        while (!net.to_b.empty()) {
            auto p = std::move(net.to_b.front());
            net.to_b.pop_front();
            b.link->onPacket(p, -50, 9, now);
        }
    }
}

std::vector<LinkEvent> drain(Node& n)
{
    std::vector<LinkEvent> v;
    LinkEvent e;
    while (n.link->pollEvent(&e)) v.push_back(e);
    return v;
}

bool pairBoth(double loss, bool wrong_first, unsigned seed = 1234)
{
    Net net;
    net.rng.seed(seed);
    net.loss = loss;
    uint64_t now = 1000;
    auto a = make(&net, true, 0xA0A0A0A0, "alpha");
    auto b = make(&net, false, 0xB0B0B0B0, "bravo");
    const std::string code = a.link->startShowing(now);
    CHECK(code.size() == 6);
    b.link->startScanning(now);
    pump(net, a, b, now, 3000);
    auto peers = b.link->peers(now);
    if (peers.empty()) {
        std::printf("  seed %u: no beacon\n", seed);
        return false;
    }
    CHECK(peers[0].dev_id == a.id.dev_id);
    CHECK(std::string(peers[0].name.data(), 5) == "alpha");

    if (wrong_first) {
        std::string wrong = code;
        wrong[0]          = wrong[0] == '9' ? '0' : static_cast<char>(wrong[0] + 1);
        CHECK(b.link->submitCode(a.id.dev_id, wrong, now));
        pump(net, a, b, now, 8000);
        CHECK(b.link->phase() == PairPhase::Failed);
        CHECK(b.link->lastError() == PairError::WrongCode || b.link->lastError() == PairError::Timeout);
        CHECK(!a.link->paired());
        CHECK(a.link->phase() == PairPhase::Showing);
        CHECK(a.link->code() == code);  // one failure does not rotate the code
    }

    CHECK(b.link->submitCode(a.id.dev_id, code, now));
    pump(net, a, b, now, 10000);
    if (!(a.link->paired() && b.link->paired())) {
        std::printf("  seed %u: not paired, A phase %d paired %d, B phase %d paired %d err %d\n", seed,
                    static_cast<int>(a.link->phase()), a.link->paired(), static_cast<int>(b.link->phase()),
                    b.link->paired(), static_cast<int>(b.link->lastError()));
        return false;
    }
    CHECK(a.link->pairing().key == b.link->pairing().key);
    CHECK(a.link->pairing().peer_id == b.id.dev_id);
    CHECK(b.link->pairing().peer_id == a.id.dev_id);
    CHECK(std::string(a.link->pairing().peer_name.data(), 5) == "bravo");

    // Role sync: A says Baby -> B becomes Parent.
    drain(a);
    drain(b);
    a.link->setRole(Role::Baby, now);
    pump(net, a, b, now, 15000);
    CHECK(b.link->pairing().role == Role::Parent);
    CHECK(a.link->pairing().role == Role::Baby);

    // Messages both ways, including after loss.
    int got = 0;
    for (int i = 0; i < 50; ++i) {
        a.link->sendSecure(wire::Type::Heartbeat, wire::encode(wire::Heartbeat{-30, 50, 0}));
        pump(net, a, b, now, 100);
        for (auto& e : drain(b))
            if (e.kind == LinkEvent::Kind::Message && e.type == wire::Type::Heartbeat) ++got;
    }
    CHECK(got > 0);
    if (loss == 0) CHECK(got == 50);
    return true;
}

}  // namespace

int main()
{
    CHECK(crypto::init());
    CHECK(pairBoth(0.0, false));
    CHECK(pairBoth(0.0, true));
    // 30% loss each way: retries must still complete within the time budget.
    int ok = 0;
    for (unsigned i = 0; i < 20; ++i) ok += pairBoth(0.3, false, 100 + i) ? 1 : 0;
    std::printf("30%% loss: %d/20 paired\n", ok);
    CHECK(ok >= 17);

    // Three wrong guesses rotate the code.
    {
        Net net;
        uint64_t now = 0;
        auto a = make(&net, true, 1, "a");
        auto b = make(&net, false, 2, "b");
        const auto code = a.link->startShowing(now);
        b.link->startScanning(now);
        pump(net, a, b, now, 2000);
        for (int i = 0; i < 3; ++i) {
            std::string wrong = code;
            wrong[5]          = wrong[5] == '9' ? '0' : static_cast<char>(wrong[5] + 1);
            pump(net, a, b, now, 1500);  // let a fresh beacon arrive
            CHECK(b.link->submitCode(1, wrong, now));
            pump(net, a, b, now, 8000);
        }
        CHECK(a.link->code() != code);
        CHECK(!a.link->paired());
        // B now holds a stale session; it must refresh from beacons.
        b.link->startScanning(now);
        pump(net, a, b, now, 2000);
        CHECK(b.link->submitCode(1, a.link->code(), now));
        pump(net, a, b, now, 5000);
        CHECK(a.link->paired() && b.link->paired());
    }

    // Replay protection and persistence round-trip.
    {
        Net net;
        uint64_t now = 0;
        auto a = make(&net, true, 10, "a");
        auto b = make(&net, false, 20, "b");
        const auto code = a.link->startShowing(now);
        b.link->startScanning(now);
        pump(net, a, b, now, 2000);
        CHECK(b.link->submitCode(10, code, now));
        pump(net, a, b, now, 5000);
        CHECK(a.link->paired() && b.link->paired());
        drain(b);
        // Capture a packet and replay it.
        std::deque<wire::Bytes> captured;
        a.link->sendSecure(wire::Type::Heartbeat, wire::encode(wire::Heartbeat{}));
        captured = net.to_b;
        pump(net, a, b, now, 20);
        CHECK(drain(b).size() == 1);
        for (auto& p : captured) b.link->onPacket(p, 0, 0, now);
        CHECK(drain(b).empty());

        // Store serialisation.
        Pairing p = a.link->pairing();
        auto s    = store::serialize(p);
        auto p2   = store::deserialize(s);
        CHECK(p2 && p2->key == p.key && p2->peer_id == p.peer_id && p2->tx_counter == p.tx_counter);
        s[10] ^= 1;  // corrupt the key -> pair_id check fails
        CHECK(!store::deserialize(s));
        auto resumed = Link::resumeCounters(p);
        CHECK(resumed.tx_counter == p.tx_counter + Link::kCounterStride);
        CHECK(resumed.rx_counter == p.rx_counter);

        // Restart replay: B persists, A sends 10 packets which an attacker
        // records, B restarts from its store (clean shutdown -> flush), and
        // the recording is replayed. Nothing may be accepted.
        {
            std::optional<Pairing> b_saved;
            Link b2(b.id, [](wire::Bytes) {}, [&](const Pairing& pp) { b_saved = pp; });
            b2.restore(Link::resumeCounters(b.link->pairing()));
            std::deque<wire::Bytes> recorded;
            for (int i = 0; i < 10; ++i) {
                a.link->sendSecure(wire::Type::Heartbeat, wire::encode(wire::Heartbeat{}));
                recorded.push_back(net.to_b.back());
                b2.onPacket(net.to_b.back(), 0, 0, now);
            }
            net.to_b.clear();
            LinkEvent ev;
            int live = 0;
            while (b2.pollEvent(&ev)) live += ev.kind == LinkEvent::Kind::Message;
            CHECK(live == 10);
            b2.flush();  // clean shutdown
            CHECK(b_saved);
            Link b3(b.id, [](wire::Bytes) {}, nullptr);
            b3.restore(Link::resumeCounters(*b_saved));
            for (auto& r : recorded) b3.onPacket(r, 0, 0, now);
            int replayed = 0;
            while (b3.pollEvent(&ev)) replayed += ev.kind == LinkEvent::Kind::Message;
            CHECK(replayed == 0);
            // ...while A's next genuine packet is still accepted.
            a.link->sendSecure(wire::Type::Heartbeat, wire::encode(wire::Heartbeat{}));
            b3.onPacket(net.to_b.back(), 0, 0, now);
            net.to_b.clear();
            int fresh = 0;
            while (b3.pollEvent(&ev)) fresh += ev.kind == LinkEvent::Kind::Message;
            CHECK(fresh == 1);
            // Crash (no flush): at most kRxSaveStride-1 recorded packets replay.
            std::optional<Pairing> c_saved;
            Link b4(b.id, [](wire::Bytes) {}, [&](const Pairing& pp) { c_saved = pp; });
            b4.restore(Link::resumeCounters(b3.pairing()));
            std::deque<wire::Bytes> rec2;
            for (uint32_t i = 0; i < 2 * Link::kRxSaveStride; ++i) {
                a.link->sendSecure(wire::Type::Heartbeat, wire::encode(wire::Heartbeat{}));
                rec2.push_back(net.to_b.back());
                b4.onPacket(net.to_b.back(), 0, 0, now);
            }
            net.to_b.clear();
            Link b5(b.id, [](wire::Bytes) {}, nullptr);
            b5.restore(Link::resumeCounters(*c_saved));
            for (auto& r : rec2) b5.onPacket(r, 0, 0, now);
            int crash_replayed = 0;
            while (b5.pollEvent(&ev)) crash_replayed += ev.kind == LinkEvent::Kind::Message;
            CHECK(crash_replayed < static_cast<int>(Link::kRxSaveStride));
        }

        // Unpair propagates.
        a.link->unpair();
        pump(net, a, b, now, 100);
        CHECK(!a.link->paired());
        CHECK(!b.link->paired());
    }

    // Link-loss timer (what the parent's LINK LOST banner shows).
    CHECK(linkLost(5000, 0));  // never heard
    CHECK(!linkLost(kLinkLostMs, 1));
    CHECK(linkLost(kLinkLostMs + 2, 1));
    {
        Net net;
        uint64_t now = 0;
        auto a = make(&net, true, 30, "baby");
        auto b = make(&net, false, 40, "parent");
        const auto code = a.link->startShowing(now);
        b.link->startScanning(now);
        pump(net, a, b, now, 2000);
        CHECK(b.link->submitCode(30, code, now));
        pump(net, a, b, now, 5000);
        CHECK(a.link->paired() && b.link->paired());
        // Baby heartbeats every 2 s: link stays up.
        for (int i = 0; i < 10; ++i) {
            a.link->sendSecure(wire::Type::Heartbeat, wire::encode(wire::Heartbeat{}));
            pump(net, a, b, now, 2000);
            CHECK(!linkLost(now, b.link->lastRxMs()));
        }
        // Baby goes silent (out of range / off): lost only after kLinkLostMs.
        const uint64_t silent_from = now;
        while (!linkLost(now, b.link->lastRxMs())) pump(net, a, b, now, 100);
        const uint64_t detected = now - silent_from;
        std::printf("link loss detected after %llu ms of silence\n", static_cast<unsigned long long>(detected));
        CHECK(detected > kLinkLostMs - 2000 && detected <= kLinkLostMs + 200);
        // Packets that fail authentication (wrong key) do not count as alive.
        crypto::Key wrong{};
        b.link->onPacket(crypto::seal(wrong, 0, {wire::Type::Heartbeat, b.link->pairing().pair_id, 999999}, {}), 0, 0,
                         now);
        CHECK(linkLost(now, b.link->lastRxMs()));
        // Back in range: the next heartbeat clears it.
        a.link->sendSecure(wire::Type::Heartbeat, wire::encode(wire::Heartbeat{}));
        pump(net, a, b, now, 50);
        CHECK(!linkLost(now, b.link->lastRxMs()));
    }

    std::puts("link_test OK");
    return 0;
}
