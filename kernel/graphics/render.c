/* REFACTOR_P7B_SURFACE_RENDER_OWNER：surface 渲染。 */

#include "internal.h"
#include <kernel/pixel_blend.h>
#include <uapi/window_theme.h>
#include <uapi/chrome_caption.h>

/* 这些步骤仅由本文件调度，不对其他图形模块公开。 */
static void
compositor_surface_locked(Rect clip,
    const compositor_window_view_t *window);
static void
compositor_surface_source_prepare(
    const compositor_window_view_t *window,
    compositor_surface_source_context_t *context);
static void
compositor_copy_surface_span(
    const compositor_window_view_t *window,
    const compositor_surface_source_context_t *surface,
    uint32_t source_row,
    uint32_t first_column,
    uint32_t last_column,
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    uint64_t *cached_page_index,
    uint8_t **cached_base);


/* 客户端内容或服务器装饰接触圆角前先捕获底层像素。包括底边框在内，最终结果只做一次遮罩。
 * 固定的 256 个采样点使矩形内部仍可走批量复制快速路径。 */
static void compositor_window_corners(Rect clip, const compositor_window_view_t *window,
    uint32_t width, uint32_t height, uint32_t background[256], bool restore) {
    /* 装饰类型和主题在本帧不变，捕获底层时也无需解析边框颜色。 */
    const bool framed = !window_client_decorations(window->flags);
    const uint32_t frame = restore && framed ?
        os_theme_resolve(OS_THEME_BORDER, g_compositor_snapshot.theme) : 0U;
    for (uint32_t corner = 0U; corner < 4U; ++corner) {
        int64_t left = (int64_t)window->x + ((corner & 1U) ? width - 8U : 0U);
        int64_t top = (int64_t)window->y + ((corner & 2U) ? height - 8U : 0U);
        Rect visible;
        /* 每个圆角只裁剪一次；行偏移共用，像素循环无需重复检查屏幕和损坏边界。 */
        if (!rect_clip_to_extent(left > clip.x0 ? left : clip.x0,
                top > clip.y0 ? top : clip.y0,
                left + 8 < clip.x1 ? left + 8 : clip.x1,
                top + 8 < clip.y1 ? top + 8 : clip.y1,
                g_window_server.display_width, g_window_server.display_height,
                &visible)) continue;
        for (int32_t py = visible.y0; py < visible.y1; ++py) {
            uint32_t y = (uint32_t)((int64_t)py - top);
            if ((corner & 2U) != 0U) y = 7U - y;
            uint64_t row = (uint64_t)py * g_window_server.display_stride;
            for (int32_t px = visible.x0; px < visible.x1; ++px) {
                uint32_t x = (uint32_t)((int64_t)px - left);
                if ((corner & 1U) != 0U) x = 7U - x;
                uint32_t alpha = chrome_corner_coverage[y][x];
                uint32_t inner=framed?chrome_corner_inner_coverage[y][x]:alpha;
                if (alpha == 255U && inner == 255U) continue;
                uint64_t offset = row + (uint32_t)px;
                uint32_t i = corner * 64U + y * 8U + x;
                if (!restore) { background[i] = g_window_server.composite_framebuffer[offset]; continue; }
                uint32_t source = g_window_server.composite_framebuffer[offset], under = background[i];
                uint32_t inverse = 255U - alpha;
                uint32_t border=alpha-inner;
                /* 内部、边环和底层一次混合，避免外圈被提前裁剪或重复混合。 */
                uint32_t r = (((source >> 16U) & 255U) * inner + ((frame >> 16U) & 255U) * border + ((under >> 16U) & 255U) * inverse + 127U) / 255U;
                uint32_t g = (((source >> 8U) & 255U) * inner + ((frame >> 8U) & 255U) * border + ((under >> 8U) & 255U) * inverse + 127U) / 255U;
                uint32_t b = ((source & 255U) * inner + (frame & 255U) * border + (under & 255U) * inverse + 127U) / 255U;
                g_window_server.composite_framebuffer[offset] = (under & 0xFF000000U) | (r << 16U) | (g << 8U) | b;
            }
        }
    }
}

void compositor_draw_window_locked(Rect clip,
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
    if (rounded) compositor_window_corners(clip, window, width, height, corner_background, false);

    if (!window_client_decorations(window->flags) &&
        !compositor_damage_inside_surface_interior(clip, window)) {
        compositor_titlebar_locked(clip,
            window,
            frame_color);
    }

    if (window->controls) compositor_control_tree(clip, window);
    else compositor_surface_locked(clip, window);
    if (window_standard_caption(window->flags) && !window_control_has_caption(window->controls))
        compositor_standard_caption_locked(clip, window);
    if (rounded) compositor_window_corners(clip, window, width, height, corner_background, true);
}

typedef struct compositor_surface_job {
    const compositor_window_view_t *window;
    compositor_surface_source_context_t source;
    Rect bounds;
    int64_t surface_x, surface_y;
    int64_t frame_width, frame_height;
    bool frame_corners;
} compositor_surface_job_t;

