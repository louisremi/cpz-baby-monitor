#include "app/debug_modes.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "audio/audio_io.hpp"
#include "audio/beep.hpp"
#include "audio/codec.hpp"
#include "audio/dsp.hpp"
#include "net/wifi_radio.hpp"
#include "power/display_power.hpp"
#include "radio/radio.hpp"

namespace cpzbm {
namespace {

uint64_t nowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

LinkProfile profileFromEnv()
{
    const char* p = std::getenv("CPZBM_PROFILE");
    if (p && std::strcmp(p, "eu") == 0) return kProfileEu;
    return kProfileSf9;
}

// Raw radio test packets use a version nibble (0xF) the app ignores.
constexpr uint8_t kPing = 0xF1, kPong = 0xF2;

int radioPing(int count, const volatile std::sig_atomic_t* stop)
{
    Radio radio;
    const auto prof = profileFromEnv();
    if (!radio.start(prof)) {
        std::fprintf(stderr, "radio: %s\n", radio.error().c_str());
        return 1;
    }
    std::printf("ping: %d packets, 87 bytes, ToA %u us\n", count, loraTimeOnAirUs(prof, 87));
    int got = 0;
    uint64_t sent_at[256] = {};
    for (int i = 0; i < count && !*stop; ++i) {
        std::vector<uint8_t> p(87, 0x55);
        p[0] = kPing;
        p[1] = static_cast<uint8_t>(i);
        sent_at[i & 255] = nowMs();
        radio.send(p);
        const uint64_t until = nowMs() + 1000;
        while (nowMs() < until && !*stop) {
            RxPacket rx;
            while (radio.receive(&rx)) {
                if (rx.bytes.size() >= 4 && rx.bytes[0] == kPong) {
                    ++got;
                    std::printf("pong %3d rtt %4llu ms  local rssi %.0f snr %.1f | remote rssi %d snr %.1f\n",
                                rx.bytes[1], static_cast<unsigned long long>(nowMs() - sent_at[rx.bytes[1]]),
                                rx.rssi, rx.snr, -static_cast<int>(rx.bytes[2]),
                                static_cast<int8_t>(rx.bytes[3]) / 4.0);
                    std::fflush(stdout);
                }
            }
            usleep(5000);
        }
    }
    const auto st = radio.stats();
    std::printf("done: %d/%d pongs, tx_ok %llu dropped %llu\n", got, count,
                static_cast<unsigned long long>(st.tx_ok), static_cast<unsigned long long>(st.tx_dropped));
    return got > 0 ? 0 : 2;
}

int radioListen(const volatile std::sig_atomic_t* stop)
{
    Radio radio;
    if (!radio.start(profileFromEnv())) {
        std::fprintf(stderr, "radio: %s\n", radio.error().c_str());
        return 1;
    }
    std::printf("listening...\n");
    std::fflush(stdout);
    while (!*stop) {
        RxPacket rx;
        while (radio.receive(&rx)) {
            std::printf("rx %3zu bytes type 0x%02x rssi %.0f snr %.1f\n", rx.bytes.size(),
                        rx.bytes.empty() ? 0 : rx.bytes[0], rx.rssi, rx.snr);
            std::fflush(stdout);
            if (rx.bytes.size() >= 2 && rx.bytes[0] == kPing) {
                const int rssi = static_cast<int>(-rx.rssi);
                std::vector<uint8_t> pong{kPong, rx.bytes[1], static_cast<uint8_t>(rssi < 0 ? 0 : rssi > 255 ? 255 : rssi),
                                          static_cast<uint8_t>(static_cast<int8_t>(rx.snr * 4))};
                radio.send(pong);
            }
        }
        usleep(5000);
    }
    return 0;
}

int micTest(int seconds, const volatile std::sig_atomic_t* stop)
{
    audio::Capture cap;
    if (!cap.start(320)) {
        std::fprintf(stderr, "capture: %s\n", cap.error().c_str());
        return 1;
    }
    const uint64_t end = nowMs() + static_cast<uint64_t>(seconds) * 1000;
    float peak_db = -100;
    int blocks    = 0;
    while (nowMs() < end && !*stop) {
        std::vector<int16_t> b;
        while (cap.read(&b)) {
            const float db = audio::rmsDbfs(b.data(), b.size());
            if (db > peak_db) peak_db = db;
            if (++blocks % 3 == 0) {
                const int bars = static_cast<int>((peak_db + 80) / 2);
                std::printf("%6.1f dBFS |%.*s\n", peak_db, bars < 0 ? 0 : bars,
                            "########################################");
                std::fflush(stdout);
                peak_db = -100;
            }
        }
        usleep(10000);
    }
    std::printf("blocks %d (expected ~%d)\n", blocks, seconds * 25);
    return blocks > 0 ? 0 : 2;
}

int codecLoop(int seconds, const volatile std::sig_atomic_t* stop)
{
    audio::Capture cap;
    audio::Playback play;
    audio::Codec codec(audio::Mode::M1600);
    if (!cap.start(320) || !play.start()) {
        std::fprintf(stderr, "audio: %s %s\n", cap.error().c_str(), play.error().c_str());
        return 1;
    }
    std::printf("codec loop %d s: speak into the mic (use headphones to avoid feedback)\n", seconds);
    const uint64_t end = nowMs() + static_cast<uint64_t>(seconds) * 1000;
    std::vector<uint8_t> bits;
    std::vector<int16_t> out(320);
    audio::Agc agc;
    while (nowMs() < end && !*stop) {
        std::vector<int16_t> b;
        while (cap.read(&b)) {
            agc.process(b.data(), b.size());
            bits.clear();
            codec.encode(b.data(), &bits);
            codec.decode(bits.data(), out.data());
            play.write(out.data(), out.size());
        }
        usleep(5000);
    }
    return 0;
}

int benchCodec()
{
    audio::Codec c(audio::Mode::M1600);
    std::vector<int16_t> pcm(320), out(320);
    for (int i = 0; i < 320; ++i) pcm[i] = static_cast<int16_t>((i * 37) % 2000 - 1000);
    std::vector<uint8_t> bits;
    const int n  = 250;  // 10 s of audio
    auto t0      = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        bits.clear();
        c.encode(pcm.data(), &bits);
    }
    auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) c.decode(bits.data(), out.data());
    auto t2      = std::chrono::steady_clock::now();
    const double enc = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double dec = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::printf("codec2 1600: encode %.2f ms/frame (%.1f%% core), decode %.2f ms/frame (%.1f%% core)\n", enc / n,
                enc / n / 40 * 100, dec / n, dec / n / 40 * 100);
    return 0;
}

