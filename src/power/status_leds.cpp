#include "power/status_leds.hpp"

#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace cpzbm {
namespace {

constexpr const char* kCpuLed   = "/ACT";
constexpr const char* kPowerLed = "/default-on";

}  // namespace

StatusLeds::StatusLeds(std::string root)
{
    cpu_   = probe(root + kCpuLed);
    power_ = probe(root + kPowerLed);
}

StatusLeds::Led StatusLeds::probe(const std::string& dir)
{
    Led led;
    led.dir      = dir;
    led.max      = std::max(0, readInt(dir + "/max_brightness"));
    led.writable = led.max > 0 && access((dir + "/brightness").c_str(), W_OK) == 0;
    if (led.writable) {
        led.brightness = readInt(dir + "/brightness");
        led.trigger    = activeTrigger(readText(dir + "/trigger"));
    }
    return led;
}

int StatusLeds::readInt(const std::string& path)
{
    std::ifstream f(path);
    int value = -1;
    if (!(f >> value)) return -1;
    return value;
}

std::string StatusLeds::readText(const std::string& path)
{
    std::ifstream f(path);
    std::string text;
    std::getline(f, text);
    return text;
}

std::string StatusLeds::activeTrigger(const std::string& list)
{
    const std::size_t open = list.find('[');
    if (open == std::string::npos) return {};
    const std::size_t close = list.find(']', open);
    if (close == std::string::npos) return {};
    return list.substr(open + 1, close - open - 1);
}

bool StatusLeds::write(const std::string& path, const std::string& text)
{
    std::ofstream f(path, std::ios::trunc);
    if (f) {
        f << text << '\n';
        f.flush();
    }
    return static_cast<bool>(f);
}

void StatusLeds::setPowerPercent(int percent)
{
    power_percent_ = std::clamp(percent, 0, 100);
}

bool StatusLeds::apply()
{
    if (applied_) return true;
    if (!available()) return false;

    bool ok = true;
    if (cpu_.writable) {
        cpu_.brightness = readInt(cpu_.dir + "/brightness");
        cpu_.trigger    = activeTrigger(readText(cpu_.dir + "/trigger"));
        ok = write(cpu_.dir + "/trigger", "none") && ok;
        ok = write(cpu_.dir + "/brightness", "0") && ok;
    }
    if (power_.writable) {
        power_.brightness = readInt(power_.dir + "/brightness");
        power_.trigger    = activeTrigger(readText(power_.dir + "/trigger"));
        const int level   = power_.max * power_percent_ / 100;
        ok = write(power_.dir + "/trigger", "none") && ok;
        ok = write(power_.dir + "/brightness", std::to_string(level)) && ok;
    }
    applied_ = true;
    return ok;
}

void StatusLeds::restore()
{
    if (!applied_) return;
    // Brightness first, then the trigger: the trigger owns the brightness once
    // it is active again (default-on sets it back to full by itself).
    if (power_.writable) {
        if (power_.brightness >= 0) write(power_.dir + "/brightness", std::to_string(power_.brightness));
        if (!power_.trigger.empty()) write(power_.dir + "/trigger", power_.trigger);
    }
    if (cpu_.writable) {
        if (cpu_.brightness >= 0) write(cpu_.dir + "/brightness", std::to_string(cpu_.brightness));
        if (!cpu_.trigger.empty()) write(cpu_.dir + "/trigger", cpu_.trigger);
    }
    applied_ = false;
}

}  // namespace cpzbm
