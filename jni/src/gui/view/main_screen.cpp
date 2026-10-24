#include "main_screen.hpp"

#include "gui/dsl.hpp"
#include "gui/icons.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace Leticia::screens {

namespace {

using namespace Leticia::units;
using Leticia::font_manager::weight;

constexpr uint32_t kAppRgb = 0x1d1d1d;
constexpr uint32_t kSurfaceRgb = 0x202020;
constexpr uint32_t kActiveRgb = 0x292929;
constexpr uint32_t kTextRgb = 0xf5f5f5;
constexpr uint32_t kMutedRgb = 0x888888;
constexpr uint32_t kAccentRgb = 0xdd669c;
constexpr uint32_t kMoreRgb = 0xa0a0a0;
constexpr uint32_t kMenuRgb = 0x303030;
constexpr uint32_t kMenuBorderRgb = 0x474747;
constexpr uint32_t kTrackRgb = 0x555555;

constexpr float kCoverDp = 38.0f;
constexpr float kRowMoreDp = 15.0f;       /* Kebab icon size inside a track row. */
constexpr float kRowPadRightDp = 12.0f;   /* Right padding of a track row. */
constexpr float kHeaderMoreBoxDp = 40.0f; /* Touch target of the header kebab. */
constexpr uint32_t kVolumeHideMs = 1500;
constexpr uint32_t kVolumePollMs = 50;
constexpr uint32_t kTickMs = 250;
constexpr size_t kNoIndex = static_cast<size_t>(-1);
constexpr lv_style_selector_t kPressed =
    static_cast<lv_style_selector_t>(LV_PART_MAIN) | static_cast<lv_style_selector_t>(LV_STATE_PRESSED);

enum class repeat_mode { off, track, list };
enum class order_mode { normal, shuffle };

struct player_view;

struct row_data {
    player_view *view;
    size_t index;
};

struct menu_choice {
    player_view *view;
    int value;
    lv_obj_t *item;
    lv_obj_t *icon;
    const char *icon_name;
};

struct player_view {
    Leticia::audio_manager *audio = nullptr;
    std::atomic<bool> *should_exit = nullptr;
    repeat_mode repeat = repeat_mode::track;
    order_mode order = order_mode::normal;

    lv_obj_t *rows = nullptr;
    lv_obj_t *status_label = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *artist = nullptr;
    lv_obj_t *art_image = nullptr;
    lv_obj_t *seek = nullptr;
    lv_obj_t *elapsed = nullptr;
    lv_obj_t *duration = nullptr;
    lv_obj_t *play_icon = nullptr;
    lv_obj_t *repeat_icon = nullptr;
    lv_obj_t *shuffle_icon = nullptr;
    lv_obj_t *scrim = nullptr;
    lv_obj_t *header_menu = nullptr;
    lv_obj_t *repeat_menu = nullptr;
    lv_obj_t *shuffle_menu = nullptr;
    lv_obj_t *alsa_modal = nullptr;
    lv_obj_t *alsa_values[9] = {};
    lv_obj_t *volume_overlay = nullptr;
    lv_obj_t *volume_bar = nullptr;
    lv_obj_t *volume_label = nullptr;
    uint32_t volume_hide_at_ms = 0;

    std::vector<lv_obj_t *> row_objects;
    std::vector<lv_obj_t *> row_icons;
    std::vector<row_data> row_data_items;
    std::vector<lv_image_dsc_t> cover_descriptors;
    std::deque<menu_choice> choices;

    size_t shown_track = kNoIndex;
    bool shown_audible = false;
    bool shown_repeat_valid = false;
    int shown_repeat = -1;
    int shown_order = -1;
    uint64_t shown_position = static_cast<uint64_t>(-1);
};

player_view g_view;

/**
 * Converts a 0xRRGGBB value to an LVGL color.
 *
 * @param rgb Color value.
 * @return LVGL color.
 */
lv_color_t color_of(uint32_t rgb) {
    return lv_color_hex(rgb);
}

/**
 * Creates a decorative container with no theme styling, no scrolling and no click handling.
 *
 * Decorative containers must not be clickable. A clickable child would take the
 * touch away from the button that holds it.
 *
 * @param parent Parent object.
 * @return New container.
 */
lv_obj_t *make_plain(lv_obj_t *parent) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    return obj;
}

/**
 * Creates a button with no theme styling and a pressed background.
 *
 * @param parent Parent object.
 * @param radius Corner radius in pixels.
 * @return New button.
 */
lv_obj_t *make_button(lv_obj_t *parent, int32_t radius) {
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_scrollbar_mode(button, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(button, false);
    lv_obj_set_style_radius(button, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_white(), kPressed);
    lv_obj_set_style_bg_opa(button, LV_OPA_20, kPressed);
    return button;
}

/**
 * Creates a label.
 *
 * @param parent Parent object.
 * @param text Initial text.
 * @param size Font size in sp.
 * @param rgb Text color.
 * @param w Font weight.
 * @return New label.
 */
lv_obj_t *make_label(lv_obj_t *parent, const char *text, float size, uint32_t rgb, weight w = weight::regular) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    const lv_font_t *font = Leticia::font_manager::get_font(sp{size}.px(), w);
    if (font != nullptr)
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color_of(rgb), LV_PART_MAIN);
    return label;
}

