// cpz-baby-monitor: LoRa baby monitor for the M5Stack CardputerZero.

#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>

#include "app/app.hpp"
#include "app/debug_modes.hpp"
#include "hal/gps_lvgl_hal.hpp"
#include "input/gps_keypad.hpp"
#include "ui/fonts.hpp"
#include "ui/ui.hpp"

namespace {

volatile sig_atomic_t g_stop = 0;
// APPLaunch sends SIGINT on a long Home press and SIGKILL 3 s later. Give the
// radio/audio teardown 2 s, then exit hard so the launcher regains the screen.
constexpr unsigned kShutdownTimeoutSeconds = 2;

void onSignal(int)
{
    if (g_stop == 0) alarm(kShutdownTimeoutSeconds);
    g_stop = 1;
}

void onAlarm(int) { _exit(0); }

uint64_t nowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace

int main(int argc, char** argv)
{
    spdlog::set_pattern("%H:%M:%S.%e [%^%l%$] %v");
    spdlog::cfg::load_env_levels();
    spdlog::flush_on(spdlog::level::info);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGALRM, onAlarm);

    const int dbg = cpzbm::runDebugMode(argc, argv, &g_stop);
    if (dbg >= 0) return dbg;

    spdlog::info("cpz-baby-monitor {} starting", CPZBM_VERSION);
    lv_init();
    if (!cap_gps::initLvglHal(320, 170)) return 1;
    cpzbm::fonts::init();

    cpzbm::Controller ctl;
    ctl.start();  // radio failure is shown in the UI
    cpzbm::Ui ui(&ctl);

    cap_gps::GpsKeypad keypad;
    keypad.setKeyCallback([&ui](uint32_t key, const char*, bool pressed) { return ui.onKey(key, pressed, nowMs()); });
    if (!keypad.openDefault()) spdlog::error("no keyboard input device");

    ui.start();
    while (!g_stop && !ui.quitRequested()) {
        keypad.poll();
        const uint64_t now = nowMs();
        ctl.tick(now);
        ui.tick(now);
        if (ui.screenOn()) {
            lv_timer_handler();
            usleep(5000);
        } else {
            // The panel is dark and the UI is not drawing: the radio and audio
            // threads keep running, so a slower loop only adds a little latency.
            usleep(20000);
        }
    }
    if (!g_stop) alarm(kShutdownTimeoutSeconds);
    spdlog::info("shutting down");
    ui.shutdown();
    ctl.stop();
    keypad.close();
    cpzbm::fonts::deinit();
    cap_gps::shutdownLvglHal();
    alarm(0);
    return 0;
}
