#include "input/safety_exit.hpp"
#include "input/evdev_probe.hpp"
#include "util/updater_proto.hpp"

#include <cstdint>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace Leticia {

namespace {

constexpr const char *kInputDir = "/dev/input";
constexpr uint32_t kHoldThresholdMs = 3000;
constexpr uint32_t kForceExitGraceMs = 2000;
constexpr int kPollTimeoutMs = 100;

uint64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + (ts.tv_nsec / 1000000);
}

} // namespace

safety_exit_monitor::~safety_exit_monitor() {
    stop();
}

void safety_exit_monitor::open_key_sources() {
    DIR *dir = opendir(kInputDir);
    if (dir == nullptr)
        return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        char path[288];
        snprintf(path, sizeof(path), "%s/%s", kInputDir, entry->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        if (evdev_probe::has_key(fd, KEY_POWER)) {
            key_sources_.push_back({fd});
            Leticia::ui_print("safety_exit_monitor: watching %s", path);
        } else {
            close(fd);
        }
    }

    closedir(dir);
}

void safety_exit_monitor::close_key_sources() {
    for (auto &source : key_sources_) {
        if (source.fd >= 0)
            close(source.fd);
    }
    key_sources_.clear();
}

void safety_exit_monitor::start(std::function<void()> on_long_press) {
    if (running_.load())
        return;

    on_long_press_ = std::move(on_long_press);
    open_key_sources();

    if (key_sources_.empty()) {
        Leticia::ui_print("safety_exit_monitor: no power button node found, safety exit disabled");
        return;
    }

    running_.store(true);
    thread_ = std::thread(&safety_exit_monitor::run, this);
}

void safety_exit_monitor::stop() {
    running_.store(false);
    if (thread_.joinable())
        thread_.join();
    close_key_sources();
}

void safety_exit_monitor::run() {
    bool power_held = false;
    bool triggered = false;
    uint64_t press_start_ms = 0;
    uint64_t force_exit_deadline_ms = 0;

    std::vector<struct pollfd> poll_fds;
    for (auto &source : key_sources_)
        poll_fds.push_back({source.fd, POLLIN, 0});

    while (running_.load(std::memory_order_relaxed)) {
        poll(poll_fds.data(), poll_fds.size(), kPollTimeoutMs);

        for (auto &pfd : poll_fds) {
            struct input_event ev;
            ssize_t n;
            while ((n = read(pfd.fd, &ev, sizeof(ev))) == sizeof(ev)) {
                if (ev.type != EV_KEY || ev.code != KEY_POWER || ev.value == 2)
                    continue;

                power_held = ev.value != 0;
            }
        }

        uint64_t now = now_ms();

        if (!triggered) {
            if (power_held) {
                if (press_start_ms == 0) {
                    press_start_ms = now;
                } else if (now - press_start_ms >= kHoldThresholdMs) {
                    triggered = true;
                    force_exit_deadline_ms = now + kForceExitGraceMs;
                    Leticia::ui_print("Requesting Leticia exit via power button hold");
                    if (on_long_press_)
                        on_long_press_();
                }
            } else {
                press_start_ms = 0;
            }
        }

        if (triggered && now >= force_exit_deadline_ms) {
            Leticia::ui_print("UI did not exit in time, forcing termination");
            _exit(1);
        }
    }
}

} // namespace Leticia