/**
 * Sets label text only when it differs, so LVGL does not invalidate the area for nothing.
 *
 * @param label Label object.
 * @param text New text.
 */
void set_text(lv_obj_t *label, const char *text) {
    if (std::strcmp(lv_label_get_text(label), text) != 0)
        lv_label_set_text(label, text);
}

/**
 * Creates an image that shows a tinted SVG icon.
 *
 * @param parent Parent object.
 * @param name Icon name.
 * @param rgb Icon color.
 * @param size_dp Edge length in dp.
 * @return New image.
 */
lv_obj_t *make_icon(lv_obj_t *parent, const char *name, uint32_t rgb, float size_dp) {
    lv_obj_t *image = lv_image_create(parent);
    lv_obj_set_size(image, dp{size_dp}.px(), dp{size_dp}.px());
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_src(image, Leticia::gui::icons::path(name, rgb).c_str());
    return image;
}

/**
 * Changes the icon of an image.
 *
 * @param image Image object.
 * @param name Icon name.
 * @param rgb Icon color.
 */
void set_icon(lv_obj_t *image, const char *name, uint32_t rgb) {
    lv_image_set_src(image, Leticia::gui::icons::path(name, rgb).c_str());
}

/**
 * Creates a round icon button.
 *
 * @param parent Parent object.
 * @param name Icon name.
 * @param rgb Icon color.
 * @param icon_dp Icon size in dp.
 * @param box_dp Button size in dp.
 * @param icon_out Receives the icon image.
 * @return New button.
 */
lv_obj_t *make_icon_button(lv_obj_t *parent, const char *name, uint32_t rgb, float icon_dp, float box_dp,
                           lv_obj_t **icon_out) {
    lv_obj_t *button = make_button(parent, LV_RADIUS_CIRCLE);
    lv_obj_set_size(button, dp{box_dp}.px(), dp{box_dp}.px());
    lv_obj_t *icon = make_icon(button, name, rgb, icon_dp);
    lv_obj_center(icon);
    if (icon_out != nullptr)
        *icon_out = icon;
    return button;
}

/**
 * Formats a duration as m:ss and updates a label.
 *
 * @param label Label object.
 * @param seconds Duration in seconds.
 */
void set_time_text(lv_obj_t *label, uint64_t seconds) {
    char text[16];
    snprintf(text, sizeof(text), "%llu:%02llu", static_cast<unsigned long long>(seconds / 60),
             static_cast<unsigned long long>(seconds % 60));
    set_text(label, text);
}

/**
 * Hides the scrim and every popup menu.
 *
 * @param view Player state.
 */
void close_menus(player_view &view) {
    lv_obj_set_hidden(view.scrim, true);
    lv_obj_set_hidden(view.header_menu, true);
    lv_obj_set_hidden(view.repeat_menu, true);
    lv_obj_set_hidden(view.shuffle_menu, true);
}

/**
 * Shows one popup menu above the scrim.
 *
 * @param view Player state.
 * @param menu Menu to show.
 */
void open_menu(player_view &view, lv_obj_t *menu) {
    close_menus(view);
    lv_obj_set_hidden(view.scrim, false);
    lv_obj_move_foreground(view.scrim);
    lv_obj_set_hidden(menu, false);
    lv_obj_move_foreground(menu);
}

/**
 * Updates the checked look of the repeat and shuffle menu items.
 *
 * @param view Player state.
 */