// Backlight path end to end: read, off, back on, restore.
int screenTest()
{
    DisplayPower power;
    if (!power.available()) {
        std::fprintf(stderr, "screen: no backlight at %s\n", power.dir().c_str());
        return 1;
    }
    const int original = power.brightness();
    std::printf("screen: backlight %d of %d\n", original, power.max());
    if (!power.set(0)) {
        std::fprintf(stderr, "screen: cannot switch the backlight off (group video?)\n");
        return 2;
    }
    std::printf("screen: off\n");
    usleep(800000);
    const int wake = power.wakeLevel();
    power.set(wake);
    std::printf("screen: on %d\n", wake);
    usleep(800000);
    power.set(original > 0 ? original : wake);
    std::printf("screen: restored\n");
    return 0;
}

// Volume feedback: three tones a second apart, then the stream is released.
int beepTest()
{
    audio::Beep beep;
    for (const int direction : {1, -1, 0}) {
        if (!beep.play(direction, nowMs())) {
            std::fprintf(stderr, "beep: audio unavailable\n");
            return 1;
        }
        std::printf("beep: %s\n", direction > 0 ? "up" : direction < 0 ? "down" : "mute");
        const uint64_t until = nowMs() + 1000;
        while (nowMs() < until) {
            beep.tick(nowMs());
            usleep(20000);
        }
    }
    beep.stop();
    return 0;
}

// Wi-Fi switch end to end: off for a few seconds, then back on. Closing the
// app first is required; the restore path is the one the app uses on exit.
int wifiTest(int seconds)
{
    auto wifi = WifiRadio::system([](const std::string& m) { std::fprintf(stderr, "%s\n", m.c_str()); });
    if (!wifi->available()) {
        std::fprintf(stderr, "wifi: nmcli unavailable\n");
        return 1;
    }
    std::printf("wifi: %s -> off\n", wifi->enabled() ? "on" : "off");
    if (!wifi->set(false)) {
        std::fprintf(stderr, "wifi: could not switch the radio off\n");
        return 2;
    }
    for (int i = 0; i < seconds; ++i) sleep(1);
    std::printf("wifi: restoring\n");
    wifi->restore();
    std::printf("wifi: %s\n", wifi->enabled() ? "on" : "off");
    return wifi->enabled() ? 0 : 3;
}

int argInt(int argc, char** argv, int i, int def) { return i < argc ? std::atoi(argv[i]) : def; }

}  // namespace

int runDebugMode(int argc, char** argv, const volatile std::sig_atomic_t* stop)
{
    if (argc < 2) return -1;
    const std::string m = argv[1];
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (m == "--radio-ping") return radioPing(argInt(argc, argv, 2, 20), stop);
    if (m == "--radio-listen") return radioListen(stop);
    if (m == "--mic-test") return micTest(argInt(argc, argv, 2, 10), stop);
    if (m == "--codec-loop") return codecLoop(argInt(argc, argv, 2, 15), stop);
    if (m == "--bench-codec") return benchCodec();
    if (m == "--screen-test") return screenTest();
    if (m == "--beep-test") return beepTest();
    if (m == "--wifi-test") return wifiTest(argInt(argc, argv, 2, 10));
    if (m == "--version") {
        std::printf("cpz-baby-monitor %s\n", CPZBM_VERSION);
        return 0;
    }
    return -1;
}

}  // namespace cpzbm
