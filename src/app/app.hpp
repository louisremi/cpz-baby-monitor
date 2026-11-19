#pragma once

// Application controller: owns the radio, the link state machine and the
// audio pipeline, and exposes a snapshot for the UI. Runs on the UI thread
// via tick(); the radio and audio devices have their own threads behind
// queues, so tick() never blocks.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio/audio_io.hpp"
#include "audio/beep.hpp"
#include "audio/codec.hpp"
#include "audio/dsp.hpp"
#include "net/wifi_radio.hpp"
#include "proto/link.hpp"
#include "proto/region_handshake.hpp"
#include "radio/radio.hpp"

namespace cpzbm {

inline constexpr int kWaveColumns = 300;  // 40 ms per column, 12 s on screen
// Number of codec frames per AUDIO packet (8 x 40 ms = 320 ms).
inline constexpr int kFramesPerPacket = 8;

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
    audio::Mode codec  = audio::Mode::M1600;  // baby-side encoder mode
    // Parent side
    bool speaker_muted       = false;
    float wave[kWaveColumns] = {};  // ring of 0..1 peaks, newest at wave_head-1
    int wave_head            = 0;
    int8_t peer_level        = -127;  // last heartbeat level
    uint8_t peer_battery     = wire::kBatteryUnknown;
    bool peer_sound          = false;  // audio received recently
    // Common
    uint64_t now_ms     = 0;
    uint64_t last_rx_ms = 0;
    float rssi = 0, snr = 0;
    uint64_t tx_packets = 0, rx_packets = 0, audio_packets = 0, lost_packets = 0;
    uint8_t battery       = wire::kBatteryUnknown;
    Region region         = Region::US915;
    bool region_switching = false;
    // Wi-Fi radio (power saving): available when nmcli works, enabled while on.
    bool wifi_available = false;
    bool wifi_enabled   = true;
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

    void toggleAlwaysStream() { status_.always_stream = !status_.always_stream; }
    // Baby: switch Codec2 1600 <-> 700C (range fallback). Saved per device;
    // the parent follows the mode byte carried in every audio packet.
    void toggleCodecMode();
    void toggleMute();
    void adjustVox(float delta_db);

    // Power saving and system feedback.
    void toggleWifi();                              // key 9: radio off/on
    void volumeKey(int direction, uint64_t now_ms); // +1 up, -1 down, 0 mute

    // Switches to the other radio band. When paired, the peer is told first
    // (on the current band) and both devices retune; unpaired, only this one.
    void toggleRegion(uint64_t now_ms);

    // Link events are drained by the controller and re-queued for the UI.
    bool pollUiEvent(LinkEvent* e);

private:
    void babyTick(uint64_t now_ms);
    void parentTick(uint64_t now_ms);
    void onMessage(const LinkEvent& e, uint64_t now_ms);
    void onAudio(const wire::Bytes& body, uint64_t now_ms);
    void pushWave(float v);
    void readBattery(uint64_t now_ms);
    void applyRegionAction(const RegionHandshake::Action& a);
    void retune(Region r);
    void sendAudioPacket(std::vector<uint8_t> frames);
    void startEncoder();

    Radio radio_;
    Identity identity_;
    std::string config_dir_;
    std::unique_ptr<Link> link_;
    std::vector<LinkEvent> ui_events_;
    AppStatus status_;
    Role active_role_ = Role::None;
    RegionHandshake region_{Region::US915};

    // Baby
    audio::Capture capture_;
    std::unique_ptr<audio::Codec> enc_;
    audio::VoxGate vox_{-65.0f, 75};  // 75 blocks = 3 s hangover
    audio::Agc agc_;
    std::optional<audio::Packetizer> packetizer_;
    uint8_t audio_stream_ = 0;  // random per pipeline start (see wire::Audio)
    uint16_t audio_seq_   = 0;
    uint64_t next_heartbeat_ms_ = 0;
    float level_peak_           = -100;

    // Parent
    audio::Playback playback_;
    std::unique_ptr<audio::Codec> dec_;
    audio::JitterBuffer jitter_{2};
    uint64_t last_audio_ms_     = 0;
    uint64_t next_keepalive_ms_ = 0;
    uint64_t next_wave_ms_      = 0;
    std::vector<float> pending_wave_;

    uint64_t next_battery_ms_ = 0;

    // Power saving: Wi-Fi switch and the volume-key confirmation tone.
    std::unique_ptr<WifiRadio> wifi_;
    audio::Beep beep_;
};

}  // namespace cpzbm
