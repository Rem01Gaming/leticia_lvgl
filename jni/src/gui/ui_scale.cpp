#include "ui_scale.hpp"

namespace Leticia::ui_scale {

int estimate_dpi(int32_t hor_res, int32_t ver_res) {
    int32_t shortest_side = hor_res < ver_res ? hor_res : ver_res;
    int dpi = (shortest_side / 160) * 80;

    if (dpi < 120 || dpi > 960)
        dpi = 160;

    return dpi;
}

} // namespace Leticia::ui_scale
