#include "internal.h"
#include <uapi/chrome_caption.h>

window_server_window_t *window_scene_hit_test_locked(uint32_t x, uint32_t y) {
    for (uint32_t index = g_window_server.scene.count; index != 0U; --index) {
        window_server_window_t *window = g_window_server.scene.children[index - 1U];
        int64_t relative_x;
        int64_t relative_y;
        uint32_t outer_width;
        uint32_t outer_height;
        uint32_t inset;

        if (window == 0 || window->minimized ||
            (window->flags & OS_WINDOW_VISIBLE) == 0U) continue;
        outer_width = window_outer_width(window->width, window->flags);
        outer_height = window_outer_height(window->height, window->flags);
        relative_x = (int64_t)x - window->x;
        relative_y = (int64_t)y - window->y;
        if (relative_x < 0 || relative_y < 0 ||
            relative_x >= (int64_t)outer_width ||
            relative_y >= (int64_t)outer_height) continue;
        inset = window_corner_inset((uint32_t)relative_y,
                                    outer_width, outer_height);
        if (window->maximized) inset = 0U;
        else if (window_standard_caption(window->flags))
            inset = chrome_frame_row_inset((uint32_t)relative_y, outer_width, outer_height);
        if (relative_x >= (int64_t)inset &&
            relative_x < (int64_t)outer_width - inset) return window;
    }
    return 0;
}
