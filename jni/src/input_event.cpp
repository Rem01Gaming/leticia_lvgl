#include "input_event.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace Leticia {

namespace {

constexpr const char *kInputDir = "/dev/input";
constexpr const char *kPowerSupplyDir = "/sys/class/power_supply";

uint64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + (ts.tv_nsec / 1000000);
}

bool test_bit(unsigned int bit, const unsigned long *bitmask)
{
    constexpr unsigned int bits_per_long = sizeof(unsigned long) * 8;
    return (bitmask[bit / bits_per_long] >> (bit % bits_per_long)) & 1;
}

} // namespace

input_event_monitor::~input_event_monitor()
{
    close();
}

bool input_event_monitor::node_reports_watched_bits(int fd)
{
    unsigned long ev_bits[(EV_MAX / (sizeof(unsigned long) * 8)) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
        return false;

    if (test_bit(EV_KEY, ev_bits)) {
        unsigned long key_bits[(KEY_MAX / (sizeof(unsigned long) * 8)) + 1] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
            if (test_bit(KEY_POWER, key_bits) || test_bit(KEY_VOLUMEUP, key_bits) ||
                test_bit(KEY_VOLUMEDOWN, key_bits))
                return true;
        }
    }

    if (test_bit(EV_SW, ev_bits)) {
        unsigned long sw_bits[(SW_MAX / (sizeof(unsigned long) * 8)) + 1] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sw_bits)), sw_bits) >= 0) {
            if (test_bit(SW_HEADPHONE_INSERT, sw_bits))
                return true;
        }
    }

    return false;
}

bool input_event_monitor::find_usb_online_node(std::string &out_path)
{
    DIR *dir = opendir(kPowerSupplyDir);
    if (dir == nullptr)
        return false;

    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;

        char type_path[256];
        snprintf(type_path, sizeof(type_path), "%s/%s/type", kPowerSupplyDir, entry->d_name);

        FILE *f = fopen(type_path, "r");
        if (f == nullptr)
            continue;

        char type[32] = {0};
        bool is_usb = fgets(type, sizeof(type), f) != nullptr && strncmp(type, "USB", 3) == 0;
        fclose(f);

        if (!is_usb)
            continue;

        char online_path[256];
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

bool input_event_monitor::open()
{
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
                fprintf(stderr, "input_event_monitor: watching %s\n", path);
            } else {
                ::close(fd);
            }
        }
        closedir(dir);
    }

    if (find_usb_online_node(usb_online_path_))
        fprintf(stderr, "input_event_monitor: watching %s\n", usb_online_path_.c_str());

    return !key_sources_.empty() || !usb_online_path_.empty();
}

void input_event_monitor::close()
{
    for (auto &source : key_sources_) {
        if (source.fd >= 0)
            ::close(source.fd);
    }
    key_sources_.clear();

    usb_online_path_.clear();
    usb_connected_ = false;
    usb_state_known_ = false;
}

void input_event_monitor::set_callback(input_event_cb_t callback)
{
    callback_ = std::move(callback);
}

void input_event_monitor::dispatch(input_event_type type) const
{
    if (callback_)
        callback_(input_event_t{type, now_ms()});
}

void input_event_monitor::poll_key_source(int fd)
{
    struct input_event ev;
    ssize_t n;

    while ((n = read(fd, &ev, sizeof(ev))) == sizeof(ev)) {
        if (ev.type == EV_KEY && ev.value == 1) {
            switch (ev.code) {
                case KEY_POWER:
                    dispatch(input_event_type::power_button);
                    break;
                case KEY_VOLUMEUP:
                    dispatch(input_event_type::volume_up);
                    break;
                case KEY_VOLUMEDOWN:
                    dispatch(input_event_type::volume_down);
                    break;
                default:
                    break;
            }
        } else if (ev.type == EV_SW && ev.code == SW_HEADPHONE_INSERT) {
            dispatch(ev.value != 0 ? input_event_type::headphone_insert
                                    : input_event_type::headphone_remove);
        }
    }

    if (n < 0 && errno != EAGAIN)
        perror("input_event_monitor: read");
}

void input_event_monitor::poll_usb()
{
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

void input_event_monitor::poll()
{
    for (auto &source : key_sources_)
        poll_key_source(source.fd);

    poll_usb();
}

} // namespace Leticia
