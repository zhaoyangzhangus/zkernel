/* REFACTOR_P7B_SURFACE_RENDER_OWNER：surface 渲染。 */

#include "internal.h"
#include <uapi/window_theme.h>
#include <uapi/chrome_caption.h>

static bool compositor_corner_pixel(const compositor_window_view_t *window,
    uint32_t width, uint32_t height, uint32_t corner, uint32_t x, uint32_t y,
    uint64_t *offset) {
    int64_t px = (int64_t)window->x + ((corner & 1U) ? width - 1U - x : x);
    int64_t py = (int64_t)window->y + ((corner & 2U) ? height - 1U - y : y);
    if (px < 0 || py < 0 || px >= g_window_server.display_width ||
        py >= g_window_server.display_height ||
        px < g_compositor_snapshot.damage_bounds.x0 || px >= g_compositor_snapshot.damage_bounds.x1 ||
        py < g_compositor_snapshot.damage_bounds.y0 || py >= g_compositor_snapshot.damage_bounds.y1) return false;
    *offset = (uint64_t)py * g_window_server.display_stride + (uint32_t)px;
    return true;
}

/* 客户端内容或服务器装饰接触圆角前先捕获底层像素。包括底边框在内，最终结果只做一次遮罩。
 * 固定的 256 个采样点使矩形内部仍可走批量复制快速路径。 */
static void compositor_window_corners(const compositor_window_view_t *window,
    uint32_t width, uint32_t height, uint32_t background[256], bool restore) {
    for (uint32_t corner = 0U; corner < 4U; ++corner) {
        int64_t left = (int64_t)window->x + ((corner & 1U) ? width - 8U : 0U);
        int64_t top = (int64_t)window->y + ((corner & 2U) ? height - 8U : 0U);
        if (left + 8 <= g_compositor_snapshot.damage_bounds.x0 ||
            left >= g_compositor_snapshot.damage_bounds.x1 ||
            top + 8 <= g_compositor_snapshot.damage_bounds.y0 ||
            top >= g_compositor_snapshot.damage_bounds.y1 ||
            left + 8 <= 0 || top + 8 <= 0 ||
            left >= g_window_server.display_width ||
            top >= g_window_server.display_height) continue;
        for (uint32_t y = 0U; y < 8U; ++y)
            for (uint32_t x = 0U; x < 8U; ++x) {
                uint32_t alpha = chrome_corner_coverage[y][x];
                bool framed=!window_client_decorations(window->flags);
                uint32_t inner=framed?chrome_corner_inner_coverage[y][x]:alpha;
                uint64_t offset;
                if ((alpha == 255U && inner == 255U) || !compositor_corner_pixel(window, width, height,
                    corner, x, y, &offset)) continue;
                uint32_t i = corner * 64U + y * 8U + x;
                if (!restore) { background[i] = g_window_server.composite_framebuffer[offset]; continue; }
                uint32_t source = g_window_server.composite_framebuffer[offset], under = background[i];
                uint32_t inverse = 255U - alpha;
                uint32_t border=alpha-inner;
                uint32_t frame=os_theme_resolve(OS_THEME_BORDER,g_compositor_snapshot.theme);
                /* 内部、边环和底层一次混合，避免外圈被提前裁剪或重复混合。 */
                uint32_t r = (((source >> 16U) & 255U) * inner + ((frame >> 16U) & 255U) * border + ((under >> 16U) & 255U) * inverse + 127U) / 255U;
                uint32_t g = (((source >> 8U) & 255U) * inner + ((frame >> 8U) & 255U) * border + ((under >> 8U) & 255U) * inverse + 127U) / 255U;
                uint32_t b = ((source & 255U) * inner + (frame & 255U) * border + (under & 255U) * inverse + 127U) / 255U;
                g_window_server.composite_framebuffer[offset] = (under & 0xFF000000U) | (r << 16U) | (g << 8U) | b;
            }
    }
}

void compositor_draw_window_locked(
    const compositor_window_view_t *window) {

    uint32_t frame_color;
    uint32_t corner_background[256];

    if (window == 0) {
        return;
    }

    frame_color = OS_THEME_BORDER;
    uint32_t width = window_outer_width(window->width, window->flags);
    uint32_t height = window_outer_height(window->height, window->flags);
    bool rounded = !window->maximized && width >= 16U && height >= 16U &&
                   g_window_server.composite_framebuffer != 0;
    if (rounded) compositor_window_corners(window, width, height, corner_background, false);

    if (!window_client_decorations(window->flags) &&
        !compositor_damage_inside_surface_interior(window)) {
        compositor_titlebar_locked(
            window,
            frame_color);
    }

    if (window->controls) compositor_control_tree(window);
    else compositor_surface_locked(window);
    if (window_standard_caption(window->flags) && !window_control_has_caption(window->controls))
        compositor_standard_caption_locked(window);
    if (rounded) compositor_window_corners(window, width, height, corner_background, true);
}

