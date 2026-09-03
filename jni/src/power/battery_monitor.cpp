#include "power/battery_monitor.hpp"
#include "util/updater_proto.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace Leticia {

namespace {

constexpr const char *kPowerSupplyDir = "/sys/class/power_supply";

/**
 * @brief Reads an integer value from a sysfs file.
 *
 * @return The parsed value, or -1 if the file could not be read.
 */
int read_int_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "r");
    if (f == nullptr)
        return -1;

    int value = -1;
    if (fscanf(f, "%d", &value) != 1)
        value = -1;

    fclose(f);
    return value;
}

/**
 * @brief Reads and trims a short text sysfs file.
 */
std::string read_text_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "r");
    if (f == nullptr)
        return {};

    char buf[32] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        n--;
    buf[n] = '\0';

    return std::string(buf);
}

/**
 * @brief Checks whether a power_supply node's type file reads "Battery".
 */
bool node_is_battery(const std::string &node_path) {
    return read_text_file(node_path + "/type") == "Battery";
}

/**
 * @brief Maps a power_supply status string to a battery_status.
 */
battery_status parse_status(const std::string &text) {
    if (text == "Charging")
        return battery_status::charging;
    if (text == "Full")
        return battery_status::full;
    if (text == "Discharging" || text == "Not charging")
        return battery_status::discharging;
    return battery_status::unknown;
}

} // namespace

battery_monitor::~battery_monitor() {
    deinit();
}

bool battery_monitor::find_node(const device_config_t &config) {
    if (!config.battery_path.empty()) {
        if (access((config.battery_path + "/capacity").c_str(), R_OK) == 0) {
            node_path_ = config.battery_path;
            Leticia::ui_print("Using configured battery node: %s", node_path_.c_str());
            return true;
        }
        Leticia::ui_print("Configured battery_path unusable, falling back to auto-detect");
    }

    DIR *dir = opendir(kPowerSupplyDir);
    if (dir == nullptr) {
        Leticia::ui_print("battery_monitor: %s not available", kPowerSupplyDir);
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;

        std::string candidate = std::string(kPowerSupplyDir) + "/" + entry->d_name;
        if (node_is_battery(candidate)) {
            node_path_ = candidate;
            break;
        }
    }
    closedir(dir);

    if (node_path_.empty()) {
        Leticia::ui_print("battery_monitor: no power_supply node of type Battery found");
        return false;
    }

    Leticia::ui_print("Auto-detected battery node: %s", node_path_.c_str());
    return true;
}

bool battery_monitor::init(const device_config_t &config, uint32_t poll_interval_ms) {
    if (!find_node(config))
        return false;

    poll();
    poll_timer_ = lv_timer_create(poll_timer_trampoline, poll_interval_ms, this);
    return true;
}

void battery_monitor::deinit() {
    if (poll_timer_ != nullptr) {
        lv_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
    }
    node_path_.clear();
}

void battery_monitor::poll() {
    int new_percent = read_int_file(node_path_ + "/capacity");
    if (new_percent < 0)
        new_percent = percent_;
    if (new_percent > 100)
        new_percent = 100;

    battery_status new_status = parse_status(read_text_file(node_path_ + "/status"));

    bool changed = (new_percent != percent_) || (new_status != status_);
    percent_ = new_percent;
    status_ = new_status;

    if (changed && on_change_)
        on_change_();
}

void battery_monitor::poll_timer_trampoline(lv_timer_t *timer) {
    auto *self = static_cast<battery_monitor *>(lv_timer_get_user_data(timer));
    self->poll();
}

} // namespace Leticia
