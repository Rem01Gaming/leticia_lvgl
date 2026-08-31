#pragma once

#include <cstdint>

#include <lvgl.h>

#include "gui/dsl.hpp"
#include "gui/units.hpp"

namespace Leticia::ui {

/**
 * @brief Easing curve for an animation, mapped to LVGL's built-in path callbacks.
 */
enum class ease {
    linear,
    ease_in,
    ease_out,
    ease_in_out,
    overshoot,
    bounce,
};

/**
 * @brief Fluent wrapper around lv_anim_t.
 *
 * Chain the setters then call start(). LVGL copies the animation state on
 * start(), so this object does not need to outlive the running animation.
 */
class animation final {
public:
    animation();

    animation &target(void *var);
    animation &exec(lv_anim_exec_xcb_t cb);
    animation &values(int32_t start_value, int32_t end_value);
    animation &duration(uint32_t ms);
    animation &delay(uint32_t ms);
    animation &path(ease curve);
    animation &repeat(uint32_t count);
    animation &reverse(uint32_t duration_ms);
    animation &on_complete(lv_anim_completed_cb_t cb);

    /**
     * @brief Starts the animation.
     *
     * @return Pointer to the running animation, owned by LVGL's animation list.
     */
    lv_anim_t *start();

private:
    lv_anim_t anim_;
};

/**
 * @brief Fades a widget's opacity between two values.
 *
 * @param target Widget to animate.
 * @param from Starting opacity (0 to 255, LV_OPA_TRANSP to LV_OPA_COVER).
 * @param to Ending opacity.
 * @param duration_ms Animation duration in milliseconds.
 */
animation fade(widget &target, lv_opa_t from, lv_opa_t to, uint32_t duration_ms);

/**
 * @brief Scales a widget uniformly on both axes, with an overshoot bounce.
 *
 * @param target Widget to animate.
 * @param from_percent Starting scale, 100 is normal size.
 * @param to_percent Ending scale, 100 is normal size.
 * @param duration_ms Animation duration in milliseconds.
 */
animation scale_pop(widget &target, int32_t from_percent, int32_t to_percent, uint32_t duration_ms);

/**
 * @brief Slides a widget vertically between two dp offsets.
 *
 * @param target Widget to animate.
 * @param from Starting Y position.
 * @param to Ending Y position.
 * @param duration_ms Animation duration in milliseconds.
 */
animation slide_y(widget &target, units::dp from, units::dp to, uint32_t duration_ms);

} // namespace Leticia::ui
