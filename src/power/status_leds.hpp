#pragma once

// Status LEDs at night. The CardputerZero has two indicator LEDs that are far
// too bright in a dark room:
//   ACT        - the activity LED (STATUS_LED_N, GPIO 29): plain on/off, no PWM.
//   default-on - the power LED on an M5IOE1 PWM channel: 0..255.
// While the app runs, the activity LED is switched off and the power LED is
// left faint (10% by default), then both are put back the way they were found.
//
// Both attributes are root-owned. The package ships a udev rule making them
// group-writable for `gpio`, which the desktop user belongs to.
//
// This is only file IO, so the tests point it at a fake sysfs tree.

#include <string>

namespace cpzbm {

class StatusLeds {
public:
    explicit StatusLeds(std::string root = "/sys/class/leds");

    // True when at least one of the two can be written by this user.
    bool available() const { return cpu_.writable || power_.writable; }
    bool applied() const { return applied_; }

    // Brightness left on the power LED while running, as a percentage of its
    // maximum (0 turns it off).
    int powerPercent() const { return power_percent_; }
    void setPowerPercent(int percent);

    // Saves the current trigger and brightness, then darkens both LEDs.
    bool apply();
    // Puts the saved trigger and brightness back (no-op if never applied).
    void restore();

    // Exposed for diagnostics and tests.
    int cpuLevel() const { return readInt(cpu_.dir + "/brightness"); }
    int powerLevel() const { return readInt(power_.dir + "/brightness"); }
    int cpuMax() const { return cpu_.max; }
    int powerMax() const { return power_.max; }
    const std::string& cpuTrigger() const { return cpu_.trigger; }
    const std::string& powerTrigger() const { return power_.trigger; }

private:
    struct Led {
        std::string dir;
        int max          = 0;
        bool writable    = false;
        std::string trigger;  // active trigger when we found it
        int brightness   = -1;
    };

    static Led probe(const std::string& dir);
    static int readInt(const std::string& path);
    static std::string readText(const std::string& path);
    // "none default [actpwr] timer" -> "actpwr"
    static std::string activeTrigger(const std::string& list);
    static bool write(const std::string& path, const std::string& text);

    Led cpu_;    // ACT
    Led power_;  // default-on
    int power_percent_ = 10;
    bool applied_      = false;
};

}  // namespace cpzbm
