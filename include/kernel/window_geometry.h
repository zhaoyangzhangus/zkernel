#pragma once

#include <uapi/window.h>
#include <stdbool.h>

/* 共用的窗口框架几何信息。其所有权仍由合成器之外的模块管理。 */
#define WINDOW_FRAME_BORDER              1U
#define WINDOW_FRAME_EXTRA               (WINDOW_FRAME_BORDER * 2U)
#define WINDOW_TITLEBAR_HEIGHT           30U
#define WINDOW_CLIENT_DRAG_REGION_HEIGHT 56U
#define WINDOW_TITLE_BUTTON_SIZE         20U
#define WINDOW_TITLE_BUTTON_GAP          4U
#define WINDOW_TITLE_BUTTON_RIGHT_MARGIN 6U
#define WINDOW_TITLE_CONTROLS_WIDTH \
    (WINDOW_TITLE_BUTTON_RIGHT_MARGIN + \
     WINDOW_TITLE_BUTTON_SIZE * 3U + \
     WINDOW_TITLE_BUTTON_GAP * 2U)

enum {
    WINDOW_TITLE_BUTTON_NONE = 0U,
    WINDOW_TITLE_BUTTON_MINIMIZE = 1U,
    WINDOW_TITLE_BUTTON_MAXIMIZE = 2U,
    WINDOW_TITLE_BUTTON_CLOSE = 3U,
};

bool window_client_decorations(uint32_t flags);
bool window_standard_caption(uint32_t flags);
bool window_caption_button_rect(uint32_t flags, int32_t x, int32_t y,
                                uint32_t width, uint32_t button,
                                int32_t *out_x, int32_t *out_y);
uint32_t window_frame_border(uint32_t flags);
uint32_t window_frame_extra(uint32_t flags);
uint32_t window_titlebar_height(uint32_t flags);
uint32_t window_client_offset_y(uint32_t flags);
uint32_t window_client_offset_x(uint32_t flags);
uint32_t window_outer_width(uint32_t width, uint32_t flags);
uint32_t window_outer_height(uint32_t height, uint32_t flags);
bool window_client_drag_region(uint32_t flags, int32_t window_x,
                               int32_t window_y, uint32_t window_width,
                               uint32_t pointer_x, uint32_t pointer_y);
bool window_title_button_rect(int32_t window_x,
                              int32_t window_y,
                              uint32_t window_width,
                              uint32_t button,
                              int32_t *out_x,
                              int32_t *out_y);
uint32_t window_corner_inset(uint32_t row, uint32_t width,
                             uint32_t height);
