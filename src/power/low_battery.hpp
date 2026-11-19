#pragma once

// Shut down cleanly before the cell is empty. The fuel gauge reports a
// percentage and a charging state; a low reading only counts once it has been
// confirmed by several polls, so one bad sample cannot switch the device off.
// Nothing happens while charging (or while the charger state is unknown).
//
// The app runs as the desktop user; the stock polkit rule
// 50-factory_test-power.rules lets group sudo power off, so the shutdown needs
// no privilege of its own.
//
// Pure logic, host-tested with synthetic readings.

namespace cpzbm {

class LowBattery {
public:
    // threshold_percent <= 0 disables the shutdown.
    explicit LowBattery(int threshold_percent, int confirmations = 2)
        : threshold_(threshold_percent), required_(confirmations < 1 ? 1 : confirmations)
    {
    }

    int threshold() const { return threshold_; }
    bool confirmed() const { return confirmed_; }

    // `percent` < 0 means the gauge could not be read. Returns true exactly
    // once, on the poll that confirms the shutdown.
    bool update(int percent, bool charging)
    {
        if (threshold_ <= 0 || confirmed_) return false;
        if (charging || percent < 0 || percent > threshold_) {
            hits_ = 0;
            return false;
        }
        if (++hits_ < required_) return false;
        confirmed_ = true;
        return true;
    }

private:
    int threshold_  = 0;
    int required_   = 2;
    int hits_       = 0;
    bool confirmed_ = false;
};

}  // namespace cpzbm
