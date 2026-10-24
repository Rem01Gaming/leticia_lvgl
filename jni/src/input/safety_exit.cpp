#include "input/safety_exit.hpp"
#include "input/evdev_probe.hpp"
#include "util/parent_mute.hpp"
#include "util/updater_proto.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>

namespace Leticia {

namespace {

constexpr const char *kInputDir = "/dev/input";
constexpr uint32_t kHoldThresholdMs = 3000;
constexpr uint32_t kForceExitGraceMs = 2000;
constexpr int kPollTimeoutMs = 100;
constexpr int kRescanIntervalMs = 1000;

/**
 * Reads the monotonic clock.
 *
 * @return Time in milliseconds.
 */
uint64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

/**
 * Checks whether a node can report a key the watchdog uses.
 *
 * @param fd Open descriptor of an input node.
 * @return true if the node reports the power key or both volume keys.
 */
bool node_has_exit_keys(int fd) {
    if (evdev_probe::has_key(fd, KEY_POWER))
        return true;
    return evdev_probe::has_key(fd, KEY_VOLUMEUP) && evdev_probe::has_key(fd, KEY_VOLUMEDOWN);
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

        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        if (node_has_exit_keys(fd)) {
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

void safety_exit_monitor::start(std::function<void()> on_long_press, std::function<void()> on_force_exit) {
    if (running_.load())
        return;

    on_long_press_ = std::move(on_long_press);
    on_force_exit_ = std::move(on_force_exit);
    open_key_sources();

    running_.store(true);
    thread_ = std::thread(&safety_exit_monitor::run, this);
}

void safety_exit_monitor::begin_shutdown(uint32_t timeout_ms) {
    deadline_ms_.store(now_ms() + timeout_ms, std::memory_order_release);
}

void safety_exit_monitor::stop() {
    running_.store(false);
    if (thread_.joinable())
        thread_.join();
    close_key_sources();
}

bool safety_exit_monitor::drain_source(size_t index) {
    struct input_event ev;
    ssize_t n;
    while ((n = read(key_sources_[index].fd, &ev, sizeof(ev))) == static_cast<ssize_t>(sizeof(ev))) {
        if (ev.type != EV_KEY)
            continue;

        const bool held = ev.value != 0;
        switch (ev.code) {
            case KEY_POWER:
                power_held_ = held;
                break;
            case KEY_VOLUMEUP:
                volume_up_held_ = held;
                break;
            case KEY_VOLUMEDOWN:
                volume_down_held_ = held;
                break;
            default:
                break;
        }
    }

    if (n < 0 && errno != EAGAIN && errno != EINTR) {
        close(key_sources_[index].fd);
        key_sources_[index].fd = -1;
        return false;
    }
    return true;
}

void safety_exit_monitor::run() {
    bool triggered = false;
    uint64_t press_start_ms = 0;
    uint64_t last_rescan_ms = now_ms();

    while (running_.load(std::memory_order_relaxed)) {
        if (key_sources_.empty()) {
            usleep(kPollTimeoutMs * 1000);
            const uint64_t now = now_ms();
            if (now - last_rescan_ms >= kRescanIntervalMs) {
                last_rescan_ms = now;
                open_key_sources();
            }
        } else {
            std::vector<struct pollfd> poll_fds;
            poll_fds.reserve(key_sources_.size());
            for (const auto &source : key_sources_)
                poll_fds.push_back({source.fd, POLLIN, 0});

            poll(poll_fds.data(), poll_fds.size(), kPollTimeoutMs);

            for (size_t i = key_sources_.size(); i-- > 0;) {
                if (!drain_source(i))
                    key_sources_.erase(key_sources_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }

        const uint64_t now = now_ms();
        const bool hold = power_held_ || (volume_up_held_ && volume_down_held_);

        if (!triggered) {
            if (!hold) {
                press_start_ms = 0;
            } else if (press_start_ms == 0) {
                press_start_ms = now;
            } else if (now - press_start_ms >= kHoldThresholdMs) {
                triggered = true;
                uint64_t expected = 0;
                deadline_ms_.compare_exchange_strong(expected, now + kForceExitGraceMs, std::memory_order_acq_rel);
                Leticia::ui_print("Exit requested by button hold");
                if (on_long_press_)
                    on_long_press_();
            }
        }

        const uint64_t deadline = deadline_ms_.load(std::memory_order_acquire);
        if (deadline != 0 && now >= deadline) {
            Leticia::ui_print("UI did not exit in time, forcing termination");
            if (on_force_exit_)
                on_force_exit_();
            parent_mute::emergency_resume();
            _exit(1);
        }
    }
}

} // namespace Leticia
