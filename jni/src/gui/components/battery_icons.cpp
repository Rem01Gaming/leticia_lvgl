#include "gui/components/battery_icons.hpp"

#include "config/config_resolve.hpp"
#include "util/updater_proto.hpp"

#include <array>

namespace Leticia::gui::battery_icons {

namespace {

/* Index 0..6 mirror the battery_android_0..6.svg fill steps; 7 and 8 are
 * the two named icons (full, alert) rather than a fill step. */
constexpr int kFillStepCount = 7;

constexpr std::array<const char *, kFillStepCount> kFillZipEntries = {
    "svg/battery_android_0.svg", "svg/battery_android_1.svg", "svg/battery_android_2.svg",
    "svg/battery_android_3.svg", "svg/battery_android_4.svg", "svg/battery_android_5.svg",
    "svg/battery_android_6.svg",
};
constexpr std::array<const char *, kFillStepCount> kFillEnvVars = {
    "LETICIA_ICON_BATTERY_0", "LETICIA_ICON_BATTERY_1", "LETICIA_ICON_BATTERY_2",
    "LETICIA_ICON_BATTERY_3", "LETICIA_ICON_BATTERY_4", "LETICIA_ICON_BATTERY_5",
    "LETICIA_ICON_BATTERY_6",
};
constexpr const char *kFullZipEntry = "svg/battery_android_full.svg";
constexpr const char *kFullEnvVar = "LETICIA_ICON_BATTERY_FULL";
constexpr const char *kAlertZipEntry = "svg/battery_android_alert.svg";
constexpr const char *kAlertEnvVar = "LETICIA_ICON_BATTERY_ALERT";
constexpr const char *kBoltZipEntry = "svg/bolt.svg";
constexpr const char *kBoltEnvVar = "LETICIA_ICON_BOLT";

std::array<std::string, kFillStepCount> g_fill_paths;
std::string g_full_path;
std::string g_alert_path;
std::string g_bolt_path;
bool g_initialized = false;

} // namespace

bool init(const std::string &zip_path) {
    for (int i = 0; i < kFillStepCount; ++i) {
        if (!resolve_config_file_path(zip_path, kFillEnvVars[i], kFillZipEntries[i], "battery icon",
                                      g_fill_paths[i])) {
            Leticia::ui_print("battery_icons: failed to resolve %s", kFillZipEntries[i]);
            return false;
        }
    }

    if (!resolve_config_file_path(zip_path, kFullEnvVar, kFullZipEntry, "battery icon (full)", g_full_path)) {
        Leticia::ui_print("battery_icons: failed to resolve %s", kFullZipEntry);
        return false;
    }

    if (!resolve_config_file_path(zip_path, kAlertEnvVar, kAlertZipEntry, "battery icon (alert)", g_alert_path)) {
        Leticia::ui_print("battery_icons: failed to resolve %s", kAlertZipEntry);
        return false;
    }

    if (!resolve_config_file_path(zip_path, kBoltEnvVar, kBoltZipEntry, "charging bolt icon", g_bolt_path)) {
        Leticia::ui_print("battery_icons: failed to resolve %s", kBoltZipEntry);
        return false;
    }

    g_initialized = true;
    return true;
}

const std::string &icon_path(int percent, Leticia::battery_status status) {
    static const std::string kEmpty;
    if (!g_initialized)
        return kEmpty;

    /* percent >= 100 always shows the fully-solid icon, regardless of
     * charge status: battery_android_6 (the highest of the 7 fill
     * steps) deliberately still shows a thin unfilled gap, reserved for
     * "high but not quite full". Without this check, a 100%-charged
     * battery that isn't currently plugged in would fall through to
     * that step and look less than full until charging began. */
    if (percent >= 100 || status == Leticia::battery_status::full)
        return g_full_path;

    if (status == Leticia::battery_status::unknown)
        return g_alert_path;

    /* Map 0-99 onto the 7 fill steps (0, 17, 33, 50, 67, 83, 100). */
    int step = (percent * (kFillStepCount - 1) + 50) / 100;
    if (step < 0)
        step = 0;
    if (step > kFillStepCount - 1)
        step = kFillStepCount - 1;

    return g_fill_paths[step];
}

const std::string &bolt_path() {
    static const std::string kEmpty;
    return g_initialized ? g_bolt_path : kEmpty;
}

} // namespace Leticia::gui::battery_icons
