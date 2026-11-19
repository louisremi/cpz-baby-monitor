#include "power/display_power.hpp"

#include <algorithm>
#include <fstream>
#include <utility>

namespace cpzbm {
namespace {

int readInt(const std::string& path)
{
    std::ifstream f(path);
    int value = -1;
    if (!(f >> value)) return -1;
    return value;
}

}  // namespace

DisplayPower::DisplayPower(std::string dir) : dir_(std::move(dir))
{
    max_       = std::max(0, readInt(dir_ + "/max_brightness"));
    available_ = max_ > 0;
    if (available_) captureWakeLevel();
}

int DisplayPower::brightness() const
{
    if (!available_) return -1;
    return readInt(dir_ + "/brightness");
}

bool DisplayPower::write(int value)
{
    std::ofstream f(dir_ + "/brightness", std::ios::trunc);
    if (f) {
        f << value << '\n';
        f.flush();
    }
    return static_cast<bool>(f);
}

bool DisplayPower::set(int value)
{
    if (!available_) return false;
    const int clamped = std::clamp(value, 0, max_);
    if (!write(clamped)) {
        available_ = false;  // permissions changed: stop trying every frame
        return false;
    }
    touched_ = true;
    if (clamped > 0) wake_level_ = clamped;
    return true;
}

void DisplayPower::captureWakeLevel()
{
    const int level = brightness();
    if (level > 0) wake_level_ = level;
}

int DisplayPower::wakeLevel() const
{
    return wake_level_ > 0 ? wake_level_ : (max_ * 3) / 4;
}

bool DisplayPower::restore()
{
    if (!available_ || !touched_) return true;
    if (brightness() != 0) return true;
    return write(wakeLevel());
}

}  // namespace cpzbm
