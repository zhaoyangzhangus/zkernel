#include <ascii_font.h>
#include <uapi/window_theme.h>
#include <uapi/chrome_caption.h>
#include <uapi/utf8.h>
#include "internal.h"


/* REFACTOR_P7C_DECORATION_OWNER：标题栏、控件和窗口装饰。 */

static void compositor_put_pixel_locked(int32_t x, int32_t y,
                                            uint32_t color) {
    if (g_window_server.composite_framebuffer == 0 ||
        x < 0 ||
        y < 0 ||
        x >= (int32_t)g_window_server.display_width ||
        y >= (int32_t)g_window_server.display_height ||
        x < (int32_t)g_compositor_snapshot.damage_bounds.x0 ||
        y < (int32_t)g_compositor_snapshot.damage_bounds.y0 ||
        x >= (int32_t)g_compositor_snapshot.damage_bounds.x1 ||
        y >= (int32_t)g_compositor_snapshot.damage_bounds.y1) {
        return;
    }

    g_window_server.composite_framebuffer[
        (uint64_t)(uint32_t)y * g_window_server.display_stride +
        (uint32_t)x] = os_theme_resolve(color,g_compositor_snapshot.theme);
}

static void compositor_blend_pixel_locked(int32_t x, int32_t y,
                                           uint32_t color, uint32_t alpha) {
    color=os_theme_resolve(color,g_compositor_snapshot.theme);
    uint32_t destination;
    uint32_t inverse;
    uint32_t red;
    uint32_t green;
    uint32_t blue;

    if (g_window_server.composite_framebuffer == 0 || alpha == 0U ||
        x < 0 || y < 0 ||
        x >= (int32_t)g_window_server.display_width ||
        y >= (int32_t)g_window_server.display_height ||
        x < (int32_t)g_compositor_snapshot.damage_bounds.x0 ||
        y < (int32_t)g_compositor_snapshot.damage_bounds.y0 ||
        x >= (int32_t)g_compositor_snapshot.damage_bounds.x1 ||
        y >= (int32_t)g_compositor_snapshot.damage_bounds.y1) {
        return;
    }

    if (alpha >= 255U) {
        compositor_put_pixel_locked(x, y, color);
        return;
    }

    destination = g_window_server.composite_framebuffer[
        (uint64_t)(uint32_t)y * g_window_server.display_stride +
        (uint32_t)x];
    inverse = 255U - alpha;
    red = (((color >> 16U) & 0xFFU) * alpha +
           ((destination >> 16U) & 0xFFU) * inverse + 127U) / 255U;
    green = (((color >> 8U) & 0xFFU) * alpha +
             ((destination >> 8U) & 0xFFU) * inverse + 127U) / 255U;
    blue = ((color & 0xFFU) * alpha +
            (destination & 0xFFU) * inverse + 127U) / 255U;
    g_window_server.composite_framebuffer[
        (uint64_t)(uint32_t)y * g_window_server.display_stride +
        (uint32_t)x] = (destination & 0xFF000000U) |
                        (red << 16U) | (green << 8U) | blue;
}

