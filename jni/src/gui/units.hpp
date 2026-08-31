#pragma once

#include <cstdint>

namespace Leticia::units {

/**
 * @brief Sets the density used to convert dp and sp to pixels.
 *
 * Density follows the Android convention: density = dpi / 160, so a value
 * of 1.0 means baseline mdpi (160dpi), 2.0 means xhdpi (320dpi), and so on.
 *
 * @param dpi Panel DPI, typically from Leticia::ui_scale::estimate_dpi().
 */
void init(int dpi);

/**
 * @brief Sets the sp-to-dp multiplier, mirroring Android's font scale setting.
 *
 * @param scale Font scale factor. 1.0 means sp and dp convert identically.
 */
void set_font_scale(float scale);

/**
 * @brief Gets the current density (dpi / 160).
 *
 * @return Current density factor.
 */
float density();

/**
 * @brief Gets the current font scale factor.
 *
 * @return Current font scale factor.
 */
float font_scale();

/**
 * @brief Density-independent pixel value, matching Android's dp unit.
 */
class dp final {
public:
    constexpr explicit dp(float value) : value_(value) {}

    /**
     * @brief Converts this value to physical pixels using the current density.
     *
     * @return Rounded pixel value.
     */
    int32_t px() const;

    constexpr float value() const { return value_; }

private:
    float value_;
};

/**
 * @brief Scale-independent pixel value, matching Android's sp unit for text.
 */
class sp final {
public:
    constexpr explicit sp(float value) : value_(value) {}

    /**
     * @brief Converts this value to physical pixels using density and font scale.
     *
     * @return Rounded pixel value.
     */
    int32_t px() const;

    constexpr float value() const { return value_; }

private:
    float value_;
};

constexpr dp operator""_dp(long double value) { return dp(static_cast<float>(value)); }
constexpr dp operator""_dp(unsigned long long value) { return dp(static_cast<float>(value)); }
constexpr sp operator""_sp(long double value) { return sp(static_cast<float>(value)); }
constexpr sp operator""_sp(unsigned long long value) { return sp(static_cast<float>(value)); }

constexpr dp operator-(dp value) { return dp(-value.value()); }
constexpr sp operator-(sp value) { return sp(-value.value()); }

} // namespace Leticia::units