void compositor_surface_locked(
    const compositor_window_view_t *window) {

    compositor_surface_source_context_t source_context;
    uint64_t cached_page_index = UINT64_MAX;
    uint8_t *cached_base = 0;
    uint64_t source_row_offset;
    bool private_wb;
    uint32_t *private_scene;

    int64_t frame_width;
    int64_t frame_height;
    int64_t surface_x;
    int64_t surface_y;
    int64_t first_row;
    int64_t last_row;
    int64_t base_first_column;
    int64_t base_last_column;

    if (window == 0) {
        return;
    }

    frame_width =
        (int64_t)window->width +
        window_frame_extra(window->flags);

    frame_height =
        (int64_t)window->height +
        window_frame_extra(window->flags) +
        window_titlebar_height(window->flags);

    surface_x =
        (int64_t)window->x +
        window_frame_border(window->flags);

    surface_y =
        (int64_t)window->y +
        window_client_offset_y(window->flags);

    /*
    * 仅计算一次显示范围与损坏区域的交集。
     *
    * 旧路径会针对每个损坏矩形遍历 row=0..window->height，再在循环内排除无关行。因此，
    * 高度 1000 像素的窗口即使只更新 20 像素，也仍会执行约 1000 次行检查。
     */
    first_row = 0;
    last_row = window->height;

    if (surface_y + first_row < 0) {
        first_row = -surface_y;
    }

    if (surface_y + last_row >
        (int64_t)g_window_server.display_height) {
        last_row =
            (int64_t)g_window_server.display_height -
            surface_y;
    }

    if (surface_y + first_row <
        (int64_t)g_compositor_snapshot.damage_bounds.y0) {
        first_row =
            (int64_t)g_compositor_snapshot.damage_bounds.y0 -
            surface_y;
    }

    if (surface_y + last_row >
        (int64_t)g_compositor_snapshot.damage_bounds.y1) {
        last_row =
            (int64_t)g_compositor_snapshot.damage_bounds.y1 -
            surface_y;
    }

    if (first_row < 0) {
        first_row = 0;
    }

    if (last_row > (int64_t)window->height) {
        last_row = window->height;
    }

    if (first_row >= last_row) {
        return;
    }

    base_first_column = 0;
    base_last_column = window->width;

    if (surface_x + base_first_column < 0) {
        base_first_column = -surface_x;
    }

    if (surface_x + base_last_column >
        (int64_t)g_window_server.display_width) {
        base_last_column =
            (int64_t)g_window_server.display_width -
            surface_x;
    }

    if (surface_x + base_first_column <
        (int64_t)g_compositor_snapshot.damage_bounds.x0) {
        base_first_column =
            (int64_t)g_compositor_snapshot.damage_bounds.x0 -
            surface_x;
    }

    if (surface_x + base_last_column >
        (int64_t)g_compositor_snapshot.damage_bounds.x1) {
        base_last_column =
            (int64_t)g_compositor_snapshot.damage_bounds.x1 -
            surface_x;
    }

    if (base_first_column < 0) {
        base_first_column = 0;
    }

    if (base_last_column > (int64_t)window->width) {
        base_last_column = window->width;
    }

    if (base_first_column >= base_last_column) {
        return;
    }

    compositor_surface_source_prepare(window, &source_context);

    source_row_offset =
        (uint64_t)(uint32_t)first_row *
        source_context.row_bytes;

    private_wb =
        g_window_server.composite_framebuffer != 0 &&
        g_window_server.composite_framebuffer !=
            g_window_server.framebuffer;

    private_scene =
        private_wb ?
            (uint32_t *)(uintptr_t)
                g_window_server.composite_framebuffer :
            0;

    for (uint32_t row = (uint32_t)first_row;
         row < (uint32_t)last_row;
         ++row) {

        uint64_t row_source_offset =
            source_row_offset;

        source_row_offset +=
            source_context.row_bytes;

        int64_t destination_y =
            surface_y + row;

        int64_t frame_row =
            destination_y -
            window->y;

        int64_t first_column =
            base_first_column;

        int64_t last_column =
            base_last_column;

        uint32_t inset;
        uint64_t destination_index;
        uint32_t *wb_destination;
        volatile uint32_t *device_destination;

        inset = window->maximized ? 0U : (frame_row >= 0 && frame_row < frame_height ?
                 compositor_corner_inset(
                     (uint32_t)frame_row,
                     (uint32_t)frame_width,
                     (uint32_t)frame_height) :
                 0U);
        if (!window->maximized && window_client_decorations(window->flags) &&
            window_standard_caption(window->flags))
            inset = chrome_frame_row_inset((uint32_t)frame_row, window->width, window->height);

        /*
         * 保留圆角窗口周围一个像素的装饰边框。这是热循环中唯一与行相关的裁剪操作。
         */
        if (first_column <
            (int64_t)inset) {
            first_column = inset;
        }

        if (last_column >
            (int64_t)window->width -
                inset) {
            last_column =
                (int64_t)window->width -
                inset;
        }

        if (first_column >= last_column) {
            continue;
        }

        destination_index =
            (uint64_t)
                (uint32_t)destination_y *
                g_window_server.display_stride +

            (uint32_t)(
                surface_x +
                first_column);

        if (private_wb) {
            wb_destination =
                private_scene + destination_index;
            device_destination = 0;
        } else {
            wb_destination = 0;
            device_destination =
                g_window_server.composite_framebuffer +
                destination_index;
        }

        compositor_copy_surface_span(
            window,
            &source_context,
            row_source_offset,
            (uint32_t)first_column,
            (uint32_t)last_column,
            wb_destination,
            device_destination,
            &cached_page_index,
            &cached_base);
    }
}

