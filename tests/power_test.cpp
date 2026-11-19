// Display power, the screen idle policy and the Wi-Fi switch. All three are
// hardware-free by design: the backlight is a fake sysfs tree, the policy takes
// synthetic timestamps and the Wi-Fi command is an injected runner.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "net/wifi_radio.hpp"
#include "power/display_power.hpp"
#include "power/screen_power.hpp"
#include "power/status_leds.hpp"
#include "test_support.hpp"

using namespace cpzbm;

namespace {

// A minimal /sys/class/backlight/<name> tree.
std::string makeFakeBacklight(int max, int level)
{
    char tmpl[] = "/tmp/cpzbm-power-XXXXXX";
    const char* created = mkdtemp(tmpl);
    CHECK(created != nullptr);
    const std::string dir = created;
    {
        std::ofstream f(dir + "/max_brightness");
        f << max << "\n";
    }
    {
        std::ofstream f(dir + "/brightness");
        f << level << "\n";
    }
    return dir;
}

void writeFile(const std::string& path, const std::string& text)
{
    std::ofstream f(path, std::ios::trunc);
    f << text;
    CHECK(static_cast<bool>(f));
}

std::string readFile(const std::string& path)
{
    std::ifstream f(path);
    std::string text;
    std::getline(f, text);
    return text;
}

int readBacklight(const std::string& dir)
{
    std::ifstream f(dir + "/brightness");
    int value = -1;
    f >> value;
    return value;
}

void displayPowerTests()
{
    // A lit panel: the current level becomes the level we wake up to.
    {
        const std::string dir = makeFakeBacklight(100, 40);
        DisplayPower power(dir);
        CHECK(power.available());
        CHECK(power.max() == 100);
        CHECK(power.brightness() == 40);
        CHECK(power.wakeLevel() == 40);
        CHECK(power.set(150));  // clamped
        CHECK(readBacklight(dir) == 100);
        CHECK(power.wakeLevel() == 100);
        CHECK(power.set(0));
        CHECK(readBacklight(dir) == 0);
        CHECK(power.restore());
        CHECK(readBacklight(dir) == 100);  // exit turns it back on
    }

    // A panel the launcher already darkened: 75% of max is the wake level, and
    // restore() does nothing until we have written to it ourselves.
    {
        const std::string dir = makeFakeBacklight(100, 0);
        DisplayPower power(dir);
        CHECK(power.wakeLevel() == 75);
        CHECK(power.restore());
        CHECK(readBacklight(dir) == 0);
        CHECK(power.set(0));
        CHECK(power.restore());
        CHECK(readBacklight(dir) == 75);
    }

    // No backlight node at all (SDL dev build, other hardware): disabled, and
    // every call is a harmless no-op.
    {
        DisplayPower power("/tmp/cpzbm-no-such-backlight");
        CHECK(!power.available());
        CHECK(power.max() == 0);
        CHECK(power.brightness() == -1);
        CHECK(power.wakeLevel() == 0);
        CHECK(!power.set(50));
        CHECK(power.restore());
    }

    // `brightness` that cannot be written (a directory stands in for a denied
    // file): the feature disables itself instead of retrying every frame.
    {
        const std::string dir = makeFakeBacklight(100, 50);
        std::filesystem::remove(dir + "/brightness");
        std::filesystem::create_directory(dir + "/brightness");
        DisplayPower power(dir);
        CHECK(power.available());
        CHECK(!power.set(0));
        CHECK(!power.available());
        CHECK(!power.set(10));
    }

    for (const auto& entry : std::filesystem::directory_iterator("/tmp")) {
        if (entry.path().filename().string().rfind("cpzbm-power-", 0) == 0) std::filesystem::remove_all(entry.path());
    }
}

// The two status LEDs: binary activity LED plus a PWM power LED.
void statusLedTests()
{
    const std::string root = makeFakeBacklight(1, 0) + "-leds";  // fresh temp name
    std::filesystem::create_directories(root + "/ACT");
    std::filesystem::create_directories(root + "/default-on");
    writeFile(root + "/ACT/max_brightness", "1\n");
    writeFile(root + "/ACT/brightness", "1\n");
    writeFile(root + "/ACT/trigger", "none default rc-feedback [actpwr] timer\n");
    writeFile(root + "/default-on/max_brightness", "255\n");
    writeFile(root + "/default-on/brightness", "255\n");
    writeFile(root + "/default-on/trigger", "none [default-on] timer\n");

    {
        StatusLeds leds(root);
        CHECK(leds.available());
        CHECK(leds.cpuMax() == 1 && leds.powerMax() == 255);
        CHECK(leds.powerPercent() == 10);
        CHECK(leds.cpuTrigger() == "actpwr" && leds.powerTrigger() == "default-on");
        CHECK(leds.apply());
        CHECK(leds.applied());
        CHECK(leds.cpuLevel() == 0);
        CHECK(leds.powerLevel() == 25);  // 10% of 255
        CHECK(readFile(root + "/ACT/trigger") == "none");
        CHECK(readFile(root + "/default-on/trigger") == "none");
        leds.restore();
        CHECK(!leds.applied());
        CHECK(leds.cpuLevel() == 1);
        CHECK(leds.powerLevel() == 255);
        CHECK(readFile(root + "/ACT/trigger") == "actpwr");
        CHECK(readFile(root + "/default-on/trigger") == "default-on");
    }

    // The power level is clamped; 0 turns the LED off, 100 is full.
    {
        StatusLeds leds(root);
        leds.setPowerPercent(0);
        CHECK(leds.apply());
        CHECK(leds.powerLevel() == 0);
        leds.restore();
        leds.setPowerPercent(140);
        CHECK(leds.powerPercent() == 100);
        CHECK(leds.apply());
        CHECK(leds.powerLevel() == 255);
        leds.restore();
        CHECK(leds.powerLevel() == 255);
    }

    // No LEDs visible to this user: nothing happens, nothing crashes.
    {
        StatusLeds none("/tmp/cpzbm-no-such-leds");
        CHECK(!none.available());
        CHECK(!none.apply());
        none.restore();
    }

    std::filesystem::remove_all(root);
}

void screenPolicyTests()
{
    ScreenPowerPolicy policy(30000);
    policy.wake(1000);
    CHECK(policy.state() == ScreenPowerState::On);
    CHECK(!policy.expired(30999));
    CHECK(policy.expired(31000));
    policy.noteActivity(20000);
    CHECK(!policy.expired(31000));
    CHECK(!policy.expired(49999));
    CHECK(policy.expired(50000));

    // Adopting an external darkening stops the timer and never expires again.
    policy.adoptOff();
    CHECK(policy.state() == ScreenPowerState::Off);
    CHECK(!policy.expired(999999));
    policy.wake(60000);
    CHECK(policy.state() == ScreenPowerState::On);
    CHECK(!policy.expired(89999));
    CHECK(policy.expired(90000));

    // 0 disables the timeout entirely.
    ScreenPowerPolicy never(0);
    never.wake(0);
    CHECK(!never.expired(10000000));
}

void wifiRadioTests()
{
    // Happy path: the optimistic state is what the runner was told.
    {
        int calls   = 0;
        bool wanted = false;
        WifiRadio radio(true, true, [&](bool enable) { ++calls; wanted = enable; return true; }, {}, {}, false);
        CHECK(radio.available());
        CHECK(radio.enabled());
        CHECK(!radio.busy());
        CHECK(radio.toggle());
        CHECK(!radio.enabled());
        CHECK(calls == 1 && !wanted);
        CHECK(radio.toggle());
        CHECK(radio.enabled());
        CHECK(calls == 2 && wanted);
    }

    // A failed command rolls the radio state back.
    {
        WifiRadio radio(true, true, [](bool) { return false; }, {}, {}, false);
        radio.toggle();
        CHECK(radio.enabled());
    }

    // No nmcli: the key does nothing and the state never changes.
    {
        WifiRadio radio(true, false, [](bool) { return true; }, {}, {}, false);
        CHECK(!radio.available());
        CHECK(!radio.toggle());
        CHECK(radio.enabled());
        CHECK(!radio.set(false));
    }

    // restore() brings the radio back on (the exit path).
    {
        int calls = 0;
        std::vector<bool> asked;
        WifiRadio radio(
            true, true,
            [&](bool enable) {
                ++calls;
                asked.push_back(enable);
                return true;
            },
            {}, {}, false);
        radio.toggle();
        CHECK(!radio.enabled());
        radio.restore();
        CHECK(radio.enabled());
        CHECK(calls == 2);
        CHECK(asked.size() == 2 && !asked[0] && asked[1]);
        radio.restore();  // already on: nothing to do
        CHECK(calls == 2);
    }

    // The restore guard is armed while the radio is off and cleared when it is
    // back on, so a crash cannot leave the device unreachable.
    {
        std::vector<bool> armed;
        WifiRadio radio(
            true, true,
            [](bool) { return true; }, {},
            [&](bool arm) {
                armed.push_back(arm);
                return true;
            },
            false);
        radio.toggle();  // off -> arm
        CHECK(radio.guardArmed());
        radio.toggle();  // on -> disarm
        CHECK(!radio.guardArmed());
        radio.toggle();  // off -> arm
        radio.restore();  // on -> disarm
        CHECK(!radio.guardArmed());
        CHECK(armed.size() == 4 && armed[0] && !armed[1] && armed[2] && !armed[3]);
    }

    // The real system runner is only created on a device; constructing it here
    // must at least not crash when nmcli is absent.
    if (std::getenv("CPZBM_TEST_SYSTEM_WIFI")) {
        auto system = WifiRadio::system([](const std::string&) {});
        CHECK(system != nullptr);
    }
}

}  // namespace

int main()
{
    displayPowerTests();
    statusLedTests();
    screenPolicyTests();
    wifiRadioTests();
    std::puts("power_test OK");
    return 0;
}
