#include "app/app.hpp"

#include <spdlog/spdlog.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "proto/crypto.hpp"
#include "store/store.hpp"

namespace cpzbm {
namespace {

constexpr int kBlock                 = 320;  // 40 ms at 8 kHz = one codec2 frame
constexpr uint64_t kHeartbeatMs      = 2000;
constexpr uint64_t kKeepaliveMs      = 4000;
constexpr uint64_t kWaveColumnMs     = 40;
constexpr uint64_t kStreamStallMs    = 450;   // play a lone packet if no follow-up arrives
constexpr uint64_t kSoundIndicatorMs = 3000;  // "sound" stays lit this long after the last audio
constexpr uint64_t kBatteryPollMs    = 30000;
constexpr int kMaxWaveCatchUp        = 10;    // columns per tick; beyond this the UI was blocked
constexpr std::size_t kPacketSamples = kBlock * kFramesPerPacket;
constexpr float kVoxMinDbfs = -70.0f, kVoxMaxDbfs = -10.0f;
constexpr const char* kBatteryPath = "/sys/class/power_supply/bq27220-0/capacity";

std::string hostName()
{
    char buf[64] = {};
    if (gethostname(buf, sizeof buf - 1) != 0 || !buf[0]) return "cardputer";
    return buf;
}

const char* roleName(Role r) { return r == Role::Baby ? "baby" : r == Role::Parent ? "parent" : "none"; }

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
        spdlog::info("Store: restored pairing with {:08x}, role {}", p->peer_id, roleName(p->role));
    }
    spdlog::info("Identity: {:08x} '{}'", identity_.dev_id,
                 std::string(identity_.name.data(), strnlen(identity_.name.data(), identity_.name.size())));
    status_.codec    = store::loadCodecMode(config_dir_, audio::Mode::M1600);
    status_.region   = store::loadRegion(config_dir_, Region::US915);
    region_          = RegionHandshake(status_.region);
    status_.radio_ok = radio_.start(profileFor(status_.region));
    if (!status_.radio_ok) status_.radio_error = radio_.error();
    return status_.radio_ok;
}

void Controller::stop()
{
    leaveRole();
    if (link_) link_->flush();
    radio_.stop();
}

// ---------------------------------------------------------------- region --

void Controller::retune(Region r)
{
    if (!store::saveRegion(config_dir_, r)) spdlog::error("Store: failed to save region");
    status_.region = r;
    radio_.stop();
    status_.radio_ok    = radio_.start(profileFor(r));
    status_.radio_error = status_.radio_ok ? "" : radio_.error();
    spdlog::info("Region: now {}", regionName(r));
}

void Controller::toggleRegion(uint64_t now_ms)
{
    const Region target = status_.region == Region::EU868 ? Region::US915 : Region::EU868;
    if (!link_->paired()) {
        if (region_.busy()) return;
        region_ = RegionHandshake(target);
        retune(target);
        return;
    }
    if (region_.request(target, now_ms)) applyRegionAction(region_.tick(now_ms, radio_.txBusy()));
}

void Controller::applyRegionAction(const RegionHandshake::Action& a)
{
    if (a.send_request) link_->sendSecure(wire::Type::Region, {static_cast<uint8_t>(*a.send_request)});
    if (a.send_ack) link_->sendSecure(wire::Type::RegionAck, {static_cast<uint8_t>(*a.send_ack)});
    if (a.retune) {
        if (region_.lastSwitchUnacked()) spdlog::warn("Region: no ack from peer, switching anyway");
        retune(*a.retune);
    }
    status_.region_switching = region_.busy();
}

// ------------------------------------------------------------------ misc --

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
        startEncoder();
        if (!capture_.start(kBlock)) status_.audio_error = capture_.error();
        next_heartbeat_ms_ = 0;
    } else if (role == Role::Parent) {
        dec_.reset();  // created from the mode byte of the first audio packet
        jitter_.reset();
        if (!playback_.start()) status_.audio_error = playback_.error();
        playback_.setMuted(status_.speaker_muted);
        std::fill(std::begin(status_.wave), std::end(status_.wave), 0.0f);
        status_.wave_head = 0;
        pending_wave_.clear();
    }
    spdlog::info("Role: entered {}", roleName(role));
}

// (Re)starts the encoder pipeline. A new stream id tells the parent's jitter
// buffer that sequence numbers restart (also used on a codec mode change, as
// packets of the two modes must not be mixed in one stream).
void Controller::startEncoder()
{
    enc_ = std::make_unique<audio::Codec>(status_.codec);
    packetizer_.emplace(kFramesPerPacket, enc_->bytesPerFrame());
    audio_stream_ = static_cast<uint8_t>(crypto::randomU32());
    audio_seq_    = 0;
}

void Controller::toggleCodecMode()
{
    status_.codec = audio::otherMode(status_.codec);
    if (!store::saveCodecMode(config_dir_, status_.codec)) spdlog::error("Store: failed to save codec mode");
    if (active_role_ == Role::Baby) startEncoder();
    spdlog::info("Codec: now {}", audio::modeName(status_.codec));
}

void Controller::leaveRole()
{
    capture_.stop();
    playback_.stop();
    enc_.reset();
    dec_.reset();
    packetizer_.reset();
    active_role_ = Role::None;
}