static void compositor_title_controls_locked(
    const compositor_window_view_t *window) {

    static const uint32_t buttons[] = {
        WINDOW_TITLE_BUTTON_MINIMIZE,
        WINDOW_TITLE_BUTTON_MAXIMIZE,
        WINDOW_TITLE_BUTTON_CLOSE,
    };

    bool focused;

    if (window == 0) {
        return;
    }

    if (window_client_decorations(window->flags)) {
        return;
    }
    focused =
        window->identifier ==
        g_compositor_snapshot.focused_identifier;

    for (uint32_t index = 0U;
         index <
             sizeof(buttons) /
             sizeof(buttons[0]);
         ++index) {

        uint32_t button =
            buttons[index];

        int32_t x;
        int32_t y;

        uint32_t background;
        uint32_t glyph;

        if (!window_title_button_rect(
                window->x,
                window->y,
                window->width,
                button,
                &x,
                &y)) {
            continue;
        }

        background =
            focused ?
                OS_THEME_HEADER :
                OS_THEME_HEADER_INACTIVE;

        glyph =
            focused ?
                OS_THEME_TEXT :
                OS_THEME_MUTED;

        compositor_fill_rounded_locked(
            x,
            y,
            WINDOW_TITLE_BUTTON_SIZE,
            WINDOW_TITLE_BUTTON_SIZE,
            WINDOW_CORNER_RADIUS,
            background);

        if (button ==
            WINDOW_TITLE_BUTTON_MINIMIZE) {

            compositor_fill_locked(
                x + 6,
                y + 11,
                8U,
                1U,
                glyph);

        } else if (
            button ==
            WINDOW_TITLE_BUTTON_MAXIMIZE) {

            if (!window->maximized) {
                /* 普通最大化图标。 */
                compositor_fill_locked(
                    x + 6,
                    y + 6,
                    8U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 6,
                    y + 13,
                    8U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 6,
                    y + 6,
                    1U,
                    8U,
                    glyph);

                compositor_fill_locked(
                    x + 13,
                    y + 6,
                    1U,
                    8U,
                    glyph);

            } else {
                /* 还原图标：两个重叠矩形。 */
                compositor_fill_locked(
                    x + 8,
                    y + 6,
                    7U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 14,
                    y + 6,
                    1U,
                    7U,
                    glyph);

                compositor_fill_locked(
                    x + 7,
                    y + 8,
                    7U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 7,
                    y + 8,
                    1U,
                    7U,
                    glyph);

                compositor_fill_locked(
                    x + 7,
                    y + 14,
                    7U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 13,
                    y + 8,
                    1U,
                    7U,
                    glyph);
            }
        } else if (
            button ==
            WINDOW_TITLE_BUTTON_CLOSE) {

            /* 小型 X。 */
            for (uint32_t step = 0U;
                 step < 7U;
                 ++step) {

                compositor_fill_locked(
                    x + 7 +
                        (int32_t)step,
                    y + 7 +
                        (int32_t)step,
                    1U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 13 -
                        (int32_t)step,
                    y + 7 +
                        (int32_t)step,
                    1U,
                    1U,
                    glyph);
            }
        }
    }
}

/* 声明式按钮与原生标题栏共用同一套图标轮廓。 */
void compositor_control_caption_icon(int64_t x,int64_t y,uint32_t width,uint32_t height,
    uint32_t action,bool maximized,bool active) {
    uint32_t icon=action==OS_CONTROL_WINDOW_MAXIMIZE && maximized ? 4U : action;
    uint32_t side=(width<height?width:height)*12U/32U;
    if (!side || icon<1U || icon>4U) return;
    static uint8_t cache[4][12][12];
    static bool ready;
    if (!ready) {
        for (uint32_t k=1;k<=4;++k) for(uint32_t row=0;row<12;++row)
            for(uint32_t col=0;col<12;++col) cache[k-1U][row][col]=chrome_caption_alpha(k,col,row);
        ready=true;
    }
    for(uint32_t row=0;row<side;++row) for(uint32_t col=0;col<side;++col)
        compositor_blend_pixel_locked((int32_t)(x+(width-side)/2U+col),
            (int32_t)(y+(height-side)/2U+row),OS_THEME_MUTED,
            (cache[icon-1U][row*12U/side][col*12U/side]*(active?255U:97U)+127U)/255U);
}

