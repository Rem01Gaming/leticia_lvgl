#include "input_event.hpp"
#include "evdev_probe.hpp"
#include "util/updater_proto.hpp"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace Leticia {

namespace {

constexpr const char *kInputDir = "/dev/input";
constexpr const char *kPowerSupplyDir = "/sys/class/power_supply";

uint64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + (ts.tv_nsec / 1000000);
}

/**
 * @brief Reads the current state of an EV_SW switch code.
 */
bool try_read_switch_state(int fd, unsigned int switch_code, bool *active) {
    if (!evdev_probe::has_switch(fd, switch_code))
        return false;

    unsigned long sw_state_bits[(SW_MAX / (sizeof(unsigned long) * 8)) + 1] = {0};
    if (ioctl(fd, EVIOCGSW(sizeof(sw_state_bits)), sw_state_bits) < 0)
        return false;

    *active = evdev_probe::has_bit(sw_state_bits, switch_code);
    return true;
}

} // namespace

input_event_monitor::~input_event_monitor() {
    close();
}

bool input_event_monitor::node_reports_watched_bits(int fd) {
    return evdev_probe::has_key(fd, KEY_POWER) || evdev_probe::has_key(fd, KEY_VOLUMEUP) ||
           evdev_probe::has_key(fd, KEY_VOLUMEDOWN) || evdev_probe::has_key(fd, KEY_MEDIA) ||
           evdev_probe::has_switch(fd, SW_HEADPHONE_INSERT) || evdev_probe::has_switch(fd, SW_JACK_PHYSICAL_INSERT);
}

bool input_event_monitor::find_usb_online_node(std::string &out_path) {
    DIR *dir = opendir(kPowerSupplyDir);
    if (dir == nullptr)
        return false;

    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;

        char type_path[PATH_MAX];
        snprintf(type_path, sizeof(type_path), "%s/%s/type", kPowerSupplyDir, entry->d_name);

        FILE *f = fopen(type_path, "r");
        if (f == nullptr)
            continue;

        char type[32] = {0};
        bool is_usb = fgets(type, sizeof(type), f) != nullptr && strncmp(type, "USB", 3) == 0;
        fclose(f);

        if (!is_usb)
            continue;

        char online_path[PATH_MAX];
        snprintf(online_path, sizeof(online_path), "%s/%s/online", kPowerSupplyDir, entry->d_name);
        if (access(online_path, R_OK) != 0)
            continue;

        out_path = online_path;
        found = true;
        break;
    }

    closedir(dir);
    return found;
}

bool input_event_monitor::open() {
    close();

    DIR *dir = opendir(kInputDir);
    if (dir != nullptr) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strncmp(entry->d_name, "event", 5) != 0)
                continue;

            char path[288];
            snprintf(path, sizeof(path), "%s/%s", kInputDir, entry->d_name);

            int fd = ::open(path, O_RDONLY | O_NONBLOCK);
            if (fd < 0)
                continue;

            if (node_reports_watched_bits(fd)) {
                key_sources_.push_back({fd, path});
                Leticia::ui_print("input_event_monitor: watching %s", path);

                if (!headphone_state_known_) {
                    bool connected = false;
                    if (try_read_switch_state(fd, SW_HEADPHONE_INSERT, &connected)) {
                        headphone_connected_ = connected;
                        headphone_state_known_ = true;
                        Leticia::ui_print("input_event_monitor: initial headphone state: %s",
                                          connected ? "connected" : "not connected");
                    }
                }

                if (!jack_state_known_) {
                    bool inserted = false;
                    if (try_read_switch_state(fd, SW_JACK_PHYSICAL_INSERT, &inserted)) {
                        jack_inserted_ = inserted;
                        jack_state_known_ = true;
                        Leticia::ui_print("input_event_monitor: initial jack state: %s",
                                          inserted ? "inserted" : "not inserted");
                    }
                }
            } else {
                ::close(fd);
            }
        }
        closedir(dir);
    }

    if (find_usb_online_node(usb_online_path_))
        Leticia::ui_print("input_event_monitor: watching %s", usb_online_path_.c_str());

    return !key_sources_.empty() || !usb_online_path_.empty();
}

void input_event_monitor::close() {
    for (auto &source : key_sources_) {
        if (source.fd >= 0)
            ::close(source.fd);
    }
    key_sources_.clear();

    usb_online_path_.clear();
    usb_connected_ = false;
    usb_state_known_ = false;
    headphone_connected_ = false;
    headphone_state_known_ = false;
    jack_inserted_ = false;
    jack_state_known_ = false;
}

void input_event_monitor::set_callback(input_event_cb_t callback) {
    callback_ = std::move(callback);
}

void input_event_monitor::dispatch(input_event_type type) const {
    if (callback_)
        callback_(input_event_t{type, now_ms()});
}

void input_event_monitor::poll_key_source(int fd) {
    struct input_event ev;
    ssize_t n;

    while ((n = read(fd, &ev, sizeof(ev))) == sizeof(ev)) {
        if (ev.type == EV_KEY) {
            switch (ev.code) {
                case KEY_POWER:
                    if (ev.value == 1)
                        dispatch(input_event_type::power_button_press);
                    else if (ev.value == 0)
                        dispatch(input_event_type::power_button_release);
                    break;
                case KEY_VOLUMEUP:
                    if (ev.value == 1)
                        dispatch(input_event_type::volume_up_press);
                    else if (ev.value == 0)
                        dispatch(input_event_type::volume_up_release);
                    break;
                case KEY_VOLUMEDOWN:
                    if (ev.value == 1)
                        dispatch(input_event_type::volume_down_press);
                    else if (ev.value == 0)
                        dispatch(input_event_type::volume_down_release);
                    break;
                case KEY_MEDIA:
                    if (ev.value == 1)
                        dispatch(input_event_type::media_play_pause_press);
                    else if (ev.value == 0)
                        dispatch(input_event_type::media_play_pause_release);
                    break;
                default:
                    break;
            }
        } else if (ev.type == EV_SW && ev.code == SW_HEADPHONE_INSERT) {
            headphone_connected_ = ev.value != 0;
            headphone_state_known_ = true;
            dispatch(headphone_connected_ ? input_event_type::headphone_insert : input_event_type::headphone_remove);
        } else if (ev.type == EV_SW && ev.code == SW_JACK_PHYSICAL_INSERT) {
            jack_inserted_ = ev.value != 0;
            jack_state_known_ = true;
            dispatch(jack_inserted_ ? input_event_type::jack_insert : input_event_type::jack_remove);
        }
    }

    if (n < 0 && errno != EAGAIN) {
        Leticia::ui_print("input_event_monitor: read failed: %s", strerror(errno));
    }
}

void input_event_monitor::poll_usb() {
    if (usb_online_path_.empty())
        return;

    FILE *f = fopen(usb_online_path_.c_str(), "r");
    if (f == nullptr)
        return;

    int online = 0;
    bool ok = fscanf(f, "%d", &online) == 1;
    fclose(f);
    if (!ok)
        return;

    bool connected = online != 0;
    if (!usb_state_known_ || connected != usb_connected_) {
        usb_connected_ = connected;
        usb_state_known_ = true;
        dispatch(connected ? input_event_type::usb_connected : input_event_type::usb_disconnected);
    }
}

void input_event_monitor::poll() {
    for (auto &source : key_sources_)
        poll_key_source(source.fd);

    poll_usb();
}

} // namespace Leticia
