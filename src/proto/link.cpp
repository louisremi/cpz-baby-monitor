#include "proto/link.hpp"

#include <algorithm>

namespace cpzbm {
namespace {
constexpr uint64_t kBeaconPeriodMs   = 1000;
constexpr uint64_t kRetryMs          = 1500;
constexpr int kMaxRetries            = 4;
constexpr uint64_t kAwaitConfirmMs   = 6000;
constexpr int kMaxFailedAttempts     = 3;
constexpr uint64_t kPeerStaleMs      = 5000;
constexpr uint64_t kRoleRetryMs      = 1000;
constexpr int kMaxRoleRetries        = 10;
}  // namespace

const char* pairErrorText(PairError e)
{
    switch (e) {
        case PairError::None: return "";
        case PairError::WrongCode: return "Wrong code";
        case PairError::Timeout: return "No answer";
        case PairError::CodeExpired: return "Code expired";
        case PairError::Cancelled: return "Cancelled";
    }
    return "";
}

Link::Link(Identity self, SendFn send, PersistFn persist)
    : self_(self), send_(std::move(send)), persist_(std::move(persist))
{
}

Pairing Link::resumeCounters(Pairing p)
{
    // tx may have advanced by up to a stride since the last save: skip ahead
    // so a nonce is never reused. rx is NOT skipped: a peer that did not
    // restart is still counting from where we last heard it, and skipping
    // would drop its next packets. rx is instead saved every kRxSaveStride and
    // on clean shutdown (flush()), so only a crash leaves a small replay window.
    p.tx_counter += kCounterStride;
    return p;
}

void Link::flush()
{
    if (paired_ && (pairing_.tx_counter != persisted_tx_ || pairing_.rx_counter != persisted_rx_)) persist();
}

void Link::restore(const Pairing& p)
{
    pairing_      = p;
    paired_       = true;
    phase_        = PairPhase::Paired;
    persisted_tx_ = p.tx_counter;
    persisted_rx_ = p.rx_counter;
    persist();  // record the advanced tx counter before using it
}

uint8_t Link::myDirection() const { return self_.dev_id < pairing_.peer_id ? 0 : 1; }

void Link::persist()
{
    persisted_tx_ = pairing_.tx_counter;
    persisted_rx_ = pairing_.rx_counter;
    if (persist_) persist_(pairing_);
}

// ---------------------------------------------------------------- pairing --

void Link::newCode(uint64_t now_ms)
{
    code_            = crypto::randomCode();
    session_         = crypto::randomNonce();
    failed_attempts_ = 0;
    phase_           = PairPhase::Showing;
    next_tx_ms_      = now_ms;
    emit(LinkEvent(LinkEvent::Kind::NewCode));
}

std::string Link::startShowing(uint64_t now_ms)
{
    if (paired_) return {};
    last_error_ = PairError::None;
    newCode(now_ms);
    return code_;
}

void Link::startScanning(uint64_t now_ms)
{
    if (paired_) return;
    (void)now_ms;
    last_error_ = PairError::None;
    discovered_.clear();
    phase_ = PairPhase::Scanning;
}

std::vector<DiscoveredPeer> Link::peers(uint64_t now_ms) const
{
    std::vector<DiscoveredPeer> out;
    for (const auto& p : discovered_)
        if (now_ms - p.seen_ms <= kPeerStaleMs) out.push_back(p);
    return out;
}

bool Link::submitCode(uint32_t peer_id, const std::string& code, uint64_t now_ms)
{
    if (phase_ != PairPhase::Scanning && phase_ != PairPhase::Failed) return false;
    auto it = std::find_if(discovered_.begin(), discovered_.end(),
                           [&](const DiscoveredPeer& p) { return p.dev_id == peer_id; });
    if (it == discovered_.end()) return false;
    peer_nonce_ = crypto::randomNonce();
    if (!pake_.start(code, peer_id, self_.dev_id, it->session, peer_nonce_, /*is_a=*/false)) return false;
    pending_peer_ = peer_id;
    pending_name_ = it->name;
    session_      = it->session;  // A's session nonce this handshake is bound to
    wire::PairReq req{peer_id, self_.dev_id, peer_nonce_, pake_.share(), self_.name};
    cached_req_   = wire::encode(req);
    phase_        = PairPhase::Requesting;
    retries_      = 0;
    next_tx_ms_   = now_ms + kRetryMs;
    last_error_   = PairError::None;
    send_(cached_req_);
    return true;
}

void Link::cancelPairing()
{
    if (paired_) return;
    phase_ = PairPhase::Idle;
    code_.clear();
    discovered_.clear();
}

void Link::fail(PairError e)
{
    last_error_ = e;
    phase_      = PairPhase::Failed;
    emit(LinkEvent(LinkEvent::Kind::PairFailed, e));
}

// A handshake attempt on the showing side did not complete. Each attempt is
// one online guess of the code, so after kMaxFailedAttempts the code changes.
void Link::abandonHandshake(uint64_t now_ms)
{
    ++failed_attempts_;
    cached_resp_.clear();
    pending_peer_ = 0;
    if (failed_attempts_ >= kMaxFailedAttempts) {
        newCode(now_ms);
    } else {
        phase_      = PairPhase::Showing;
        next_tx_ms_ = now_ms;
    }
}

void Link::becomePaired(const crypto::PakeSession& pake, uint32_t peer_id, const wire::Name& peer_name,
                        uint64_t now_ms)
{
    pairing_            = Pairing{};
    pairing_.key        = pake.linkKey();
    pairing_.pair_id    = pake.pairId();
    pairing_.peer_id    = peer_id;
    pairing_.peer_name  = peer_name;
    pairing_.role       = Role::None;
    pairing_.tx_counter = 1;
    pairing_.rx_counter = 0;
    paired_             = true;
    phase_              = PairPhase::Paired;
    code_.clear();
    discovered_.clear();
    last_rx_ms_ = now_ms;
    persist();
    emit(LinkEvent(LinkEvent::Kind::Paired));
}

void Link::beacon(uint64_t now_ms)
{
    wire::Beacon b{self_.dev_id, session_, self_.name};
    send_(wire::encode(b));
    next_tx_ms_ = now_ms + kBeaconPeriodMs;
}

void Link::handlePairing(const wire::Bytes& pkt, wire::Type type, float rssi, uint64_t now_ms)
{
    switch (type) {
        case wire::Type::Beacon: {
            if (phase_ != PairPhase::Scanning && phase_ != PairPhase::Failed) return;
            auto b = wire::decodeBeacon(pkt);
            if (!b || b->dev_id == self_.dev_id) return;
            auto it = std::find_if(discovered_.begin(), discovered_.end(),
                                   [&](const DiscoveredPeer& p) { return p.dev_id == b->dev_id; });
            if (it == discovered_.end()) {
                discovered_.push_back({b->dev_id, b->name, b->session, rssi, now_ms});
            } else {
                it->name    = b->name;
                it->session = b->session;
                it->rssi    = rssi;
                it->seen_ms = now_ms;
            }
            return;
        }
        case wire::Type::PairReq: {
            auto r = wire::decodePairReq(pkt);
            if (!r || r->to != self_.dev_id) return;
            if (phase_ == PairPhase::AwaitConfirm && r->from == pending_peer_ && r->nonce == peer_nonce_) {
                send_(cached_resp_);  // retransmitted request: same answer
                phase_deadline_ = now_ms + kAwaitConfirmMs;
                return;
            }
            if (phase_ != PairPhase::Showing) return;  // one handshake at a time
            crypto::PakeSession pake;
            pake.start(code_, self_.dev_id, r->from, session_, r->nonce, /*is_a=*/true);
            if (!pake.finish(r->y)) return;  // invalid point: ignore, not a guess
            pake_         = pake;
            pending_peer_ = r->from;
            peer_nonce_   = r->nonce;
            pending_name_ = r->name;
            wire::PairResp resp{r->from, self_.dev_id, session_, pake_.share(), pake_.tagA()};
            cached_resp_    = wire::encode(resp);
            phase_          = PairPhase::AwaitConfirm;
            phase_deadline_ = now_ms + kAwaitConfirmMs;
            send_(cached_resp_);
            return;
        }
        case wire::Type::PairResp: {
            if (phase_ != PairPhase::Requesting) return;
            auto r = wire::decodePairResp(pkt);
            if (!r || r->to != self_.dev_id || r->from != pending_peer_) return;
            if (r->nonce != session_) {
                fail(PairError::CodeExpired);
                return;
            }
            if (!pake_.finish(r->y)) return;
            if (!pake_.verifyPeerTag(r->tag)) {
                send_(wire::encode(wire::PairNack{pending_peer_, self_.dev_id}));
                fail(PairError::WrongCode);
                return;
            }
            cached_confirm_ = wire::encode(wire::PairConfirm{pending_peer_, self_.dev_id, pake_.tagB()});
            phase_          = PairPhase::Confirming;
            retries_        = 0;
            next_tx_ms_     = now_ms + kRetryMs;
            send_(cached_confirm_);
            return;
        }
        case wire::Type::PairConfirm: {
            auto c = wire::decodePairConfirm(pkt);
            if (!c || c->to != self_.dev_id) return;
            if (paired_ && c->from == pairing_.peer_id) {
                // Our ack was lost: resend it.
                sendSecure(wire::Type::Keepalive, {});
                return;
            }
            if (phase_ != PairPhase::AwaitConfirm || c->from != pending_peer_) return;
            if (!pake_.verifyPeerTag(c->tag)) {
                abandonHandshake(now_ms);
                return;
            }
            becomePaired(pake_, c->from, pending_name_, now_ms);
            sendSecure(wire::Type::Keepalive, {});  // ack, completes B's handshake
            return;
        }
        case wire::Type::PairNack: {
            auto n = wire::decodePairNack(pkt);
            if (!n || n->to != self_.dev_id) return;
            if (phase_ == PairPhase::AwaitConfirm && n->from == pending_peer_) abandonHandshake(now_ms);
            return;
        }
        default:
            return;
    }
}

// --------------------------------------------------------------- secure --

bool Link::sendSecure(wire::Type type, const wire::Bytes& body)
{
    if (!paired_ || !wire::isEncrypted(type)) return false;
    wire::SecHeader h{type, pairing_.pair_id, pairing_.tx_counter++};
    auto pkt = crypto::seal(pairing_.key, myDirection(), h, body);
    if (pkt.empty()) return false;
    if (pairing_.tx_counter - persisted_tx_ >= kCounterStride) persist();
    send_(std::move(pkt));
    return true;
}

void Link::handleSecure(const wire::Bytes& pkt, float rssi, float snr, uint64_t now_ms)
{
    const bool confirming = phase_ == PairPhase::Confirming;
    if (!paired_ && !confirming) return;
    const auto hdr = wire::decodeSecHeader(pkt);
    if (!hdr) return;
    crypto::Key key;
    uint8_t peer_dir;
    if (confirming) {
        key      = pake_.linkKey();
        peer_dir = pending_peer_ < self_.dev_id ? 0 : 1;
        if (hdr->pair_id != pake_.pairId()) return;
    } else {
        key      = pairing_.key;
        peer_dir = myDirection() ^ 1U;
        if (hdr->pair_id != pairing_.pair_id) return;
    }
    auto opened = crypto::open(key, peer_dir, pkt);
    if (!opened) return;
    if (confirming) {
        becomePaired(pake_, pending_peer_, pending_name_, now_ms);
    }
    if (opened->header.counter <= pairing_.rx_counter) return;  // replay
    pairing_.rx_counter = opened->header.counter;
    if (pairing_.rx_counter - persisted_rx_ >= kRxSaveStride) persist();
    last_rx_ms_ = now_ms;
    last_rssi_  = rssi;
    last_snr_   = snr;

    switch (opened->header.type) {
        case wire::Type::Role: {
            if (opened->plain.size() != 1) return;
            const auto peer_role = static_cast<Role>(opened->plain[0]);
            const Role mine      = oppositeRole(peer_role);
            sendSecure(wire::Type::RoleAck, {static_cast<uint8_t>(peer_role)});
            if (mine != pairing_.role) {
                pairing_.role = mine;
                role_pending_ = false;
                persist();
                emit(LinkEvent(LinkEvent::Kind::RoleChanged));
            }
            return;
        }
        case wire::Type::RoleAck: {
            if (opened->plain.size() == 1 && static_cast<Role>(opened->plain[0]) == pairing_.role)
                role_pending_ = false;
            return;
        }
        case wire::Type::Unpair:
            forgetPairing();
            return;
        default: {
            LinkEvent e(LinkEvent::Kind::Message);
            e.type = opened->header.type;
            e.body = std::move(opened->plain);
            e.rssi = rssi;
            e.snr  = snr;
            emit(std::move(e));
            return;
        }
    }
}

void Link::setRole(Role role, uint64_t now_ms)
{
    if (!paired_) return;
    pairing_.role = role;
    persist();
    role_pending_ = true;
    role_retries_ = 0;
    role_next_ms_ = now_ms;
    emit(LinkEvent(LinkEvent::Kind::RoleChanged));
}

void Link::unpair()
{
    if (!paired_) return;
    // Best effort, sent twice: the peer may not hear it, in which case the user
    // unpairs there too.
    sendSecure(wire::Type::Unpair, {});
    sendSecure(wire::Type::Unpair, {});
    forgetPairing();
}

void Link::forgetPairing()
{
    paired_       = false;
    pairing_      = Pairing{};
    phase_        = PairPhase::Idle;
    role_pending_ = false;
    if (persist_) persist_(pairing_);  // peer_id 0: the store deletes the file
    emit(LinkEvent(LinkEvent::Kind::Unpaired));
}

// ------------------------------------------------------------- dispatch --

void Link::onPacket(const wire::Bytes& pkt, float rssi, float snr, uint64_t now_ms)
{
    const auto type = wire::peekType(pkt);
    if (!type) return;
    if (wire::isEncrypted(*type))
        handleSecure(pkt, rssi, snr, now_ms);
    else
        handlePairing(pkt, *type, rssi, now_ms);
}

void Link::tick(uint64_t now_ms)
{
    switch (phase_) {
        case PairPhase::Showing:
            if (now_ms >= next_tx_ms_) beacon(now_ms);
            break;
        case PairPhase::AwaitConfirm:
            if (now_ms >= phase_deadline_) abandonHandshake(now_ms);
            break;
        case PairPhase::Requesting:
        case PairPhase::Confirming:
            if (now_ms >= next_tx_ms_) {
                if (++retries_ > kMaxRetries) {
                    fail(PairError::Timeout);
                    break;
                }
                send_(phase_ == PairPhase::Requesting ? cached_req_ : cached_confirm_);
                next_tx_ms_ = now_ms + kRetryMs;
            }
            break;
        default:
            break;
    }
    if (paired_ && role_pending_ && now_ms >= role_next_ms_) {
        if (role_retries_++ >= kMaxRoleRetries) {
            role_pending_ = false;  // give up; peer applies it when it next hears ROLE
        } else {
            sendSecure(wire::Type::Role, {static_cast<uint8_t>(pairing_.role)});
            role_next_ms_ = now_ms + kRoleRetryMs;
        }
    }
}

bool Link::pollEvent(LinkEvent* out)
{
    if (events_.empty()) return false;
    *out = std::move(events_.front());
    events_.pop_front();
    return true;
}

}  // namespace cpzbm
