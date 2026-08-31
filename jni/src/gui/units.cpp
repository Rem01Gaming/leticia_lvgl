#include "units.hpp"

#include <cmath>

namespace Leticia::units {

namespace {

constexpr float kBaselineDpi = 320.0f;
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