void compositor_surface_source_prepare(
    const compositor_window_view_t *window,
    compositor_surface_source_context_t *context) {

    vm_object_t *object;

    if (context == 0) return;
    *context = (compositor_surface_source_context_t){0};
    if (window == 0) return;

    context->row_bytes =
        (uint64_t)(window->surface_lease ? window->surface_width : window->width) * sizeof(uint32_t);

    /* 交换模式固定的是上一张完整帧及其行跨度，缩放期间不能把它替换成空白。
     * 单缓冲仍不能读取：生产者可能正在按新尺寸覆盖同一块内存。 */
    if ((window->resize_pending && !window->surface_lease) ||
        (window->surface_lease && (!window->surface_width || !window->surface_height)) ||
        window->section == 0 ||
        window->section->vm_object == 0 ||
        window->owner_address == 0U) {
        return;
    }

    object = window->section->vm_object;
    if (object->type != VM_OBJECT_SHARED) return;

    context->object = object;
    context->cache = compositor_surface_cache_get(window);
    context->readable = true;
}

/* 以 source-over 方式绘制到已合成的下层窗口上。Skia N32Premul 存储的是预乘分量，再次将
 * 源颜色乘以 alpha 会使抗锯齿边缘变暗。Alpha 为零时不得修改目标。 */
static void compositor_surface_blend_span(
    uint32_t *wb_destination, volatile uint32_t *device_destination,
    const uint32_t *source, uint32_t color, uint32_t pixels) {
    /* 常量背景只判定一次 alpha，透明时不读写目标，不透明时批量填充。 */
    if (source == 0) {
        if ((color >> 24U) == 0U) return;
        if ((color >> 24U) == 255U) {
            compositor_surface_fill_destination(wb_destination, device_destination, pixels, color);
            return;
        }
    }
    for (uint32_t index = 0U; index < pixels; ++index) {
        uint32_t foreground = source != 0 ? source[index] : color;
        uint32_t alpha = foreground >> 24U;
        if (alpha == 0U) continue;
        /* 即使大面积不透明区域，Chrome 也使用预乘表面。边缘保留 alpha；不透明区域则使用批量
         * 复制器处理，而不是逐像素存储。 */
        if (alpha == 255U && source != 0 && pixels - index >= 16U) {
            uint32_t end = index + 1U;
            /* 每次检查两个像素，只在剩余长度足够时读取；透明边缘仍交给标量混合。 */
            while (pixels - end >= 2U) {
                uint64_t pair;
                __builtin_memcpy(&pair, source + end, sizeof(pair));
                if ((pair & 0xff000000ff000000ULL) != 0xff000000ff000000ULL) break;
                end += 2U;
            }
            while (end < pixels && (source[end] >> 24U) == 255U) ++end;
            if (end - index >= 16U) {
                compositor_surface_copy_destination(
                    wb_destination != 0 ? wb_destination + index : 0,
                    device_destination != 0 ? device_destination + index : 0,
                    source + index, end - index);
            } else {
                /* 短段也只探测一次，避免后续像素反复扫描同一段。 */
                for (uint32_t i = index; i < end; ++i) {
                    if (wb_destination != 0) wb_destination[i] = source[i];
                    else device_destination[i] = source[i];
                }
            }
            index = end - 1U;
            continue;
        }
        uint32_t result = foreground;
        if (alpha != 255U) {
            uint32_t background = wb_destination != 0 ?
                wb_destination[index] : device_destination[index];
            uint32_t inverse = 255U - alpha;
            result = 0U;
            for (uint32_t shift = 0U; shift < 24U; shift += 8U) {
                uint32_t component = ((foreground >> shift) & 255U) +
                    (((background >> shift) & 255U) * inverse + 127U) / 255U;
                if (component > 255U) component = 255U;
                result |= component << shift;
            }
        }
        if (wb_destination != 0) wb_destination[index] = result;
        else device_destination[index] = result;
    }
}

