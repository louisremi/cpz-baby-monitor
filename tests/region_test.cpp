// RegionHandshake: two devices over a channel with configurable loss must
// always converge on the same band.

#include <cstdio>
#include <deque>
#include <random>

#include "proto/region_handshake.hpp"
#include "store/store.hpp"
#include "test_support.hpp"

using namespace cpzbm;

namespace {

struct Msg {
    bool ack;
    Region r;
};

struct Side {
    RegionHandshake hs{Region::US915};
    Region band = Region::US915;  // what the radio is tuned to
    std::deque<Msg> inbox;
    int retunes = 0;
};

// Delivers a message only if both ends are on the same band (as on air).
void deliver(std::mt19937& rng, double loss, const Side& from, Side& to, Msg m)
{
    if (from.band != to.band) return;
    if (std::uniform_real_distribution<double>(0, 1)(rng) < loss) return;
    to.inbox.push_back(m);
}

void apply(std::mt19937& rng, double loss, Side& self, Side& peer, const RegionHandshake::Action& a)
{
    if (a.send_request) deliver(rng, loss, self, peer, {false, *a.send_request});
    if (a.send_ack) deliver(rng, loss, self, peer, {true, *a.send_ack});
    if (a.retune) {
        self.band = *a.retune;
        ++self.retunes;
    }
}

// Returns true if both sides end up on `target`.
bool run(double loss, unsigned seed, Region target, bool peer_off = false)
{
    std::mt19937 rng(seed);
    Side a, b;
    uint64_t now = 0;
    CHECK(a.hs.request(target, now));
    for (int step = 0; step < 2000; ++step, now += 10) {
        for (Side* s : {&a, &b}) {
            Side& peer = s == &a ? b : a;
            if (peer_off && s == &b) {
                s->inbox.clear();
                continue;
            }
            while (!s->inbox.empty()) {
                const Msg m = s->inbox.front();
                s->inbox.pop_front();
                apply(rng, loss, *s, peer, m.ack ? s->hs.onAck(m.r) : s->hs.onRegion(m.r, now));
            }
            apply(rng, loss, *s, peer, s->hs.tick(now, false));
        }
    }
    CHECK(!a.hs.busy());
    CHECK(a.retunes == 1);
    if (peer_off) return a.band == target && a.hs.lastSwitchUnacked();
    return a.band == target && b.band == target && b.retunes == 1;
}

}  // namespace

int main()
{
    // Lossless: request -> ack -> both retune exactly once.
    CHECK(run(0.0, 1, Region::EU868));
    // Requests to the current band are ignored.
    {
        RegionHandshake hs(Region::EU868);
        CHECK(!hs.request(Region::EU868, 0));
        CHECK(!hs.busy());
    }
    // A responder acks even when already on the band (requester's retry
    // after it switched) and does not retune again.
    {
        RegionHandshake hs(Region::EU868);
        auto act = hs.onRegion(Region::EU868, 0);
        CHECK(act.send_ack == Region::EU868);
        CHECK(!act.retune && !hs.busy());
    }
    // The responder holds the old band long enough to ack a retry.
    {
        RegionHandshake hs(Region::US915);
        CHECK(hs.onRegion(Region::EU868, 0).send_ack == Region::EU868);
        CHECK(!hs.tick(RegionHandshake::kRetryMs, false).retune);
        CHECK(!hs.tick(RegionHandshake::kResponderHoldMs + 1, true).retune);  // ack still in flight
        CHECK(hs.tick(RegionHandshake::kResponderHoldMs + 1, false).retune == Region::EU868);
    }
    // Lossy channel: both sides converge in the vast majority of runs.
    for (double loss : {0.1, 0.3}) {
        int ok = 0;
        for (unsigned seed = 0; seed < 50; ++seed) ok += run(loss, 100 + seed, Region::EU868) ? 1 : 0;
        std::printf("region switch, %.0f%% loss: %d/50 converged\n", loss * 100, ok);
        CHECK(ok >= (loss < 0.2 ? 50 : 45));
    }
    // Peer off: requester gives up and switches anyway (flagged unacked).
    CHECK(run(0.0, 7, Region::EU868, true));

    // On-air byte parsing.
    CHECK(regionFromByte(0) == Region::US915);
    CHECK(regionFromByte(1) == Region::EU868);
    CHECK(!regionFromByte(2));
    CHECK(store::parseRegion("EU868\n") == Region::EU868);
    CHECK(store::parseRegion("US915") == Region::US915);
    CHECK(!store::parseRegion("eu"));

    std::puts("region_test OK");
    return 0;
}
