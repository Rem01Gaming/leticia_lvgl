#pragma once

#include <lvgl.h>

#include "gui/ui_scale.hpp"
#include "gui/units.hpp"

namespace Leticia::ui {

    using units::dp;
    using units::sp;

/**
 * @brief Base fluent wrapper around an lv_obj_t.
 *
 * Every setter returns a reference to itself so calls can be chained in
 * declarative order, e.g. widget(obj).size(200_dp, 80_dp).align(LV_ALIGN_CENTER).
 * Widgets never own the underlying lv_obj_t; LVGL's tree owns it, the same
 * as plain lv_obj_create() calls.
 */
    class widget {
    public:
        explicit widget(lv_obj_t *obj) : obj_(obj) {}

        lv_obj_t *raw() const { return obj_; }
        operator lv_obj_t *() const { return obj_; }

        widget &size(dp w, dp h) {
            lv_obj_set_size(obj_, w.px(), h.px());
            return *this;
        }

        widget &size_content() {
            lv_obj_set_size(obj_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            return *this;
        }

        widget &width(dp w) {
            lv_obj_set_width(obj_, w.px());
            return *this;
        }

        widget &width_pct(int32_t pct) {
            lv_obj_set_width(obj_, LV_PCT(pct));
            return *this;
        }

        widget &height(dp h) {
            lv_obj_set_height(obj_, h.px());
            return *this;
        }

        widget &pos(dp x, dp y) {
            lv_obj_set_pos(obj_, x.px(), y.px());
            return *this;
        }

        widget &align(lv_align_t alignment, dp x_ofs = dp{0}, dp y_ofs = dp{0}) {
            lv_obj_align(obj_, alignment, x_ofs.px(), y_ofs.px());
            return *this;
        }

        widget &center() {
            lv_obj_center(obj_);
            return *this;
        }

        widget &on(lv_event_code_t code, lv_event_cb_t cb, void *user_data = nullptr) {
            lv_obj_add_event_cb(obj_, cb, code, user_data);
            return *this;
        }

        widget &radius(dp r, lv_style_selector_t selector = LV_PART_MAIN) {
            lv_obj_set_style_radius(obj_, r.px(), selector);
            return *this;
        }

        widget &bg_color(lv_color_t color, lv_style_selector_t selector = LV_PART_MAIN) {
            lv_obj_set_style_bg_color(obj_, color, selector);
            return *this;
        }

        widget &bg_opa(lv_opa_t opa, lv_style_selector_t selector = LV_PART_MAIN) {
            lv_obj_set_style_bg_opa(obj_, opa, selector);
            return *this;
        }

        widget &text_color(lv_color_t color, lv_style_selector_t selector = LV_PART_MAIN) {
            lv_obj_set_style_text_color(obj_, color, selector);
            return *this;
        }

        widget &font(sp size, lv_style_selector_t selector = LV_PART_MAIN) {
            lv_obj_set_style_text_font(obj_, ui_scale::pick_font(size.px()), selector);
            return *this;
        }

        widget &pad(dp all, lv_style_selector_t selector = LV_PART_MAIN) {
            lv_obj_set_style_pad_all(obj_, all.px(), selector);
            return *this;
        }

        widget &border(dp width_, lv_color_t color, lv_style_selector_t selector = LV_PART_MAIN) {
            lv_obj_set_style_border_width(obj_, width_.px(), selector);
            lv_obj_set_style_border_color(obj_, color, selector);
            return *this;
        }

        widget &flex_flow(lv_flex_flow_t flow) {
            lv_obj_set_flex_flow(obj_, flow);
            return *this;
        }

        widget &flex_align(lv_flex_align_t main_place, lv_flex_align_t cross_place, lv_flex_align_t track_cross_place) {
            lv_obj_set_flex_align(obj_, main_place, cross_place, track_cross_place);
            return *this;
        }

        widget &no_scroll() {
            lv_obj_set_scrollbar_mode(obj_, LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_scrollable(obj_, false);
            return *this;
        }

        widget &hidden(bool state) {
            lv_obj_set_hidden(obj_, state);
            return *this;
        }

        /**
         * @brief Lets events on this object propagate to its parent instead
         * of stopping here.
         *
         * Every LVGL object is clickable by default, so a plain container
         * placed between a screen and its children (e.g. content_area)
         * intercepts taps meant for the screen's own handlers unless this
         * is set. Prefer this over re-attaching the same callback to every
         * new container -- it keeps a single handler on the ancestor
         * working for whatever gets added underneath later.
         */
        widget &bubble_events() {
            lv_obj_set_event_bubble(obj_, true);
            return *this;
        }

    protected:
        lv_obj_t *obj_;
    };

/**
 * @brief A screen root, created with no parent per LVGL 9.6 semantics.
 */
    class screen final : public widget {
    public:
        screen() : widget(lv_obj_create(nullptr)) {}

        void activate() { lv_screen_load(obj_); }
    };

/**
 * @brief A plain container object, the DSL equivalent of lv_obj_create().
 */
    class container final : public widget {
    public:
        explicit container(widget &parent) : widget(lv_obj_create(parent.raw())) {}
        explicit container(lv_obj_t *parent) : widget(lv_obj_create(parent)) {}
    };

/**
 * @brief A screen sub-region that starts below a fixed top inset.
 *
 * Views build their content inside this instead of directly on the screen,
 * so nothing is ever drawn underneath a persistent top overlay such as the
 * status bar.
 */
    class content_area final : public widget {
    public:
        content_area(widget &parent, int32_t top_inset_px) : widget(lv_obj_create(parent.raw())) {
            setup(parent.raw(), top_inset_px);
        }

        content_area(lv_obj_t *parent, int32_t top_inset_px) : widget(lv_obj_create(parent)) {
            setup(parent, top_inset_px);
        }

    private:
        void setup(lv_obj_t *parent, int32_t top_inset_px) {
            lv_obj_set_pos(obj_, 0, top_inset_px);
            lv_obj_set_width(obj_, LV_PCT(100));
            lv_obj_set_height(obj_, lv_obj_get_height(parent) - top_inset_px);
            lv_obj_set_style_pad_all(obj_, 0, LV_PART_MAIN);
            lv_obj_set_style_border_width(obj_, 0, LV_PART_MAIN);
            lv_obj_set_scrollbar_mode(obj_, LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_scrollable(obj_, false);
            /* Purely a layout container -- let clicks on empty space fall
             * through to whatever handlers the screen itself has (e.g. the
             * activity/wake handler), instead of dead-ending here. */
            lv_obj_set_event_bubble(obj_, true);
        }
    };

/**
 * @brief A text label.
 */
    class label final : public widget {
    public:
        label(widget &parent, const char *value) : widget(lv_label_create(parent.raw())) {
            lv_label_set_text(obj_, value);
        }

        label(lv_obj_t *parent, const char *value) : widget(lv_label_create(parent)) {
            lv_label_set_text(obj_, value);
        }

        label &text(const char *value) {
            lv_label_set_text(obj_, value);
            return *this;
        }

        /**
         * @brief Sets text without copying it, for a literal or a buffer the caller owns.
         */
        label &text_static(const char *value) {
            lv_label_set_text_static(obj_, value);
            return *this;
        }
    };

/**
 * @brief A clickable button with a single centered text label.
 */
    class button final : public widget {
    public:
        explicit button(widget &parent) : widget(lv_button_create(parent.raw())) {
            lv_obj_set_press_lock(obj_, false);
        }
        explicit button(lv_obj_t *parent) : widget(lv_button_create(parent)) {
            lv_obj_set_press_lock(obj_, false);
        }

        /**
         * @brief Creates or replaces this button's label child.
         *
         * @param value Text to show.
         * @param font_size Text size in sp. Ignored if the label already exists.
         */
        button &text(const char *value, sp font_size = sp{16.0f}) {
            lv_obj_t *lbl = label_obj();
            if (lbl == nullptr) {
                label built(obj_, value);
                built.font(font_size).center();
            } else {
                lv_label_set_text(lbl, value);
            }
            return *this;
        }

        /**
         * @brief Gets this button's label child, for runtime text updates.
         *
         * @return The label's lv_obj_t, or nullptr if none was created yet.
         */
        lv_obj_t *label_obj() const {
            return lv_obj_get_child(obj_, 0);
        }
    };

} // namespace Leticia::ui