void refresh_menu_checks(player_view &view) {
    for (menu_choice &choice : view.choices) {
        bool checked;
        if (choice.value < 3)
            checked = static_cast<int>(view.repeat) == choice.value;
        else
            checked = (choice.value == 4) == (view.order == order_mode::shuffle);
        lv_obj_set_style_bg_color(choice.item, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(choice.item, checked ? LV_OPA_10 : LV_OPA_TRANSP, LV_PART_MAIN);
        set_icon(choice.icon, choice.icon_name, checked ? kAccentRgb : kMutedRgb);
    }
}

/**
 * Updates the play icon of one track row.
 *
 * @param view Player state.
 * @param index Row index.
 */
void update_row_icon(player_view &view, size_t index) {
    if (index >= view.row_icons.size())
        return;
    const bool show_pause = index == view.audio->current_track_index() && view.audio->is_audible();
    set_icon(view.row_icons[index], show_pause ? "pause" : "play", kTextRgb);
}

/**
 * Shows the cover of a track in an image object.
 *
 * @param image Image object.
 * @param track_index Track index.
 * @return true if the track has a cover.
 */
bool apply_cover(lv_obj_t *image, size_t track_index) {
    const auto &tracks = g_view.audio->tracks();
    if (track_index >= tracks.size() || track_index >= g_view.cover_descriptors.size())
        return false;

    const music_track_t &track = tracks[track_index];
    if (track.cover_pixels.empty())
        return false;

    lv_image_dsc_t &descriptor = g_view.cover_descriptors[track_index];
    descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    descriptor.header.flags = 0;
    descriptor.header.w = static_cast<uint16_t>(track.cover_width);
    descriptor.header.h = static_cast<uint16_t>(track.cover_height);
    descriptor.header.stride = static_cast<uint16_t>(track.cover_width * 2);
    descriptor.data_size = static_cast<uint32_t>(track.cover_pixels.size());
    descriptor.data = track.cover_pixels.data();
    lv_image_set_src(image, &descriptor);
    return true;
}

/**
 * Updates the title, artist, cover, duration and row highlight for the current track.
 *
 * @param view Player state.
 */
void sync_track(player_view &view) {
    const size_t current = view.audio->current_track_index();
    const music_track_t *track = view.audio->current_track();

    if (track != nullptr) {
        set_text(view.title, track->title.c_str());
        set_text(view.artist, track->artist.c_str());
        const int32_t max_seconds = static_cast<int32_t>(std::min<uint64_t>(track->duration_seconds, INT32_MAX));
        lv_slider_set_range(view.seek, 0, std::max<int32_t>(max_seconds, 1));
        set_time_text(view.duration, track->duration_seconds);
        lv_obj_set_hidden(view.art_image, !apply_cover(view.art_image, current));
    } else {
        set_text(view.title, "No music found");
        set_text(view.artist, "Check music_scan_path");
        lv_slider_set_range(view.seek, 0, 1);
        set_time_text(view.duration, 0);
        lv_obj_set_hidden(view.art_image, true);
    }

    const size_t previous = view.shown_track;
    view.shown_track = current;
    view.shown_position = static_cast<uint64_t>(-1);

    if (previous != kNoIndex && previous < view.row_objects.size()) {
        lv_obj_set_style_bg_color(view.row_objects[previous], color_of(kAppRgb), LV_PART_MAIN);
        update_row_icon(view, previous);
    }
    if (current < view.row_objects.size())
        lv_obj_set_style_bg_color(view.row_objects[current], color_of(kActiveRgb), LV_PART_MAIN);
    update_row_icon(view, current);
}

/**
 * Updates the play button, the repeat icon and the shuffle icon after a state change.
 *
 * @param view Player state.
 */
void sync_transport(player_view &view) {
    const bool audible = view.audio->is_audible();
    if (audible != view.shown_audible || !view.shown_repeat_valid) {
        set_icon(view.play_icon, audible ? "pause" : "play", kTextRgb);
        update_row_icon(view, view.audio->current_track_index());
        view.shown_audible = audible;
    }

    const int repeat = static_cast<int>(view.repeat);
    const int order = static_cast<int>(view.order);
    if (repeat != view.shown_repeat || order != view.shown_order || !view.shown_repeat_valid) {
        set_icon(view.repeat_icon, "repeat", view.repeat == repeat_mode::off ? kMutedRgb : kAccentRgb);
        set_icon(view.shuffle_icon, "shuffle", view.order == order_mode::shuffle ? kAccentRgb : kMutedRgb);
        refresh_menu_checks(view);
        view.shown_repeat = repeat;
        view.shown_order = order;
    }
    view.shown_repeat_valid = true;
}

/**
 * Moves the slider and the elapsed label to the playback position.
 *
 * @param view Player state.
 */
void sync_progress(player_view &view) {
    if (lv_slider_is_dragged(view.seek))
        return;

    const uint64_t position = view.audio->position_seconds();
    if (position == view.shown_position)
        return;

    view.shown_position = position;
    lv_slider_set_value(view.seek, static_cast<int32_t>(std::min<uint64_t>(position, INT32_MAX)), LV_ANIM_OFF);
    set_time_text(view.elapsed, position);
}

/**
 * Updates the rows of the ALSA info dialog.
 *
 * @param view Player state.
 */
void refresh_alsa_modal(player_view &view) {
    const Leticia::audio_device_info_t info = view.audio->device_info();
    char text[96];

    set_text(view.alsa_values[0], info.name.c_str());
    snprintf(text, sizeof(text), "hw:%u,%u", info.card, info.device);
    set_text(view.alsa_values[1], text);
    set_text(view.alsa_values[2], info.output.c_str());
    if (info.sample_rate != 0)
        snprintf(text, sizeof(text), "%u Hz", info.sample_rate);
    else
        snprintf(text, sizeof(text), "-");
    set_text(view.alsa_values[3], text);
    if (info.channels != 0)
        snprintf(text, sizeof(text), "%s, %zu ch", info.format.c_str(), info.channels);
    else
        snprintf(text, sizeof(text), "-");
    set_text(view.alsa_values[4], text);
    set_text(view.alsa_values[5], info.supported_formats.c_str());
    set_text(view.alsa_values[6], info.supported_rates.c_str());
    snprintf(text, sizeof(text), "%u", info.max_channels);
    set_text(view.alsa_values[7], info.max_channels != 0 ? text : "-");
    set_text(view.alsa_values[8], info.active ? (view.audio->is_paused() ? "Paused" : "Playing") : "Idle");
}

/**
 * Picks the next track index.
 *
 * @param view Player state.
 * @param manual true if the user pressed next.
 * @return Next index. It can equal the track count when the list ends.
 */
size_t next_index(player_view &view, bool manual) {
    const size_t count = view.audio->tracks().size();
    const size_t current = view.audio->current_track_index();
    if (count < 2)
        return 0;
    if (view.order == order_mode::shuffle) {
        size_t index = static_cast<size_t>(rand()) % count;
        return index == current ? (index + 1) % count : index;
    }
    if (manual || view.repeat == repeat_mode::list)
        return (current + 1) % count;
    return current + 1;
}

void track_clicked_cb(lv_event_t *event) {
    auto *data = static_cast<row_data *>(lv_event_get_user_data(event));
    player_view &view = *data->view;
    if (data->index == view.audio->current_track_index() && view.audio->is_playing())
        view.audio->toggle_playback();
    else
        view.audio->play_track(data->index);
    close_menus(view);
    sync_track(view);
    sync_transport(view);
}

void play_clicked_cb(lv_event_t *event) {
    auto *view = static_cast<player_view *>(lv_event_get_user_data(event));
    view->audio->toggle_playback();
    sync_transport(*view);
}

void previous_clicked_cb(lv_event_t *event) {
    auto *view = static_cast<player_view *>(lv_event_get_user_data(event));
    const size_t count = view->audio->tracks().size();
    if (count == 0)
        return;
    const size_t current = view->audio->current_track_index();
    view->audio->play_track(current == 0 ? count - 1 : current - 1);
    sync_track(*view);
    sync_transport(*view);
}

void next_clicked_cb(lv_event_t *event) {
    auto *view = static_cast<player_view *>(lv_event_get_user_data(event));
    if (view->audio->tracks().empty())
        return;
    view->audio->play_track(next_index(*view, true));
    sync_track(*view);
    sync_transport(*view);
}

void seek_changed_cb(lv_event_t *event) {
    auto *view = static_cast<player_view *>(lv_event_get_user_data(event));
    if (lv_slider_is_dragged(view->seek))
        set_time_text(view->elapsed, static_cast<uint64_t>(lv_slider_get_value(view->seek)));
}

void seek_released_cb(lv_event_t *event) {
    auto *view = static_cast<player_view *>(lv_event_get_user_data(event));
    const uint64_t target = static_cast<uint64_t>(lv_slider_get_value(view->seek));
    view->audio->seek_to(target);
    view->shown_position = static_cast<uint64_t>(-1);
}

void repeat_button_cb(lv_event_t *event) {
    auto *view = static_cast<player_view *>(lv_event_get_user_data(event));
    open_menu(*view, view->repeat_menu);
}

void shuffle_button_cb(lv_event_t *event) {
    auto *view = static_cast<player_view *>(lv_event_get_user_data(event));
    open_menu(*view, view->shuffle_menu);
}

void header_more_cb(lv_event_t *event) {
    auto *view = static_cast<player_view *>(lv_event_get_user_data(event));
    open_menu(*view, view->header_menu);
}

void scrim_cb(lv_event_t *event) {
    close_menus(*static_cast<player_view *>(lv_event_get_user_data(event)));
}

void menu_choice_cb(lv_event_t *event) {
    auto *choice = static_cast<menu_choice *>(lv_event_get_user_data(event));
    player_view &view = *choice->view;
    if (choice->value < 3)
        view.repeat = static_cast<repeat_mode>(choice->value);
    else
        view.order = choice->value == 4 ? order_mode::shuffle : order_mode::normal;
    close_menus(view);
    sync_transport(view);
}

void exit_clicked_cb(lv_event_t *event) {
    auto *view = static_cast<player_view *>(lv_event_get_user_data(event));
    view->should_exit->store(true, std::memory_order_relaxed);
}

void alsa_open_cb(lv_event_t *event) {
    auto *view = static_cast<player_view *>(lv_event_get_user_data(event));
    close_menus(*view);
    refresh_alsa_modal(*view);
    lv_obj_set_hidden(view->alsa_modal, false);
    lv_obj_move_foreground(view->alsa_modal);
}

void alsa_close_cb(lv_event_t *event) {
    auto *view = static_cast<player_view *>(lv_event_get_user_data(event));
    lv_obj_set_hidden(view->alsa_modal, true);
}

/**
 * Creates a popup menu on the top layer.
 *
 * @param view Player state.
 * @param labels Item labels. The list ends with nullptr.
 * @param icons Item icon names.
 * @param first_value Value of the first item. The next items count up.
 * @param checkable true if the items show a checked state.
 * @return New menu. It starts hidden.
 */
lv_obj_t *create_menu(player_view &view, const char *const *labels, const char *const *icons, int first_value,
                      bool checkable) {
    lv_obj_t *menu = make_plain(lv_layer_top());
    lv_obj_set_size(menu, dp{192}.px(), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(menu, color_of(kMenuRgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(menu, color_of(kMenuBorderRgb), LV_PART_MAIN);
    lv_obj_set_style_border_width(menu, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(menu, dp{6}.px(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(menu, dp{5}.px(), LV_PART_MAIN);
    lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_COLUMN);

    for (int index = 0; labels[index] != nullptr; ++index) {
        lv_obj_t *item = make_button(menu, dp{4}.px());
        lv_obj_set_size(item, LV_PCT(100), dp{42}.px());
        lv_obj_set_style_pad_left(item, dp{11}.px(), LV_PART_MAIN);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(item, dp{12}.px(), LV_PART_MAIN);
        lv_obj_t *icon = make_icon(item, icons[index], kMutedRgb, 16);
        make_label(item, labels[index], 13, kTextRgb);
        if (checkable) {
            view.choices.push_back({&view, first_value + index, item, icon, icons[index]});
            lv_obj_add_event_cb(item, menu_choice_cb, LV_EVENT_CLICKED, &view.choices.back());
        }
    }
    lv_obj_set_hidden(menu, true);
    return menu;
}

/**
 * Creates the header menu with the Exit and ALSA Info items.
 *
 * @param view Player state.
 * @param status_height Status bar height in pixels.
 */
void create_header_menu(player_view &view, int32_t status_height) {
    static const char *const labels[] = {"Exit", "ALSA Info", nullptr};
    static const char *const icons[] = {"right_from_bracket", "volume_high"};
    view.header_menu = create_menu(view, labels, icons, 0, false);
    lv_obj_align(view.header_menu, LV_ALIGN_TOP_RIGHT, -dp{12}.px(), status_height + dp{52}.px());

    lv_obj_t *exit_item = lv_obj_get_child(view.header_menu, 0);
    lv_obj_t *alsa_item = lv_obj_get_child(view.header_menu, 1);
    lv_obj_add_event_cb(exit_item, exit_clicked_cb, LV_EVENT_CLICKED, &view);
    lv_obj_add_event_cb(alsa_item, alsa_open_cb, LV_EVENT_CLICKED, &view);
}

/**
 * Creates the ALSA info dialog on the top layer.
 *
 * @param view Player state.
 */
void create_alsa_modal(player_view &view) {
    view.alsa_modal = make_plain(lv_layer_top());
    lv_obj_set_size(view.alsa_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(view.alsa_modal, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(view.alsa_modal, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_pad_all(view.alsa_modal, dp{16}.px(), LV_PART_MAIN);
    lv_obj_set_flex_flow(view.alsa_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view.alsa_modal, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(view.alsa_modal, true);

    lv_obj_t *panel = make_plain(view.alsa_modal);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_style_max_width(panel, dp{340}.px(), LV_PART_MAIN);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, LV_PCT(92), LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, color_of(0x252525), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, color_of(0x3d3d3d), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, dp{8}.px(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, dp{20}.px(), LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, dp{10}.px(), LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(panel, true);
    lv_obj_set_clickable(panel, true);

    make_label(panel, "ALSA Device Info", 18, kTextRgb, weight::semibold);
    make_label(panel, "Current audio output device", 12, kMutedRgb);

    static const char *const keys[] = {"Device", "PCM", "Output", "Sample rate", "Format",
                                       "Formats", "Rates", "Max channels", "Status"};
    for (size_t index = 0; index < 9; ++index) {
        lv_obj_t *row = make_plain(panel);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(row, dp{12}.px(), LV_PART_MAIN);
        make_label(row, keys[index], 13, kMutedRgb);
        lv_obj_t *value = make_label(row, "-", 13, kTextRgb);
        lv_obj_set_flex_grow(value, 1);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        view.alsa_values[index] = value;
    }

    lv_obj_t *close = make_button(panel, LV_RADIUS_CIRCLE);
    lv_obj_set_size(close, LV_PCT(100), dp{40}.px());
    lv_obj_set_style_bg_color(close, color_of(kAccentRgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *label = make_label(close, "Close", 13, 0xffffff, weight::semibold);
    lv_obj_center(label);
    lv_obj_add_event_cb(close, alsa_close_cb, LV_EVENT_CLICKED, &view);
    lv_obj_set_hidden(view.alsa_modal, true);
}

/**
 * Creates one track row.
 *
 * @param view Player state.
 * @param track Track data.
 * @param index Track index.
 */
void add_track_row(player_view &view, const Leticia::music_track_t &track, size_t index) {
    lv_obj_t *row = make_button(view.rows, 0);
    lv_obj_set_size(row, LV_PCT(100), dp{52}.px());
    lv_obj_set_style_bg_color(row, color_of(kAppRgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, color_of(kActiveRgb), kPressed);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, kPressed);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, color_of(0x111111), LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, dp{10}.px(), LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, dp{kRowPadRightDp}.px(), LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, dp{10}.px(), LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cover = make_plain(row);
    lv_obj_set_size(cover, dp{kCoverDp}.px(), dp{kCoverDp}.px());
    lv_obj_set_style_bg_color(cover, color_of(0x393939), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(cover, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(cover, true, LV_PART_MAIN);
    lv_obj_t *cover_image = lv_image_create(cover);
    lv_obj_set_pos(cover_image, 0, 0);
    if (apply_cover(cover_image, index) == false)
        lv_obj_set_hidden(cover_image, true);
    lv_obj_t *icon = make_icon(cover, "play", kTextRgb, 13);
    lv_obj_center(icon);

    lv_obj_t *copy = make_plain(row);
    lv_obj_set_flex_grow(copy, 1);
    lv_obj_set_height(copy, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(copy, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(copy, dp{2}.px(), LV_PART_MAIN);
    lv_obj_t *title = make_label(copy, track.title.c_str(), 13, kTextRgb, weight::semibold);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *artist = make_label(copy, track.artist.c_str(), 12, kMutedRgb);
    lv_obj_set_width(artist, LV_PCT(100));
    lv_label_set_long_mode(artist, LV_LABEL_LONG_MODE_DOTS);

    make_icon(row, "ellipsis_vertical", kMoreRgb, kRowMoreDp);

    view.row_objects.push_back(row);
    view.row_icons.push_back(icon);
    view.row_data_items.push_back({&view, index});
    lv_obj_add_event_cb(row, track_clicked_cb, LV_EVENT_CLICKED, &view.row_data_items.back());
}

/**
 * Fills the track list after the library scan finished.
 *
 * @param view Player state.
 */
void populate_rows(player_view &view) {
    const auto &tracks = view.audio->tracks();
    lv_obj_clean(view.rows);
    view.row_objects.clear();
    view.row_icons.clear();
    view.row_data_items.clear();
    view.row_objects.reserve(tracks.size());
    view.row_icons.reserve(tracks.size());
    view.row_data_items.reserve(tracks.size());
    view.cover_descriptors.assign(tracks.size(), lv_image_dsc_t{});
    view.shown_track = kNoIndex;

    for (size_t index = 0; index < tracks.size(); ++index)
        add_track_row(view, tracks[index], index);

    if (tracks.empty()) {
        view.status_label = make_label(view.rows, "No music files found", 14, kMutedRgb);
        lv_obj_set_width(view.status_label, LV_PCT(100));
        lv_obj_set_style_text_align(view.status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_pad_top(view.status_label, dp{24}.px(), LV_PART_MAIN);
    } else {
        view.status_label = nullptr;
    }
    sync_track(view);
    sync_transport(view);
}

/**
 * Creates the volume indicator on the top layer. It starts hidden.
 *
 * @param view Player state.
 * @param status_height Height of the status bar in pixels.
 */
void create_volume_overlay(player_view &view, int32_t status_height) {
    lv_obj_t *overlay = make_plain(lv_layer_top());
    lv_obj_set_size(overlay, dp{196}.px(), dp{40}.px());
    lv_obj_set_style_bg_color(overlay, color_of(kMenuRgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(overlay, color_of(kMenuBorderRgb), LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(overlay, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_left(overlay, dp{14}.px(), LV_PART_MAIN);
    lv_obj_set_style_pad_right(overlay, dp{14}.px(), LV_PART_MAIN);
    lv_obj_set_style_pad_column(overlay, dp{10}.px(), LV_PART_MAIN);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(overlay, LV_ALIGN_TOP_MID, 0, status_height + dp{8}.px());

    make_icon(overlay, "volume_high", kTextRgb, 16);

    lv_obj_t *bar = lv_bar_create(overlay);
    lv_obj_remove_style_all(bar);
    lv_obj_set_height(bar, dp{4}.px());
    lv_obj_set_flex_grow(bar, 1);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, color_of(kTrackRgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, color_of(kAccentRgb), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    lv_obj_t *label = make_label(overlay, "100%", 12, kTextRgb, weight::semibold);
    lv_obj_set_width(label, dp{34}.px());
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    lv_obj_set_hidden(overlay, true);
    view.volume_overlay = overlay;
    view.volume_bar = bar;
    view.volume_label = label;
}

/**
 * Shows the volume indicator after a volume change and hides it again after a short delay.
 *
 * @param timer Timer that carries the player state.
 */
void volume_timer_cb(lv_timer_t *timer) {
    auto *view = static_cast<player_view *>(lv_timer_get_user_data(timer));

    if (view->audio->take_volume_changed()) {
        const int volume = view->audio->volume();
        lv_bar_set_value(view->volume_bar, volume, LV_ANIM_OFF);
        char text[8];
        snprintf(text, sizeof(text), "%d%%", volume);
        set_text(view->volume_label, text);
        lv_obj_set_hidden(view->volume_overlay, false);
        lv_obj_move_foreground(view->volume_overlay);
        view->volume_hide_at_ms = lv_tick_get() + kVolumeHideMs;
    } else if (!lv_obj_is_hidden(view->volume_overlay) &&
               static_cast<int32_t>(lv_tick_get() - view->volume_hide_at_ms) >= 0) {
        lv_obj_set_hidden(view->volume_overlay, true);
    }
}

void player_timer_cb(lv_timer_t *timer) {
    auto *view = static_cast<player_view *>(lv_timer_get_user_data(timer));

    if (view->audio->take_scan_result())
        populate_rows(*view);
    else if (view->status_label != nullptr && view->audio->tracks().empty()) {
        char text[48];
        snprintf(text, sizeof(text), "Scanning music... %zu", view->audio->scan_progress());
        set_text(view->status_label, text);
    }

    if (view->audio->take_track_finished()) {
        const size_t current = view->audio->current_track_index();
        const size_t count = view->audio->tracks().size();
        if (count != 0 && view->repeat == repeat_mode::track)
            view->audio->play_track(current);
        else if (count != 0 && (view->order == order_mode::shuffle || view->repeat == repeat_mode::list || current + 1 < count))
            view->audio->play_track(next_index(*view, false));
        sync_track(*view);
    }

    if (view->audio->current_track_index() != view->shown_track)
        sync_track(*view);
    sync_transport(*view);
    sync_progress(*view);

    if (!lv_obj_is_hidden(view->alsa_modal))
        refresh_alsa_modal(*view);
}

} // namespace

unsigned int cover_size_px() {
    return static_cast<unsigned int>(dp{kCoverDp}.px());
}

void build_main_screen(Leticia::audio_manager &audio, const Leticia::gui::status_bar &status_bar,
                       std::atomic<bool> &should_exit) {
    player_view &view = g_view;
    view.audio = &audio;
    view.should_exit = &should_exit;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, color_of(kAppRgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *root = make_plain(screen);
    lv_obj_set_pos(root, 0, status_bar.height_px());
    lv_obj_set_size(root, LV_PCT(100), lv_obj_get_height(screen) - status_bar.height_px());
    lv_obj_set_style_bg_color(root, color_of(kAppRgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *header = make_plain(root);
    lv_obj_set_size(header, LV_PCT(100), dp{56}.px());
    lv_obj_set_style_pad_left(header, dp{14}.px(), LV_PART_MAIN);
    lv_obj_t *app_title = make_label(header, "Leticia", 17, kTextRgb, weight::semibold);
    lv_obj_align(app_title, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *header_more = make_icon_button(header, "ellipsis_vertical", kTextRgb, 16, kHeaderMoreBoxDp, nullptr);
    /* Put the header kebab on the same vertical axis as the kebabs of the track rows.
     * The row icon center is its right padding plus half its width from the right edge.
     * The button center must land there, so the button edge is offset by the difference. */
    const double row_icon_center = dp{kRowPadRightDp}.px() + dp{kRowMoreDp}.px() / 2.0;
    const double button_half = dp{kHeaderMoreBoxDp}.px() / 2.0;
    lv_obj_align(header_more, LV_ALIGN_RIGHT_MID, -static_cast<int32_t>(std::lround(row_icon_center - button_half)), 0);
    lv_obj_add_event_cb(header_more, header_more_cb, LV_EVENT_CLICKED, &view);

    view.rows = make_plain(root);
    lv_obj_set_width(view.rows, LV_PCT(100));
    lv_obj_set_flex_grow(view.rows, 1);
    lv_obj_set_flex_flow(view.rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(view.rows, true);
    lv_obj_set_scroll_dir(view.rows, LV_DIR_VER);
    lv_obj_set_clickable(view.rows, true);
    view.status_label = make_label(view.rows, "Scanning music...", 14, kMutedRgb);
    lv_obj_set_width(view.status_label, LV_PCT(100));
    lv_obj_set_style_text_align(view.status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_top(view.status_label, dp{24}.px(), LV_PART_MAIN);

    lv_obj_t *player = make_plain(root);
    lv_obj_set_size(player, LV_PCT(100), dp{150}.px());
    lv_obj_set_style_bg_color(player, color_of(kSurfaceRgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(player, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(player, dp{12}.px(), LV_PART_MAIN);
    lv_obj_set_style_pad_right(player, dp{12}.px(), LV_PART_MAIN);
    lv_obj_set_style_pad_top(player, dp{12}.px(), LV_PART_MAIN);

    view.title = make_label(player, "", 14, kTextRgb, weight::semibold);
    lv_obj_set_width(view.title, LV_PCT(80));
    lv_label_set_long_mode(view.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(view.title, LV_ALIGN_TOP_LEFT, 0, 0);
    view.artist = make_label(player, "", 12, kMutedRgb);
    lv_obj_set_width(view.artist, LV_PCT(80));
    lv_label_set_long_mode(view.artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(view.artist, LV_ALIGN_TOP_LEFT, 0, dp{21}.px());

    lv_obj_t *art_box = make_plain(player);
    lv_obj_set_size(art_box, dp{kCoverDp}.px(), dp{kCoverDp}.px());
    lv_obj_set_style_bg_color(art_box, color_of(0x393939), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(art_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(art_box, dp{3}.px(), LV_PART_MAIN);
    lv_obj_set_style_clip_corner(art_box, true, LV_PART_MAIN);
    lv_obj_align(art_box, LV_ALIGN_TOP_RIGHT, 0, 0);
    view.art_image = lv_image_create(art_box);
    lv_obj_set_pos(view.art_image, 0, 0);
    lv_obj_set_hidden(view.art_image, true);

    view.seek = lv_slider_create(player);
    lv_obj_remove_style_all(view.seek);
    lv_obj_set_size(view.seek, LV_PCT(100), dp{3}.px());
    lv_obj_set_style_bg_color(view.seek, color_of(kTrackRgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(view.seek, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(view.seek, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(view.seek, color_of(kAccentRgb), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(view.seek, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(view.seek, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(view.seek, color_of(kAccentRgb), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(view.seek, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(view.seek, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(view.seek, dp{3}.px(), LV_PART_KNOB);
    lv_obj_set_ext_click_area(view.seek, dp{16}.px());
    lv_obj_align(view.seek, LV_ALIGN_TOP_MID, 0, dp{56}.px());
    lv_obj_add_event_cb(view.seek, seek_changed_cb, LV_EVENT_VALUE_CHANGED, &view);
    lv_obj_add_event_cb(view.seek, seek_released_cb, LV_EVENT_RELEASED, &view);

    view.elapsed = make_label(player, "0:00", 10, kMutedRgb);
    lv_obj_align(view.elapsed, LV_ALIGN_TOP_LEFT, 0, dp{68}.px());
    view.duration = make_label(player, "0:00", 10, kMutedRgb);
    lv_obj_align(view.duration, LV_ALIGN_TOP_RIGHT, 0, dp{68}.px());

    lv_obj_t *transport = make_plain(player);
    lv_obj_set_size(transport, LV_PCT(100), dp{52}.px());
    lv_obj_set_flex_flow(transport, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(transport, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(transport, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *repeat_button = make_icon_button(transport, "repeat", kAccentRgb, 16, 46, &view.repeat_icon);
    lv_obj_add_event_cb(repeat_button, repeat_button_cb, LV_EVENT_CLICKED, &view);
    lv_obj_t *previous = make_icon_button(transport, "backward_step", kTextRgb, 18, 46, nullptr);
    lv_obj_add_event_cb(previous, previous_clicked_cb, LV_EVENT_CLICKED, &view);
    lv_obj_t *play = make_icon_button(transport, "play", kTextRgb, 22, 50, &view.play_icon);
    lv_obj_add_event_cb(play, play_clicked_cb, LV_EVENT_CLICKED, &view);
    lv_obj_t *next = make_icon_button(transport, "forward_step", kTextRgb, 18, 46, nullptr);
    lv_obj_add_event_cb(next, next_clicked_cb, LV_EVENT_CLICKED, &view);
    lv_obj_t *shuffle_button = make_icon_button(transport, "shuffle", kMutedRgb, 16, 46, &view.shuffle_icon);
    lv_obj_add_event_cb(shuffle_button, shuffle_button_cb, LV_EVENT_CLICKED, &view);

    view.scrim = make_plain(lv_layer_top());
    lv_obj_set_size(view.scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_clickable(view.scrim, true);
    lv_obj_add_event_cb(view.scrim, scrim_cb, LV_EVENT_CLICKED, &view);
    lv_obj_set_hidden(view.scrim, true);

    static const char *const repeat_labels[] = {"Repeat off", "Repeat track", "Repeat list", nullptr};
    static const char *const repeat_icons[] = {"circle_xmark", "repeat", "repeat"};
    view.repeat_menu = create_menu(view, repeat_labels, repeat_icons, 0, true);
    lv_obj_align(view.repeat_menu, LV_ALIGN_BOTTOM_LEFT, dp{10}.px(), -dp{68}.px());

    static const char *const order_labels[] = {"In order", "Shuffle list", nullptr};
    static const char *const order_icons[] = {"arrow_down_wide_short", "shuffle"};
    view.shuffle_menu = create_menu(view, order_labels, order_icons, 3, true);
    lv_obj_align(view.shuffle_menu, LV_ALIGN_BOTTOM_RIGHT, -dp{10}.px(), -dp{68}.px());

    create_header_menu(view, status_bar.height_px());
    create_alsa_modal(view);
    create_volume_overlay(view, status_bar.height_px());

    sync_track(view);
    sync_transport(view);
    lv_timer_create(player_timer_cb, kTickMs, &view);
    lv_timer_create(volume_timer_cb, kVolumePollMs, &view);
}

} // namespace Leticia::screens
