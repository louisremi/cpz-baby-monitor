#include "net/wifi_radio.hpp"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <vector>

extern char** environ;

namespace cpzbm {
namespace {

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string findOnPath(const char* name)
{
    const char* env   = std::getenv("PATH");
    const std::string paths = env ? env : "/usr/local/bin:/usr/bin:/bin";
    std::size_t start = 0;
    while (start <= paths.size()) {
        const std::size_t end = paths.find(':', start);
        const std::string dir = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            const std::string candidate = dir + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

// Runs `args` and returns its exit status, -1 on spawn/timeout failure.
// Stdout and stderr go to /dev/null unless `out` is given.
int runCommand(const std::vector<std::string>& args, int timeout_ms, std::string* out)
{
    if (args.empty()) return -1;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int pipe_fds[2] = {-1, -1};
    int devnull     = -1;
    if (out) {
        if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
            posix_spawn_file_actions_destroy(&actions);
            return -1;
        }
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    } else {
        // dup2 uses this fd in the child, so it must stay open until spawn returns.
        devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            posix_spawn_file_actions_adddup2(&actions, devnull, STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);
        }
    }

    pid_t pid  = 0;
    const int spawn_rc = posix_spawnp(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (devnull >= 0) close(devnull);
    if (pipe_fds[1] >= 0) close(pipe_fds[1]);
    if (spawn_rc != 0) {
        if (pipe_fds[0] >= 0) close(pipe_fds[0]);
        return -1;
    }

    const int64_t deadline = nowMs() + timeout_ms;
    if (out && pipe_fds[0] >= 0) {
        char buf[256];
        while (true) {
            const int64_t remaining = deadline - nowMs();
            if (remaining <= 0) break;
            pollfd p{pipe_fds[0], POLLIN, 0};
            const int ready = poll(&p, 1, static_cast<int>(remaining));
            if (ready <= 0) break;
            const ssize_t n = read(pipe_fds[0], buf, sizeof buf);
            if (n > 0) {
                out->append(buf, static_cast<std::size_t>(n));
            } else if (n == 0) {
                break;
            } else if (errno != EINTR && errno != EAGAIN) {
                break;
            }
        }
        close(pipe_fds[0]);
    }

    int status = 0;
    while (nowMs() < deadline) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        usleep(10000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}

bool probeRadio(const std::string& nmcli, bool* enabled)
{
    std::string out;
    if (runCommand({nmcli, "-t", "-f", "WIFI", "radio"}, 5000, &out) != 0) return false;
    if (out.find("disabled") != std::string::npos) {
        *enabled = false;
        return true;
    }
    if (out.find("enabled") != std::string::npos) {
        *enabled = true;
        return true;
    }
    return false;
}

}  // namespace

WifiRadio::WifiRadio(bool initial_enabled, bool available, Runner runner, LogFn log, GuardFn guard, bool async)
    : runner_(std::move(runner)), log_(std::move(log)), guard_(std::move(guard)), async_(async)
{
    available_.store(available, std::memory_order_release);
    enabled_.store(initial_enabled, std::memory_order_release);
}

WifiRadio::~WifiRadio()
{
    if (worker_.joinable()) worker_.join();
    restore();
}

std::unique_ptr<WifiRadio> WifiRadio::system(LogFn log)
{
    auto warn = [&log](const char* message) {
        if (log) log(message);
    };
    const std::string nmcli = findOnPath("nmcli");
    if (nmcli.empty()) {
        warn("Wi-Fi: nmcli not found; the Wi-Fi key is disabled");
        return std::make_unique<WifiRadio>(true, false, Runner{}, std::move(log), GuardFn{}, true);
    }
    bool enabled = true;
    const bool available = probeRadio(nmcli, &enabled);
    if (!available) warn("Wi-Fi: nmcli probe failed; the Wi-Fi key is disabled");

    // The guard is a user service (outside our cgroup) that polls our PID and
    // restores the radio when we are gone - on a crash, or when the launcher
    // kills the whole process group.
    const std::string systemd_run = findOnPath("systemd-run");
    const std::string systemctl   = findOnPath("systemctl");
    GuardFn guard;
    if (!systemd_run.empty() && !systemctl.empty()) {
        guard = [systemd_run, systemctl, nmcli = nmcli](bool arm) {
            runCommand({systemctl, "--user", "stop", "cpzbm-wifi-guard.service"}, 5000, nullptr);
            if (!arm) return true;
            const std::string loop = "while kill -0 " + std::to_string(getpid()) +
                                     " 2>/dev/null; do sleep 2; done; exec " + nmcli + " radio wifi on";
            return runCommand({systemd_run, "--user", "--unit=cpzbm-wifi-guard", "--collect", "--quiet", "--",
                               "/bin/sh", "-c", loop},
                              8000, nullptr) == 0;
        };
    } else {
        warn("Wi-Fi: systemd-run not found; no restore guard if the app dies");
    }
    const LogFn logger = log;
    const Runner runner = [nmcli, logger](bool enable) {
        std::string output;
        const int rc = runCommand({nmcli, "radio", "wifi", enable ? "on" : "off"}, 5000, &output);
        if (rc != 0 && logger) {
            std::string detail = output;
            while (!detail.empty() && (detail.back() == '\n' || detail.back() == ' ')) detail.pop_back();
            logger("Wi-Fi: nmcli exited " + std::to_string(rc) + (detail.empty() ? "" : ": " + detail));
        }
        return rc == 0;
    };
    return std::make_unique<WifiRadio>(enabled, available, runner, std::move(log), guard, true);
}

bool WifiRadio::toggle() { return set(!enabled_.load(std::memory_order_acquire)); }

bool WifiRadio::set(bool enable)
{
    if (!available_.load(std::memory_order_acquire)) return false;
    if (busy_.load(std::memory_order_acquire)) return false;
    const bool previous = enabled_.load(std::memory_order_acquire);
    if (previous == enable) return true;
    enabled_.store(enable, std::memory_order_release);
    if (!async_) {
        run(enable, previous);
        return enabled_.load(std::memory_order_acquire) == enable;
    }
    if (worker_.joinable()) worker_.join();
    busy_.store(true, std::memory_order_release);
    worker_ = std::thread([this, enable, previous] { run(enable, previous); });
    return true;
}

bool WifiRadio::run(bool enable, bool previous)
{
    const bool ok = runner_ && runner_(enable);
    if (!ok) {
        enabled_.store(previous, std::memory_order_release);
        if (log_) log_(enable ? "Wi-Fi: could not turn the radio on" : "Wi-Fi: could not turn the radio off");
    } else {
        enabled_.store(enable, std::memory_order_release);
        if (guard_) {
            if (enable) {
                guard_(false);
                guard_armed_ = false;
            } else {
                guard_armed_ = guard_(true);
                if (!guard_armed_ && log_) log_("Wi-Fi: could not install the restore guard; a crash would leave the radio off");
            }
        }
    }
    busy_.store(false, std::memory_order_release);
    return ok;
}

void WifiRadio::restore()
{
    if (!available_.load(std::memory_order_acquire)) return;
    if (worker_.joinable()) worker_.join();
    if (enabled_.load(std::memory_order_acquire)) {
        if (guard_ && guard_armed_) {
            guard_(false);
            guard_armed_ = false;
        }
        return;
    }
    busy_.store(true, std::memory_order_release);
    run(true, false);
}


}  // namespace cpzbm
