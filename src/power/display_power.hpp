#pragma once

// Panel backlight control through the kernel backlight class. The CardputerZero
// exposes /sys/class/backlight/backlight (0..max_brightness, linear "raw" scale)
// and Debian's 60-backlight.rules makes `brightness` group-writable for video,
// so the app - which runs as the desktop user in group video - can switch the
// panel off without privileges. Brightness 0 really is off: the M5IOE1 PWM duty
// drops to zero.
//
// Hardware-free: this is only file IO, so the tests point it at a fake sysfs
// tree. A missing or unreadable node disables the whole feature (the app then
// keeps the screen on) instead of failing every frame.

#include <string>

namespace cpzbm {

class DisplayPower {
public:
    explicit DisplayPower(std::string dir = "/sys/class/backlight/backlight");

    const std::string& dir() const { return dir_; }
    // False when max_brightness is missing/unreadable/zero.
    bool available() const { return available_; }
    // 0 when unavailable.
    int max() const { return max_; }
    // Current level, or -1 when it cannot be read.
    int brightness() const;
    // Clamped to [0, max]. A failed write disables the feature (the caller
    // keeps rendering and logs it once).
    bool set(int value);

    // Remembers the last non-zero level seen, which is what we turn the panel
    // back on to. Before any non-zero level is seen, wakeLevel() is 75% of max.
    void captureWakeLevel();
    int wakeLevel() const;

    // Called on exit: if we wrote to the backlight and left the panel dark,
    // turn it back on so the launcher is visible. Never writes 0.
    bool restore();

private:
    bool write(int value);

    std::string dir_;
    int max_          = 0;
    int wake_level_   = 0;
    bool available_   = false;
    bool touched_     = false;
};

}  // namespace cpzbm
