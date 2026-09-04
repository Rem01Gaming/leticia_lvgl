#include "units.hpp"

#include <cmath>

namespace Leticia::units {

namespace {

// Matches the Android convention documented in units.hpp (density = dpi / 160)
// and LVGL's own lv_display_dpx(), which scales relative to a 160 DPI display.
// This was previously 320, which silently halved every dp/sp value computed
// by this file relative to what the value's name implied (e.g. a documented
// "24dp" status bar rendered at the pixel height of a real 12dp).
constexpr float kBaselineDpi = 160.0f;
constexpr float kMinDensity = 0.5f;

float g_density = 1.0f;
float g_font_scale = 1.0f;

} // namespace

void init(int dpi) {
    g_density = static_cast<float>(dpi) / kBaselineDpi;
    if (g_density < kMinDensity)
        g_density = kMinDensity;
}

void set_font_scale(float scale) {
    g_font_scale = scale;
}

float density() {
    return g_density;
}

float font_scale() {
    return g_font_scale;
}

int32_t dp::px() const {
    return static_cast<int32_t>(std::lround(value_ * g_density));
}

int32_t sp::px() const {
    return static_cast<int32_t>(std::lround(value_ * g_density * g_font_scale));
}

} // namespace Leticia::units
