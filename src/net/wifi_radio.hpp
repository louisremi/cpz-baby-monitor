#pragma once

// Wi-Fi radio power switch. Measured on the CardputerZero, an idle associated
// radio (plus Tailscale) costs ~145 mW - the largest saving after the backlight.
// NetworkManager allows the toggle for the active local session (polkit action
// `enable-disable-wifi` is allow_active=yes), so the app needs no privileges.
// NM persists WirelessEnabled=false, so a crash must not leave the radio off: a
// detached user service ("guard") watches our PID and turns the radio back on
// when we disappear. It lives outside our cgroup, so it also survives the app
// being killed together with its scope.
//
// Host builds and tests inject the Runner instead of spawning nmcli.

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace cpzbm {

class WifiRadio {
public:
    // Returns true when the radio was actually switched.
    using Runner = std::function<bool(bool enable)>;
    // Diagnostics hook: the core library does not depend on spdlog.
    using LogFn = std::function<void(const std::string&)>;
    // Restore guard: armed while the radio is off, disarmed when it is on.
    // Returns false when the guard could not be installed.
    using GuardFn = std::function<bool(bool arm)>;

    // `async` runs toggle() on a worker thread so the UI thread never waits on
    // nmcli (~0.3-1 s). Tests pass false and observe the result immediately.
    WifiRadio(bool initial_enabled, bool available, Runner runner, LogFn log, GuardFn guard, bool async);
    ~WifiRadio();

    WifiRadio(const WifiRadio&)            = delete;
    WifiRadio& operator=(const WifiRadio&) = delete;

    // Real nmcli + restore guard. Probe failures disable the feature.
    static std::unique_ptr<WifiRadio> system(LogFn log = {});

    bool available() const { return available_.load(std::memory_order_acquire); }
    bool enabled() const { return enabled_.load(std::memory_order_acquire); }
    bool busy() const { return busy_.load(std::memory_order_acquire); }

    // Flips the radio. Returns the optimistic state; a failed command rolls it
    // back (visible on the next poll). Ignored while another toggle is running.
    bool toggle();
    bool set(bool enable);
    // Ensures the radio is on (exit path), synchronously, and clears the guard.
    void restore();
    bool guardArmed() const { return guard_armed_; }

private:
    bool run(bool enable, bool previous);

    Runner runner_;
    LogFn log_;
    GuardFn guard_;
    bool async_       = false;
    bool guard_armed_ = false;
    std::atomic<bool> available_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> busy_{false};
    std::thread worker_;
};

}  // namespace cpzbm
