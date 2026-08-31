#include "ui_scale.hpp"

#include <array>
#include <cstdlib>

namespace Leticia::ui_scale {

int estimate_dpi(int32_t hor_res, int32_t ver_res) {
    int32_t shortest_side = hor_res < ver_res ? hor_res : ver_res;
    int dpi = (shortest_side / 160) * 80;

    if (dpi < 120 || dpi > 960)
        dpi = 160;

    return dpi;
}

const lv_font_t *pick_font(int target_px) {
    struct font_entry {
        int px;
        const lv_font_t *font;
    };

    static const std::array<font_entry, 9> table = {{
        {14, &lv_font_montserrat_14},
        {20, &lv_font_montserrat_20},
        {24, &lv_font_montserrat_24},
        {28, &lv_font_montserrat_28},
        {32, &lv_font_montserrat_32},
        {36, &lv_font_montserrat_36},
        {40, &lv_font_montserrat_40},
        {44, &lv_font_montserrat_44},
        {48, &lv_font_montserrat_48},
    }};

    const lv_font_t *best = table[0].font;
    int best_diff = std::abs(target_px - table[0].px);

    for (size_t i = 1; i < table.size(); i++) {
        int diff = std::abs(target_px - table[i].px);
        if (diff < best_diff) {
            best_diff = diff;
            best = table[i].font;
        }
    }

    return best;
}

} // namespace Leticia::ui_scale