void compositor_standard_caption_locked(const compositor_window_view_t *window) {
    uint32_t caption_height = os_window_caption_height(window->flags);
    uint32_t button_width=os_window_caption_button_width(window->flags);
    bool dark = (window->flags & OS_WINDOW_DARK_CAPTION) != 0U;
    if (!window_standard_caption(window->flags) || window->width < 3U*button_width ||
        window->height < caption_height) return;
    bool active = window->identifier == g_compositor_snapshot.focused_identifier;
    /* 服务端按钮区与标题栏共用焦点底色；客户端自绘标题栏仍由应用决定底色。 */
    uint32_t background = dark ? 0x001E1E1EU :
        (!active && !window_client_decorations(window->flags) ?
            OS_THEME_HEADER_INACTIVE : OS_THEME_HEADER);
    /* 单个合成器工作线程只会对不变路径做一次光栅化。后续鼠标重绘不再分配堆内存，也不再执行超采样工作。 */
    static uint8_t icon_alpha[4][12][12];
    static bool icon_alpha_ready;
    if (!icon_alpha_ready) {
        for (uint32_t icon = 1; icon <= 4; ++icon)
            for (uint32_t row = 0; row < 12; ++row)
                for (uint32_t col = 0; col < 12; ++col)
                    icon_alpha[icon - 1U][row][col] = chrome_caption_alpha(icon, col, row);
        icon_alpha_ready = true;
    }
    /* 客户端会绘制相同的标题栏背景。覆盖掉旧的客户端控件。 */
    for (uint32_t button = 1; button <= 3; ++button) {
        int32_t x, y;
        if (!window_caption_button_rect(window->flags, window->x, window->y,
                                        window->width, button, &x, &y)) continue;
        if ((int64_t)x + button_width <= g_compositor_snapshot.damage_bounds.x0 ||
            x >= g_compositor_snapshot.damage_bounds.x1 ||
            (int64_t)y + caption_height <= g_compositor_snapshot.damage_bounds.y0 ||
            y >= g_compositor_snapshot.damage_bounds.y1) continue;
        for (uint32_t row = 0; row < caption_height; ++row) {
            uint32_t cut = window->maximized ? 0U : chrome_frame_row_inset(row,
                window_outer_width(window->width, window->flags),
                window_outer_height(window->height, window->flags));
            uint32_t right_cut = button == 3U ? cut : 0U;
            compositor_fill_locked(x, y + (int32_t)row, button_width - right_cut, 1U, background);
        }
        /* 绘制消费场景路由的悬停快照，不能让被遮挡的同坐标按钮自行亮起。 */
        bool hover = g_compositor_snapshot.hovered_window_identifier == window->identifier &&
            g_compositor_snapshot.hovered_title_button == button;
        bool pressed = g_compositor_snapshot.title_pressed_identifier == window->identifier &&
            g_compositor_snapshot.title_pressed_button == button;
        /* FrameCaptionButton::OnPaint(): 高亮 alpha 为 0x14；波纹为 6%。 */
        if (dark || hover || pressed) {
            /* 所有缩放共用圆形覆盖率，不能在放大后改走矩形高亮。 */
            uint32_t diameter=(dark ? 26U : 28U)*button_width/32U;
            for (uint32_t row=0;row<diameter;++row)
                for (uint32_t col=0;col<diameter;++col) {
                    uint32_t alpha=os_window_caption_circle_alpha(diameter,col,row);
                    if (!alpha) continue;
                    compositor_blend_pixel_locked(
                        x+(int32_t)(button_width-diameter)/2+(int32_t)col,
                        y+(int32_t)(caption_height-diameter)/2+(int32_t)row,
                        dark ? (pressed ? 0x00636363U : hover ? 0x00535353U : 0x00353535U) : 0U,
                        dark ? alpha : (alpha*20U+127U)/255U);
                }
        }
        uint32_t icon = button == 2U && window->maximized ? 4U : button;
        uint32_t opacity = active || hover ? 255U : 97U;
        uint32_t icon_size=12U*button_width/32U;
        for (uint32_t row = 0; row < icon_size; ++row)
            for (uint32_t col = 0; col < icon_size; ++col)
                compositor_blend_pixel_locked(x + (int32_t)(button_width-icon_size)/2 + (int32_t)col,
                    y + (int32_t)(caption_height - icon_size) / 2 + (int32_t)row,
                    dark ? 0x00EEEEEEU : OS_THEME_MUTED,
                    (icon_alpha[icon - 1U][row*12U/icon_size][col*12U/icon_size] * opacity + 127U) / 255U);
    }
    /* Chromium Linux frame_view_utils_linux.cc：1px 内边框为黑色，alpha 为 0x26。
     * 最大化窗口没有恢复状态的边框。 */
    if (!window->maximized && window_client_decorations(window->flags)) {
        /* 平台没有窗口阴影时，RestoredFrameBorderInsets() 会在侧边和底部保留 4 DIP 的
         * 窗框颜色。 */
        compositor_fill_locked(window->x, window->y + (int32_t)caption_height,
            CHROME_FRAME_BORDER_WIDTH, window->height - caption_height, background);
        compositor_fill_locked(window->x + (int32_t)window->width - 4,
            window->y + (int32_t)caption_height, CHROME_FRAME_BORDER_WIDTH,
            window->height - caption_height, background);
        compositor_fill_locked(window->x, window->y + (int32_t)window->height - 4,
            window->width, CHROME_FRAME_BORDER_WIDTH, background);
        int64_t first_row = (int64_t)g_compositor_snapshot.damage_bounds.y0 - window->y;
        int64_t last_row = (int64_t)g_compositor_snapshot.damage_bounds.y1 - window->y;
        if (first_row < 0) first_row = 0;
        if (last_row > window->height) last_row = window->height;
        for (int64_t row = first_row; row < last_row; ++row) {
            uint32_t cut = chrome_frame_row_inset(row, window->width, window->height);
            compositor_blend_pixel_locked(window->x + (int32_t)cut, window->y + (int32_t)row, 0U, 38U);
            compositor_blend_pixel_locked(window->x + (int32_t)window->width - 1 - (int32_t)cut,
                window->y + (int32_t)row, 0U, 38U);
        }
        int64_t first_col = (int64_t)g_compositor_snapshot.damage_bounds.x0 - window->x;
        int64_t last_col = (int64_t)g_compositor_snapshot.damage_bounds.x1 - window->x;
        if (first_col < 0) first_col = 0;
        if (last_col > window->width) last_col = window->width;
        for (int64_t col = first_col; col < last_col; ++col) {
            uint32_t cut = chrome_frame_inset(0U, window->width);
            if (col > cut && col + cut + 1U < window->width)
                compositor_blend_pixel_locked(window->x + (int32_t)col, window->y, 0U, 38U);
            compositor_blend_pixel_locked(window->x + (int32_t)col,
                window->y + (int32_t)window->height - 1, 0U, 38U);
        }
    }
}

