#include <kernel/window_geometry.h>
#include <uapi/chrome_caption.h>

/* REFACTOR_P7A_GEOMETRY_OWNER: one Rect/decoration/corner geometry source. */


bool window_client_decorations(uint32_t flags) {
    return (flags & OS_WINDOW_CLIENT_DECORATIONS) != 0U;
}

bool window_standard_caption(uint32_t flags) {
    return (flags & OS_WINDOW_STANDARD_CAPTION_CONTROLS) != 0U;
}

bool window_caption_button_rect(uint32_t flags, int32_t x, int32_t y,
                                uint32_t width, uint32_t button,
                                int32_t *out_x, int32_t *out_y) {
    if (!window_client_decorations(flags) && !window_standard_caption(flags))
        return window_title_button_rect(x, y, width, button, out_x, out_y);
    if (!window_standard_caption(flags) || width < 3U*os_window_caption_button_width(flags) ||
        !out_x || !out_y || button < WINDOW_TITLE_BUTTON_MINIMIZE ||
        button > WINDOW_TITLE_BUTTON_CLOSE) return false;
    int64_t left = (int64_t)x + window_outer_width(width, flags) -
        (4U - button)*os_window_caption_button_width(flags);
    if (left < INT32_MIN || left > INT32_MAX) return false;
    *out_x = (int32_t)left;
    *out_y = y;
    return true;
}

uint32_t window_frame_border(uint32_t flags) {
    return window_client_decorations(flags) ? 0U : WINDOW_FRAME_BORDER;
}

uint32_t window_frame_extra(uint32_t flags) {
    return window_frame_border(flags) * 2U;
}

uint32_t window_titlebar_height(uint32_t flags) {
    return window_client_decorations(flags) ? 0U :
           window_standard_caption(flags) ? os_window_caption_height(flags) : WINDOW_TITLEBAR_HEIGHT;
}

uint32_t window_client_offset_y(uint32_t flags) {
    return window_frame_border(flags) + window_titlebar_height(flags);
}

uint32_t window_client_offset_x(uint32_t flags) {
    return window_frame_border(flags);
}

uint32_t window_outer_width(uint32_t width, uint32_t flags) {
    return width + window_frame_extra(flags);
}

uint32_t window_outer_height(uint32_t height, uint32_t flags) {
    return height + window_frame_extra(flags) + window_titlebar_height(flags);
}

bool window_client_drag_region(uint32_t flags, int32_t window_x,
                               int32_t window_y, uint32_t window_width,
                               uint32_t pointer_x, uint32_t pointer_y) {
    int64_t relative_x;
    int64_t relative_y;

    if (!window_client_decorations(flags) ||
        (flags & OS_WINDOW_EXPLICIT_MOVE_RESIZE) != 0U) {
        return false;
    }

    relative_x = (int64_t)pointer_x - window_x;
    relative_y = (int64_t)pointer_y - window_y;
    if (relative_x < 0 || relative_y < 0 ||
        relative_x >= (int64_t)window_width ||
        relative_y >= (window_standard_caption(flags) ? os_window_caption_height(flags) : WINDOW_CLIENT_DRAG_REGION_HEIGHT)) {
        return false;
    }

    /* 文件管理器的路径栏可编辑，不得把点击路径栏当作拖动窗口。 */
    if ((flags & OS_WINDOW_DARK_CAPTION) != 0U)
        return relative_x >= 48 && relative_x < 140;

    /* 保留应用的搜索、菜单、路径及关闭控件点击区域。 */
    if ((relative_x >= 45 && relative_x < 155) ||
        (relative_x >= 300 &&
         relative_x < (int64_t)window_width - 150)) {
        return true;
    }

    return false;
}

uint32_t window_corner_inset(uint32_t row, uint32_t width,
                             uint32_t height) {
    return chrome_frame_row_inset(row, width, height);
}

uint32_t compositor_corner_inset(uint32_t row, uint32_t width,
                                 uint32_t height) {
    return window_corner_inset(row, width, height);
}

bool window_title_button_rect(
    int32_t window_x,
    int32_t window_y,
    uint32_t window_width,
    uint32_t button,
    int32_t *out_x,
    int32_t *out_y) {

    uint32_t slot;
    int64_t x;

    if (out_x == 0 ||
        out_y == 0 ||
        window_width <
            WINDOW_TITLE_CONTROLS_WIDTH) {
        return false;
    }

    switch (button) {
        case WINDOW_TITLE_BUTTON_CLOSE:
            slot = 0U;
            break;

        case WINDOW_TITLE_BUTTON_MAXIMIZE:
            slot = 1U;
            break;

        case WINDOW_TITLE_BUTTON_MINIMIZE:
            slot = 2U;
            break;

        default:
            return false;
    }

    x =
        (int64_t)window_x +
        WINDOW_FRAME_BORDER +
        window_width -
        WINDOW_TITLE_BUTTON_RIGHT_MARGIN -
        WINDOW_TITLE_BUTTON_SIZE -
        (uint64_t)slot *
            (WINDOW_TITLE_BUTTON_SIZE +
             WINDOW_TITLE_BUTTON_GAP);

    if (x < -2147483648LL ||
        x > 2147483647LL) {
        return false;
    }

    *out_x =
        (int32_t)x;

    *out_y =
        window_y +
        (int32_t)WINDOW_FRAME_BORDER +
        (int32_t)(
            (WINDOW_TITLEBAR_HEIGHT -
             WINDOW_TITLE_BUTTON_SIZE) /
            2U);

    return true;
}


/*
 * Ring0 decoration hit testing.
 *
 * A title-control hit is consumed by the window server and must never become
 * client input or start a titlebar drag.
 */
/*
 * Flat controls: no shadow, no extra framebuffer and no alpha surface.
 */