static void compositor_window_fill_span(
    const compositor_window_view_t *window, uint32_t *wb_destination,
    volatile uint32_t *device_destination, uint32_t pixels) {
    if ((window->flags & OS_WINDOW_PREMULTIPLIED_ALPHA) != 0U) {
        compositor_surface_blend_span(wb_destination, device_destination,
                                      0, window->background, pixels);
    } else {
        compositor_surface_fill_destination(wb_destination, device_destination,
                                            pixels, window->background);
    }
}

void compositor_copy_surface_span(
    const compositor_window_view_t *window,
    const compositor_surface_source_context_t *surface,
    uint64_t source_row_offset,
    uint32_t first_column,
    uint32_t last_column,
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    uint64_t *cached_page_index,
    uint8_t **cached_base) {

    uint64_t source_offset;
    uint64_t column_offset;
    uint32_t remaining;

    if (window == 0 || surface == 0 ||
        (wb_destination == 0 && device_destination == 0) ||
        cached_page_index == 0 || cached_base == 0 ||
        first_column >= last_column) {
        return;
    }

    remaining = last_column - first_column;

    if (!surface->readable) {
        compositor_window_fill_span(window,
            wb_destination, device_destination,
            remaining);
        return;
    }

    /* 新窗口可比旧帧更大。只复制旧帧有效的行列，新增区域填背景，不能跨行或越槽读取。 */
    if (window->surface_lease) {
        if (!surface->row_bytes || source_row_offset / surface->row_bytes >= window->surface_height ||
            first_column >= window->surface_width) {
            compositor_window_fill_span(window, wb_destination, device_destination, remaining);
            return;
        }
        uint32_t available = window->surface_width - first_column;
        if (available < remaining) {
            compositor_window_fill_span(window,
                wb_destination ? wb_destination + available : 0,
                device_destination ? device_destination + available : 0, remaining-available);
            remaining = available;
        }
    }

    column_offset =
        (uint64_t)first_column * sizeof(uint32_t);

    if (source_row_offset > UINT64_MAX - column_offset) {
        compositor_window_fill_span(window,
            wb_destination, device_destination,
            remaining);
        return;
    }

    source_offset = source_row_offset + column_offset;

    if (source_offset >= window->buffer_size) {
        compositor_window_fill_span(window,
            wb_destination, device_destination,
            remaining);
        return;
    }

    {
        uint64_t available_bytes =
            window->buffer_size - source_offset;
        uint64_t requested_bytes =
            (uint64_t)remaining * sizeof(uint32_t);

        if (requested_bytes > available_bytes) {
            uint32_t available_pixels =
                (uint32_t)(available_bytes / sizeof(uint32_t));

            if (available_pixels < remaining) {
                compositor_window_fill_span(window,
                    wb_destination != 0 ? wb_destination + available_pixels : 0,
                    device_destination != 0 ? device_destination + available_pixels : 0,
                    remaining - available_pixels);
                remaining = available_pixels;
            }
        }
    }

    if (window->surface_offset > UINT64_MAX - source_offset) {
        compositor_window_fill_span(window, wb_destination,
            device_destination, remaining);
        return;
    }
    source_offset += window->surface_offset;

    while (remaining != 0U) {
        uint64_t page_index = source_offset >> PAGE_SHIFT;
        uint32_t in_page =
            (uint32_t)(source_offset & (PAGE_SIZE - 1U));
        uint32_t page_pixels =
            (PAGE_SIZE - in_page) / sizeof(uint32_t);
        uint32_t chunk =
            remaining < page_pixels ? remaining : page_pixels;

        if (*cached_page_index != page_index) {
            *cached_base =
                compositor_surface_page_resolve(surface, page_index);
            *cached_page_index = page_index;
        }

        if (*cached_base != 0) {
            const uint32_t *source =
                (const uint32_t *)(const void *)(*cached_base + in_page);
            if ((window->flags & OS_WINDOW_PREMULTIPLIED_ALPHA) != 0U) {
                compositor_surface_blend_span(wb_destination, device_destination,
                                              source, 0U, chunk);
            } else {
                compositor_surface_copy_destination(
                    wb_destination, device_destination, source, chunk);
            }
        } else {
            compositor_window_fill_span(window,
                wb_destination, device_destination,
                chunk);
        }

        if (wb_destination != 0) {
            wb_destination += chunk;
        } else {
            device_destination += chunk;
        }

        source_offset += (uint64_t)chunk * sizeof(uint32_t);
        remaining -= chunk;
    }
}