void compositor_titlebar_locked(
    const compositor_window_view_t *window,
    uint32_t frame_color) {

    bool focused;

    uint32_t title_color;
    uint32_t separator_color;
    uint32_t text_color;

    uint32_t outer_width;
    uint32_t outer_height;

    int32_t text_x;
    int32_t text_y;

    if (window == 0) {
        return;
    }

    if (window_client_decorations(window->flags)) {
        return;
    }
    focused =
        window->identifier ==
        g_compositor_snapshot.focused_identifier;

    title_color =
        focused ?
            OS_THEME_HEADER :
            OS_THEME_HEADER_INACTIVE;

    separator_color = OS_THEME_BORDER;

    text_color =
        focused ?
            OS_THEME_TEXT :
            OS_THEME_MUTED;

    outer_width =
        window->width +
        window_frame_extra(window->flags);

    outer_height =
        window->height +
        window_frame_extra(window->flags) +
        window_titlebar_height(window->flags);

    /* 绘制完整的平面外框。 */
    compositor_fill_rounded_locked(
        window->x,
        window->y,
        outer_width,
        outer_height,
        WINDOW_CORNER_RADIUS,
        frame_color);

    /* 标题底色完整覆盖内框，四角的内外轮廓由最终遮罩统一处理。
     * 不能为两侧各保留一个半径宽的方块，否则会露出边框底色。 */
    compositor_fill_locked(window->x+(int32_t)window_frame_border(window->flags),
        window->y+(int32_t)window_frame_border(window->flags),
        window->width,window_titlebar_height(window->flags),title_color);

    /* 客户区/标题栏分隔线。 */
    compositor_fill_locked(
        window->x +
            (int32_t)window_frame_border(window->flags),
        window->y +
            (int32_t)window_client_offset_y(window->flags) -
            1,
        window->width,
        1U,
        separator_color);

    if (!window_standard_caption(window->flags))
        compositor_title_controls_locked(window);

    /* 与正文共用字体服务；快照在提交阶段生成，绘制不解析字体或分配内存。 */
    if (window_control_has_caption(window->controls)) return;
    const caption_text_t *text=window->caption;
    uint32_t controls=window_standard_caption(window->flags) ?
        3U*os_window_caption_button_width(window->flags) : WINDOW_TITLE_CONTROLS_WIDTH;
    if (!text || !text->width || window->width<=controls+24U) return;
    uint32_t width=window->width-controls-24U;
    if (width>text->width) width=text->width;
    text_x=window->x+12;
    uint32_t bar=window_titlebar_height(window->flags);
    text_y=window->y+(int32_t)window_frame_border(window->flags)+
        (int32_t)(bar>text->height ? (bar-text->height)/2U : 0U);
    for (uint32_t row=0;row<text->height;++row)
        for (uint32_t col=0;col<width;++col)
            compositor_blend_pixel_locked(text_x+(int32_t)col,text_y+(int32_t)row,
                text_color,text->pixels[(size_t)row*text->width+col]);
}
