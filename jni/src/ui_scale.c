#include "ui_scale.h"

#include <stdlib.h>

#define UI_SCALE_BASELINE_DPI 160

int ui_scale_estimate_dpi(int32_t hor_res, int32_t ver_res)
{
    int32_t shortest_side = hor_res < ver_res ? hor_res : ver_res;
    int dpi = (shortest_side / 160) * 80;

    if (dpi < 120 || dpi > 960)
        dpi = 160;

    return dpi;
}

float ui_scale_factor(int dpi)
{
    float factor = (float)dpi / (float)UI_SCALE_BASELINE_DPI;

    if (factor < 1.0f)
        factor = 1.0f;
    if (factor > 4.0f)
        factor = 4.0f;

    return factor;
}

const lv_font_t *ui_scale_pick_font(int target_px)
{
    static const struct {
        int px;
        const lv_font_t *font;
    } table[] = {
        {14, &lv_font_montserrat_14},
        {20, &lv_font_montserrat_20},
        {24, &lv_font_montserrat_24},
        {28, &lv_font_montserrat_28},
        {32, &lv_font_montserrat_32},
        {36, &lv_font_montserrat_36},
        {40, &lv_font_montserrat_40},
        {44, &lv_font_montserrat_44},
        {48, &lv_font_montserrat_48},
    };
    const size_t table_len = sizeof(table) / sizeof(table[0]);

    const lv_font_t *best = table[0].font;
    int best_diff = abs(target_px - table[0].px);

    for (size_t i = 1; i < table_len; i++) {
        int diff = abs(target_px - table[i].px);
        if (diff < best_diff) {
            best_diff = diff;
            best = table[i].font;
        }
    }

    return best;
}