void Controller::toggleMute()
{
    status_.speaker_muted = !status_.speaker_muted;
    playback_.setMuted(status_.speaker_muted);
}

void Controller::adjustVox(float delta_db)
{
    const float t = std::clamp(vox_.threshold() + delta_db, kVoxMinDbfs, kVoxMaxDbfs);
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
    next_battery_ms_ = now_ms + kBatteryPollMs;
    std::ifstream f(kBatteryPath);
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

    const auto rs = radio_.stats();
    if (!radio_.running() && status_.radio_ok) {
        status_.radio_ok    = false;
        status_.radio_error = rs.error.empty() ? "radio stopped" : rs.error;
    }
    status_.tx_packets = rs.tx_ok;
    status_.rx_packets = rs.rx;
    status_.last_rx_ms = link_->lastRxMs();
    status_.rssi       = link_->lastRssi();
    status_.snr        = link_->lastSnr();

    applyRegionAction(region_.tick(now_ms, radio_.txBusy()));
    if (active_role_ == Role::Baby) babyTick(now_ms);
    if (active_role_ == Role::Parent) parentTick(now_ms);
}

// ------------------------------------------------------------------ baby --

void Controller::sendAudioPacket(std::vector<uint8_t> frames)
{
    wire::Audio a;
    a.stream  = audio_stream_;
    a.seq     = audio_seq_++;
    a.mode    = status_.codec;
    a.nframes = static_cast<uint8_t>(kFramesPerPacket);
    a.frames  = std::move(frames);
    link_->sendSecure(wire::Type::Audio, wire::encode(a));
    ++status_.audio_packets;
}

void Controller::babyTick(uint64_t now_ms)
{
    std::vector<int16_t> block;
    std::vector<uint8_t> frame;
    std::vector<std::vector<uint8_t>> ready;
    while (capture_.read(&block)) {
        const float db    = audio::rmsDbfs(block.data(), block.size());
        status_.mic_dbfs  = db;
        level_peak_       = std::max(level_peak_, db);
        status_.gate_open = vox_.update(db) || status_.always_stream;
        // VOX decides on the raw mic level; the AGC only shapes what is sent.
        agc_.process(block.data(), block.size());
        status_.agc_db = agc_.gainDb();
        frame.clear();
        enc_->encode(block.data(), &frame);
        ready.clear();
        packetizer_->push(frame.data(), status_.gate_open, &ready);
        if (link_->paired())
            for (auto& p : ready) sendAudioPacket(std::move(p));
    }

    // Heartbeats keep the link alive while quiet; while sound streams, the
    // audio packets themselves prove the baby is alive.
    if (link_->paired() && !status_.gate_open && now_ms >= next_heartbeat_ms_) {
        wire::Heartbeat hb;
        hb.level_dbfs    = static_cast<int8_t>(std::clamp(level_peak_, -127.0f, 0.0f));
        hb.battery       = status_.battery;
        hb.always_stream = status_.always_stream;
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
    switch (e.type) {
        case wire::Type::Region:
        case wire::Type::RegionAck: {
            if (e.body.size() != 1) return;
            const auto r = regionFromByte(e.body[0]);
            if (!r) return;
            if (e.type == wire::Type::Region) {
                if (*r != status_.region) spdlog::info("Region: peer requested {}", regionName(*r));
                applyRegionAction(region_.onRegion(*r, now_ms));
            } else {
                applyRegionAction(region_.onAck(*r));
            }
            return;
        }
        case wire::Type::Heartbeat:
            if (auto hb = wire::decodeHeartbeat(e.body)) {
                status_.peer_level   = hb->level_dbfs;
                status_.peer_battery = hb->battery;
            }
            return;
        case wire::Type::Audio:
            if (active_role_ == Role::Parent) onAudio(e.body, now_ms);
            return;
        default:
            return;
    }
}

void Controller::onAudio(const wire::Bytes& body, uint64_t now_ms)
{
    auto a = wire::decodeAudio(body);
    if (!a) return;
    if (!dec_ || dec_->mode() != a->mode) dec_ = std::make_unique<audio::Codec>(a->mode);
    const int bytes_per_frame   = dec_->bytesPerFrame();
    const int samples_per_frame = dec_->samplesPerFrame();
    if (static_cast<int>(a->frames.size()) != bytes_per_frame * a->nframes) return;
    std::vector<int16_t> pcm(static_cast<std::size_t>(samples_per_frame) * a->nframes);
    for (int f = 0; f < a->nframes; ++f)
        dec_->decode(a->frames.data() + f * bytes_per_frame, pcm.data() + f * samples_per_frame);
    jitter_.push(a->stream, a->seq, std::move(pcm));
    last_audio_ms_     = now_ms;
    status_.peer_sound = true;
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
    if (now_ms - last_audio_ms_ >= kSoundIndicatorMs) status_.peer_sound = false;

    // Scroll the waveform at the audio rate (one column per 40 ms frame).
    // Columns are released in step with playback; when idle, scroll silence.
    if (next_wave_ms_ == 0) next_wave_ms_ = now_ms;
    int caught_up = 0;
    while (now_ms >= next_wave_ms_ && caught_up++ < kMaxWaveCatchUp) {
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
