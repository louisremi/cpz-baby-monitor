#pragma once

// Application controller: owns the radio, the link state machine and the
// audio pipeline, and exposes a snapshot for the UI. Runs on the UI thread
// via tick(); the radio and audio devices have their own threads behind
// queues, so tick() never blocks.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio_io.hpp"
#include "audio/codec.hpp"
#include "audio/dsp.hpp"
#include "proto/link.hpp"
#include "radio/radio.hpp"

namespace cpzbm {

inline constexpr int kWaveColumns = 300;  // 40 ms per column, 12 s on screen

struct AppStatus {
    bool radio_ok = false;
    std::string radio_error;
    std::string audio_error;
    // Baby side
    float mic_dbfs     = -100;
    float vox_dbfs     = -65;
    float agc_db       = 0;
    bool gate_open     = false;
    bool always_stream = false;
    // Parent side
    bool speaker_muted   = false;
    float wave[kWaveColumns] = {};  // ring of 0..1 peaks, newest at wave_head-1
    int wave_head        = 0;
    int8_t peer_level    = -127;    // last heartbeat level
    uint8_t peer_battery = 255;
    bool peer_gate       = false;
    // Common
    uint64_t now_ms       = 0;
    uint64_t last_rx_ms   = 0;
    float rssi = 0, snr = 0;
    uint64_t tx_packets = 0, rx_packets = 0, audio_packets = 0, lost_packets = 0;
    uint8_t battery = 255;
};

class Controller {
public:
    Controller();
    ~Controller();

    bool start();  // radio up, identity/pairing loaded
    void stop();
    void tick(uint64_t now_ms);

    Link& link() { return *link_; }
    const Identity& identity() const { return identity_; }
    const AppStatus& status() const { return status_; }

    // Activates the role pipeline: Baby = capture/encode/send, Parent = receive/play.
    void enterRole(Role role);
    void leaveRole();
    Role activeRole() const { return active_role_; }

    void toggleAlwaysStream() { status_.always_stream = !status_.always_stream; }
    void toggleMute();
    void adjustVox(float delta_db);

    // Link events are drained by the controller and re-queued for the UI.
    bool pollUiEvent(LinkEvent* e);

private:
    void babyTick(uint64_t now_ms);
    void parentTick(uint64_t now_ms);
    void onMessage(const LinkEvent& e, uint64_t now_ms);
    void pushWave(float v);
    void readBattery(uint64_t now_ms);

    Radio radio_;
    Identity identity_;
    std::string config_dir_;
    std::unique_ptr<Link> link_;
    std::vector<LinkEvent> ui_events_;
    AppStatus status_;
    Role active_role_ = Role::None;

    // Baby
    audio::Capture capture_;
    std::unique_ptr<audio::Codec> enc_;
    audio::VoxGate vox_{-65.0f, 75};
    audio::Agc agc_;  // 75 blocks = 3 s hangover
    std::vector<uint8_t> frames_;
    int nframes_ = 0;
    std::vector<uint8_t> preroll_;     // last full packet of frames while gate closed
    uint16_t audio_seq_ = 0;
    uint64_t next_heartbeat_ms_ = 0;
    float level_peak_ = -100;

    // Parent
    audio::Playback playback_;
    std::unique_ptr<audio::Codec> dec_;
    audio::JitterBuffer jitter_{2};
    uint64_t last_audio_ms_    = 0;
    uint64_t next_keepalive_ms_ = 0;
    uint64_t next_wave_ms_     = 0;
    std::vector<float> pending_wave_;

    uint64_t next_battery_ms_ = 0;
};

// Number of codec frames per AUDIO packet (8 x 40 ms = 320 ms).
inline constexpr int kFramesPerPacket = 8;

}  // namespace cpzbm
