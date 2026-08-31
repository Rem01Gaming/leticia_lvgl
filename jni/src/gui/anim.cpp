#include "anim.hpp"

namespace Leticia::ui {

namespace {

lv_anim_path_cb_t path_cb_for(ease curve) {
    switch (curve) {
        case ease::linear:
            return lv_anim_path_linear;
        case ease::ease_in:
            return lv_anim_path_ease_in;
        case ease::ease_out:
            return lv_anim_path_ease_out;
        case ease::ease_in_out:
            return lv_anim_path_ease_in_out;
        case ease::overshoot:
            return lv_anim_path_overshoot;
        case ease::bounce:
            return lv_anim_path_bounce;
    }
    return lv_anim_path_linear;
}

void exec_opa(void *var, int32_t value) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), static_cast<lv_opa_t>(value), LV_PART_MAIN);
}

void exec_uniform_scale(void *var, int32_t value) {
    lv_obj_t *obj = static_cast<lv_obj_t *>(var);
    lv_obj_set_style_transform_scale_x(obj, value, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(obj, value, LV_PART_MAIN);
}

void exec_y(void *var, int32_t value) {
    lv_obj_set_y(static_cast<lv_obj_t *>(var), value);
}

int32_t percent_to_lv_scale(int32_t percent) {
    return (percent * LV_SCALE_NONE) / 100;
}

} // namespace

animation::animation() {
    lv_anim_init(&anim_);
}

animation &animation::target(void *var) {
    lv_anim_set_var(&anim_, var);
    return *this;
}

animation &animation::exec(lv_anim_exec_xcb_t cb) {
    lv_anim_set_exec_cb(&anim_, cb);
    return *this;
}

animation &animation::values(int32_t start_value, int32_t end_value) {
    lv_anim_set_values(&anim_, start_value, end_value);
    return *this;
}

animation &animation::duration(uint32_t ms) {
    lv_anim_set_duration(&anim_, ms);
    return *this;
}

animation &animation::delay(uint32_t ms) {
    lv_anim_set_delay(&anim_, ms);
    return *this;
}

animation &animation::path(ease curve) {
    lv_anim_set_path_cb(&anim_, path_cb_for(curve));
    return *this;
}

animation &animation::repeat(uint32_t count) {
    lv_anim_set_repeat_count(&anim_, count);
    return *this;
}

animation &animation::reverse(uint32_t duration_ms) {
    lv_anim_set_reverse_duration(&anim_, duration_ms);
    return *this;
}

animation &animation::on_complete(lv_anim_completed_cb_t cb) {
    lv_anim_set_completed_cb(&anim_, cb);
    return *this;
}

lv_anim_t *animation::start() {
    return lv_anim_start(&anim_);
}

animation fade(widget &target, lv_opa_t from, lv_opa_t to, uint32_t duration_ms) {
    return animation()
        .target(target.raw())
        .exec(exec_opa)
        .values(from, to)
        .duration(duration_ms)
        .path(ease::ease_out);
}

animation scale_pop(widget &target, int32_t from_percent, int32_t to_percent, uint32_t duration_ms) {
    return animation()
        .target(target.raw())
        .exec(exec_uniform_scale)
        .values(percent_to_lv_scale(from_percent), percent_to_lv_scale(to_percent))
        .duration(duration_ms)
        .path(ease::overshoot);
}

animation slide_y(widget &target, units::dp from, units::dp to, uint32_t duration_ms) {
    return animation()
        .target(target.raw())
        .exec(exec_y)
        .values(from.px(), to.px())
        .duration(duration_ms)
        .path(ease::ease_out);
}

} // namespace Leticia::ui
