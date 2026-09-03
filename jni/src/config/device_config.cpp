#include "device_config.hpp"
#include "config_resolve.hpp"
#include "util/updater_proto.hpp"

#include <cstdlib>
#include <sstream>

namespace Leticia {

namespace {

constexpr const char *kDeviceConfigName = "config/device.conf";
constexpr const char *kAlsaUcmName = "config/alsa.conf";

void parse_ini(const std::string &text, device_config_t &out) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? text.size() : eol + 1;

        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos)
            continue;
        size_t end = line.find_last_not_of(" \t\r");
        line = line.substr(start, end - start + 1);

        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        size_t kstart = key.find_first_not_of(" \t");
        size_t kend = key.find_last_not_of(" \t");
        if (kstart == std::string::npos)
            continue;
        key = key.substr(kstart, kend - kstart + 1);

        size_t vstart = val.find_first_not_of(" \t");
        size_t vend = val.find_last_not_of(" \t");
        val = (vstart == std::string::npos) ? std::string() : val.substr(vstart, vend - vstart + 1);

        if (key == "backlight_path") {
            out.backlight_path = val;
            continue;
        }

        if (key == "max_brightness") {
            out.max_brightness = atoi(val.c_str());
            continue;
        }

        if (key == "screen_blank_supported") {
            out.screen_blank_supported = (val == "1" || val == "true" || val == "yes");
            continue;
        }

        if (key == "alsa_card") {
            out.alsa_card = static_cast<unsigned int>(atoi(val.c_str()));
            continue;
        }

        if (key == "battery_path") {
            out.battery_path = val;
            continue;
        }

        if (key == "status_bar_height_dp") {
            out.status_bar_height_dp = atoi(val.c_str());
            continue;
        }

        if (key == "audio_thread_affinity") {
            std::istringstream iss(val);
            std::vector<int> cpus;
            int cpu_id;
            while (iss >> cpu_id) {
                cpus.push_back(cpu_id);
            }

            if (!iss.eof()) {
                Leticia::ui_print("device_config: invalid audio_thread_affinity '%s', expected space-separated CPU IDs", val.c_str());
            } else if (cpus.empty()) {
                Leticia::ui_print("device_config: audio_thread_affinity must have at least 1 CPU ID");
            } else {
                out.audio_thread_affinity = std::move(cpus);
            }

            continue;
        }

        if (key == "lvgl_thread_affinity") {
            std::istringstream iss(val);
            std::vector<int> cpus;
            int cpu_id;
            while (iss >> cpu_id) {
                cpus.push_back(cpu_id);
            }

            if (!iss.eof()) {
                Leticia::ui_print(
                        "device_config: invalid lvgl_thread_affinity '%s', expected space-separated CPU IDs",
                        val.c_str());
            } else if (cpus.empty()) {
                Leticia::ui_print(
                        "device_config: lvgl_thread_affinity must have at least 1 CPU ID");
            } else {
                out.lvgl_thread_affinity = std::move(cpus);
            }

            continue;
        }
    }
}

} // namespace

bool load_device_config(const std::string &zip_path, device_config_t &out) {
    std::string text;
    if (!resolve_config_text(zip_path, "LETICIA_DEVICE_CONFIG", ".leticia.conf", kDeviceConfigName, "device config", text))
        return false;

    parse_ini(text, out);
    return true;
}

bool load_alsa_ucm_config_text(const std::string &zip_path, std::string &out_text) {
    return resolve_config_text(zip_path, "LETICIA_ALSA_CONFIG", ".leticia.alsa.conf", kAlsaUcmName, "alsa ucm config", out_text);
}

} // namespace Leticia