static void compositor_surface_rows(void *context, uint32_t top, uint32_t bottom) {
    const compositor_surface_job_t *job = context;
    const compositor_window_view_t *window = job->window;
    /* 分派前已裁剪至客户区与屏幕交集，工作线程只需换算自己的行区间。 */
    const int64_t surface_x = job->surface_x;
    const int64_t surface_y = job->surface_y;
    const int64_t frame_width = job->frame_width;
    const int64_t frame_height = job->frame_height;
    const int64_t first_row = (int64_t)top - surface_y;
    const int64_t last_row = (int64_t)bottom - surface_y;
    const int64_t base_first_column = (int64_t)job->bounds.x0 - surface_x;
    const int64_t base_last_column = (int64_t)job->bounds.x1 - surface_x;
    /* 装饰类型在快照内不变；每个任务只判断一次，Chrome 圆角也不再先算普通圆角。 */
    const bool clip_frame = job->frame_corners && !window->maximized;
    const bool chrome_corners = clip_frame &&
        window_client_decorations(window->flags) && window_standard_caption(window->flags);
    compositor_surface_source_context_t source_context;
    uint64_t cached_page_index = UINT64_MAX;
    uint8_t *cached_base = 0;
    bool private_wb;
    uint32_t *private_scene;

    source_context = job->source;

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

        inset = 0U;
        if (chrome_corners)
            inset = chrome_frame_row_inset((uint32_t)frame_row, window->width, window->height);
        else if (clip_frame && frame_row >= 0 && frame_row < frame_height)
            inset = compositor_corner_inset((uint32_t)frame_row,
                (uint32_t)frame_width, (uint32_t)frame_height);

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
            row,
            (uint32_t)first_column,
            (uint32_t)last_column,
            wb_destination,
            device_destination,
            &cached_page_index,
            &cached_base);
    }
}

/* 普通客户区和控件 Surface 共用复制器；只有整窗客户区需要窗口圆角。 */
void compositor_surface_region_locked(const compositor_window_view_t *window,
    int64_t left, int64_t top, Rect damage, bool frame_corners) {
    if (window == 0) return;
    compositor_surface_job_t job = {.window = window, .frame_corners = frame_corners};
    if (!rect_clip_to_extent(damage.x0, damage.y0, damage.x1, damage.y1,
            g_window_server.display_width, g_window_server.display_height,
            &job.bounds)) return;
    /* 只分派客户区实际覆盖的行，避免标题栏和窗口外的空任务。 */
    Rect client;
    if (!rect_clip_to_extent(left, top, left + window->width, top + window->height,
            g_window_server.display_width, g_window_server.display_height, &client)) return;
    if (client.x0 > job.bounds.x0) job.bounds.x0 = client.x0;
    if (client.y0 > job.bounds.y0) job.bounds.y0 = client.y0;
    if (client.x1 < job.bounds.x1) job.bounds.x1 = client.x1;
    if (client.y1 < job.bounds.y1) job.bounds.y1 = client.y1;
    if (rect_is_empty(job.bounds)) return;
    job.surface_x = left;
    job.surface_y = top;
    job.frame_width = (int64_t)window->width + window_frame_extra(window->flags);
    job.frame_height = (int64_t)window->height + window_frame_extra(window->flags) +
        window_titlebar_height(window->flags);
    compositor_surface_source_prepare(window, &job.source);
    if (g_window_server.composite_framebuffer != g_window_server.framebuffer) {
        compositor_parallel_rows(job.bounds.y0, job.bounds.y1,
            job.bounds.x1 - job.bounds.x0, compositor_surface_rows, &job);
    } else {
        compositor_surface_rows(&job, job.bounds.y0, job.bounds.y1);
    }
}

static void compositor_surface_locked(Rect clip, const compositor_window_view_t *window) {
    if (!window) return;
    compositor_surface_region_locked(window,
        (int64_t)window->x + window_frame_border(window->flags),
        (int64_t)window->y + window_client_offset_y(window->flags),
        clip, true);
}

static void compositor_surface_source_prepare(
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
            result = pixel_blend_premultiplied(background, foreground);
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

static void compositor_copy_surface_span(
    const compositor_window_view_t *window,
    const compositor_surface_source_context_t *surface,
    uint32_t source_row,
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
        if (!surface->row_bytes || source_row >= window->surface_height ||
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

    /* 调用方直接提供行号，避免先算偏移、再逐行做除法还原行号。
     * 极端跨度仍通过溢出检查退回背景填充，不能访问其他表面槽位。 */
    uint64_t source_row_offset;
    if (__builtin_mul_overflow((uint64_t)source_row, surface->row_bytes,
                               &source_row_offset)) {
        compositor_window_fill_span(window, wb_destination,
            device_destination, remaining);
        return;
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
            uint8_t *first_base = *cached_base;
            uintptr_t next_address = (uintptr_t)first_base;
            uint64_t next_index = page_index;
            /* 相邻直接映射页确认连续后合并复制，避免每跨 4 KiB 都重新调用跨度复制器。
             * 只查询本次实际需要的页；缺失或不连续的页仍交给下一轮独立处理。 */
            while (chunk < remaining && next_address <= UINTPTR_MAX - PAGE_SIZE) {
                next_address += PAGE_SIZE;
                ++next_index;
                uint8_t *next = compositor_surface_page_resolve(surface, next_index);
                *cached_page_index = next_index;
                *cached_base = next;
                if (next == 0 || (uintptr_t)next != next_address) break;
                uint32_t extra = remaining - chunk;
                if (extra > PAGE_SIZE / sizeof(uint32_t))
                    extra = PAGE_SIZE / sizeof(uint32_t);
                chunk += extra;
            }
            const uint32_t *source =
                (const uint32_t *)(const void *)(first_base + in_page);
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

