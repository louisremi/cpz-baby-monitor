#include "app/app.hpp"

#include <spdlog/spdlog.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "store/store.hpp"

namespace cpzbm {
namespace {

constexpr int kBlock                   = 320;   // 40 ms at 8 kHz = one codec2 frame
constexpr uint64_t kHeartbeatMs        = 2000;
constexpr uint64_t kKeepaliveMs        = 4000;
constexpr uint64_t kWaveColumnMs       = 40;
constexpr uint64_t kStreamStallMs      = 450;   // play a lone packet if no follow-up arrives
constexpr std::size_t kPacketSamples   = kBlock * kFramesPerPacket;

std::string hostName()
{
    char buf[64] = {};
    if (gethostname(buf, sizeof buf - 1) != 0 || !buf[0]) return "cardputer";
    return buf;
}

}  // namespace

Controller::Controller() = default;
Controller::~Controller() { stop(); }

bool Controller::start()
{
    crypto::init();
    config_dir_ = store::configDir();
    identity_   = store::loadOrCreateIdentity(config_dir_, hostName().c_str());
    const std::string dir = config_dir_;
    link_ = std::make_unique<Link>(
        identity_, [this](wire::Bytes b) { radio_.send(std::move(b)); },
        [dir](const Pairing& p) {
            if (!store::savePairing(dir, p)) spdlog::error("Store: failed to save pairing");
        });
    if (auto p = store::loadPairing(config_dir_)) {
        link_->restore(Link::resumeCounters(*p));
        spdlog::info("Store: restored pairing with {:08x}, role {}", p->peer_id, static_cast<int>(p->role));
    }
    spdlog::info("Identity: {:08x} '{}'", identity_.dev_id,
                 std::string(identity_.name.data(), strnlen(identity_.name.data(), identity_.name.size())));
    status_.region   = store::loadRegion(config_dir_, Region::US915);
    status_.radio_ok = radio_.start(profileFor(status_.region));
    if (!status_.radio_ok) status_.radio_error = radio_.error();
    return status_.radio_ok;
}

void Controller::applyRegion(Region r)
{
    if (!store::saveRegion(config_dir_, r)) spdlog::error("Store: failed to save region");
    status_.region = r;
    radio_.stop();
    status_.radio_ok = radio_.start(profileFor(r));
    status_.radio_error = status_.radio_ok ? "" : radio_.error();
    spdlog::info("Region: now {}", regionName(r));
}

void Controller::requestRegion(Region r, uint64_t now_ms)
{
    if (r == status_.region || region_req_pending_ || region_switch_due_) return;
    if (!link_->paired()) {
        applyRegion(r);
        return;
    }
    region_req_pending_     = true;
    region_req_             = r;
    region_retries_         = 0;
    region_next_ms_         = now_ms;
    status_.region_switching = true;
}

void Controller::regionTick(uint64_t now_ms)
{
    constexpr int kMaxRegionRetries   = 8;
    constexpr uint64_t kRegionRetryMs = 700;
    if (region_req_pending_ && now_ms >= region_next_ms_) {
        if (region_retries_++ >= kMaxRegionRetries) {
            // No ack heard: the peer may have switched with our ack lost, or be
            // off. Retune anyway so both ends converge on the requested band.
            spdlog::warn("Region: no ack from peer, switching anyway");
            region_req_pending_      = false;
            status_.region_switching = false;
            applyRegion(region_req_);
        } else {
            link_->sendSecure(wire::Type::Region, {static_cast<uint8_t>(region_req_)});
            region_next_ms_ = now_ms + kRegionRetryMs;
        }
    }
    if (region_switch_due_ && now_ms >= region_switch_by_ && !radio_.txBusy()) {
        region_switch_due_       = false;
        status_.region_switching = false;
        applyRegion(region_switch_to_);
    }
}

void Controller::stop()
{
    leaveRole();
    radio_.stop();
}

bool Controller::pollUiEvent(LinkEvent* e)
{
    if (ui_events_.empty()) return false;
    *e = std::move(ui_events_.front());
    ui_events_.erase(ui_events_.begin());
    return true;
}

void Controller::enterRole(Role role)
{
    if (role == active_role_) return;
    leaveRole();
    active_role_ = role;
    status_.audio_error.clear();
    if (role == Role::Baby) {
        enc_ = std::make_unique<audio::Codec>(audio::Mode::M1600);
        frames_.clear();
        preroll_.clear();
        nframes_ = 0;
        if (!capture_.start(kBlock)) status_.audio_error = capture_.error();
        next_heartbeat_ms_ = 0;
    } else if (role == Role::Parent) {
        dec_ = std::make_unique<audio::Codec>(audio::Mode::M1600);
        jitter_.reset();
        if (!playback_.start()) status_.audio_error = playback_.error();
        playback_.setMuted(status_.speaker_muted);
        std::fill(std::begin(status_.wave), std::end(status_.wave), 0.0f);
        status_.wave_head = 0;
        pending_wave_.clear();
    }
    spdlog::info("Role: entered {}", role == Role::Baby ? "baby" : role == Role::Parent ? "parent" : "none");
}

void Controller::leaveRole()
{
    capture_.stop();
    playback_.stop();
    enc_.reset();
    dec_.reset();
    active_role_ = Role::None;
}

void Controller::toggleMute()
{
    status_.speaker_muted = !status_.speaker_muted;
    playback_.setMuted(status_.speaker_muted);
}

void Controller::adjustVox(float delta_db)
{
    const float t = std::clamp(vox_.threshold() + delta_db, -70.0f, -10.0f);
    vox_.setThreshold(t);
    status_.vox_dbfs = t;
}

void Controller::pushWave(float v)
{
    status_.wave[status_.wave_head] = v;
    status_.wave_head               = (status_.wave_head + 1) % kWaveColumns;
}

void Controller::readBattery(uint64_t now_ms)
{
    if (now_ms < next_battery_ms_) return;
    next_battery_ms_ = now_ms + 30000;
    std::ifstream f("/sys/class/power_supply/bq27220-0/capacity");
    int v = -1;
    if (f >> v && v >= 0 && v <= 100) status_.battery = static_cast<uint8_t>(v);
}

void Controller::tick(uint64_t now_ms)
{
    status_.now_ms = now_ms;
    readBattery(now_ms);
    RxPacket pkt;
    while (radio_.receive(&pkt)) link_->onPacket(pkt.bytes, pkt.rssi, pkt.snr, now_ms);
    link_->tick(now_ms);

    LinkEvent e;
    while (link_->pollEvent(&e)) {
        if (e.kind == LinkEvent::Kind::Message)
            onMessage(e, now_ms);
        else
            ui_events_.push_back(std::move(e));
    }

    const auto rs        = radio_.stats();
    status_.tx_packets   = rs.tx_ok;
    status_.rx_packets   = rs.rx;
    status_.last_rx_ms   = link_->lastRxMs();
    status_.rssi         = link_->lastRssi();
    status_.snr          = link_->lastSnr();

    regionTick(now_ms);
    if (active_role_ == Role::Baby) babyTick(now_ms);
    if (active_role_ == Role::Parent) parentTick(now_ms);
}

// ------------------------------------------------------------------ baby --

void Controller::babyTick(uint64_t now_ms)
{
    std::vector<int16_t> block;
    while (capture_.read(&block)) {
        const float db = audio::rmsDbfs(block.data(), block.size());
        status_.mic_dbfs = db;
        level_peak_      = std::max(level_peak_, db);
        const bool open  = vox_.update(db) || status_.always_stream;
        const bool was   = status_.gate_open;
        status_.gate_open = open;
        // VOX decides on the raw mic level; the AGC only shapes what is sent.
        agc_.process(block.data(), block.size());
        status_.agc_db = agc_.gainDb();
        enc_->encode(block.data(), &frames_);
        if (++nframes_ < kFramesPerPacket) continue;

        if (open && link_->paired()) {
            // On gate opening, also send the previous 320 ms so the start of
            // the sound (which crossed the threshold mid-packet) is heard.
            if (!was && !preroll_.empty()) {
                wire::Audio pre{audio_seq_++, static_cast<uint8_t>(audio::Mode::M1600),
                                static_cast<uint8_t>(kFramesPerPacket), preroll_};
                link_->sendSecure(wire::Type::Audio, wire::encode(pre));
            }
            wire::Audio a{audio_seq_++, static_cast<uint8_t>(audio::Mode::M1600),
                          static_cast<uint8_t>(kFramesPerPacket), frames_};
            link_->sendSecure(wire::Type::Audio, wire::encode(a));
            ++status_.audio_packets;
            preroll_.clear();
        } else {
            preroll_ = frames_;
        }
        frames_.clear();
        nframes_ = 0;
    }

    if (link_->paired() && !status_.gate_open && now_ms >= next_heartbeat_ms_) {
        wire::Heartbeat hb;
        hb.level_dbfs = static_cast<int8_t>(std::clamp(level_peak_, -127.0f, 0.0f));
        hb.battery    = status_.battery;
        hb.flags      = static_cast<uint8_t>((status_.gate_open ? 1 : 0) | (status_.always_stream ? 2 : 0));
        link_->sendSecure(wire::Type::Heartbeat, wire::encode(hb));
        next_heartbeat_ms_ = now_ms + kHeartbeatMs;
        level_peak_        = -100;
    }
}

// ---------------------------------------------------------------- parent --

void Controller::onMessage(const LinkEvent& e, uint64_t now_ms)
{
    // The baby only listens between its own transmissions, so answer right
    // after hearing from it: our keepalive lands in its receive window.
    if (active_role_ == Role::Parent && now_ms >= next_keepalive_ms_) {
        link_->sendSecure(wire::Type::Keepalive, {});
        next_keepalive_ms_ = now_ms + kKeepaliveMs;
    }
    if (e.type == wire::Type::Region && e.body.size() == 1 && e.body[0] <= 1) {
        const auto r = static_cast<Region>(e.body[0]);
        link_->sendSecure(wire::Type::RegionAck, {e.body[0]});
        if (r != status_.region && !region_switch_due_) {
            region_switch_due_       = true;
            region_switch_to_        = r;
            // Stay on the old band long enough to ack the requester's retries
            // (every 700 ms) too: a single ack can be lost to a collision.
            region_switch_by_        = now_ms + 1500;
            status_.region_switching = true;
            spdlog::info("Region: peer requested {}", regionName(r));
        }
        return;
    }
    if (e.type == wire::Type::RegionAck) {
        if (region_req_pending_ && e.body.size() == 1 && e.body[0] == static_cast<uint8_t>(region_req_)) {
            region_req_pending_      = false;
            status_.region_switching = false;
            applyRegion(region_req_);
        }
        return;
    }
    if (e.type == wire::Type::Heartbeat) {
        if (auto hb = wire::decodeHeartbeat(e.body)) {
            status_.peer_level   = hb->level_dbfs;
            status_.peer_battery = hb->battery;
            status_.peer_gate    = (hb->flags & 1) != 0;
        }
        return;
    }
    if (e.type != wire::Type::Audio || active_role_ != Role::Parent) return;
    auto a = wire::decodeAudio(e.body);
    if (!a) return;
    if (!dec_ || static_cast<uint8_t>(dec_->mode()) != a->mode) {
        if (a->mode != static_cast<uint8_t>(audio::Mode::M1600) && a->mode != static_cast<uint8_t>(audio::Mode::M700C))
            return;
        dec_ = std::make_unique<audio::Codec>(static_cast<audio::Mode>(a->mode));
    }
    const int bpf = dec_->bytesPerFrame();
    if (static_cast<int>(a->frames.size()) != bpf * a->nframes) return;
    std::vector<int16_t> pcm(static_cast<std::size_t>(dec_->samplesPerFrame()) * a->nframes);
    for (int f = 0; f < a->nframes; ++f)
        dec_->decode(a->frames.data() + f * bpf, pcm.data() + f * dec_->samplesPerFrame());
    jitter_.push(a->seq, std::move(pcm));
    last_audio_ms_    = now_ms;
    status_.peer_gate = true;
    ++status_.audio_packets;
}

void Controller::parentTick(uint64_t now_ms)
{
    // Feed the speaker one packet ahead; release a lone packet after a stall.
    const bool stalled = now_ms - last_audio_ms_ > kStreamStallMs;
    while (playback_.queued() < kPacketSamples) {
        std::vector<int16_t> pcm;
        bool gap = false;
        if (!jitter_.pop(&pcm, &gap, stalled)) break;
        if (gap) {
            ++status_.lost_packets;
            pcm.assign(kPacketSamples, 0);
        }
        playback_.write(pcm.data(), pcm.size());
        for (std::size_t i = 0; i + kBlock <= pcm.size(); i += kBlock)
            pending_wave_.push_back(audio::peak(pcm.data() + i, kBlock));
    }
    if (stalled && jitter_.size() == 0) status_.peer_gate = status_.peer_gate && now_ms - last_audio_ms_ < 3000;

    // Scroll the waveform at the audio rate (one column per 40 ms frame).
    // Columns are released in step with playback; when idle, scroll silence.
    if (next_wave_ms_ == 0) next_wave_ms_ = now_ms;
    int budget = 0;
    while (now_ms >= next_wave_ms_ && budget++ < 10) {
        next_wave_ms_ += kWaveColumnMs;
        if (!pending_wave_.empty()) {
            pushWave(pending_wave_.front());
            pending_wave_.erase(pending_wave_.begin());
        } else {
            pushWave(0.0f);
        }
    }
    if (now_ms > next_wave_ms_ + 1000) next_wave_ms_ = now_ms;  // UI was blocked; don't fast-forward
    if (pending_wave_.size() > 40) pending_wave_.erase(pending_wave_.begin(), pending_wave_.end() - 20);
}

}  // namespace cpzbm
