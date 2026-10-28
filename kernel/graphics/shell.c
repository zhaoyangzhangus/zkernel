#include <kernel/kmem.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include <kernel/desktop_icon.h>
#include <kernel/control_tree.h>
#include <kernel/control_raster.h>
#include <uapi/window_theme.h>
#include "internal.h"

/* REFACTOR_P7A_SHELL_OWNER：桌面几何布局和交互策略。 */

#define DESKTOP_ICON_IMAGE_WIDTH    64U
#define DESKTOP_ICON_IMAGE_HEIGHT   64U

typedef struct desktop_render_context {
    Rect bounds;
    uint32_t hovered_app;
    volatile uint32_t *target;
    uint32_t target_width;
    uint32_t target_height;
    uint32_t target_stride;
    bool wallpaper_only;
    const desktop_asset_image_t *wallpaper;
    uint32_t running_mask, active_mask;
} desktop_render_context_t;

/* 桌面与应用使用同一棵只读 UI 树；几何、命中和损坏区域不再各算一遍。 */
static Rect desktop_viewport(void) {
    if (g_window_server.display_width > INT32_MAX || g_window_server.display_height > INT32_MAX)
        return (Rect){0};
    return (Rect){0, 0, (int32_t)g_window_server.display_width, (int32_t)g_window_server.display_height};
}

bool desktop_base_icon_box_locked(uint32_t app, control_tree_box_t *box) {
    if (!box || !desktop_shell_assets_available() || !g_window_server.scene.controls || !app ||
        app >= sizeof(g_desktop_programs) / sizeof(g_desktop_programs[0])) return false;
    uint32_t index = g_desktop_programs[app].node_index;
    if (index >= g_window_server.scene.control_count) return false;
    uint32_t found = control_tree_find(g_window_server.scene.controls, g_window_server.scene.control_count, desktop_viewport(),
        g_window_server.scene.controls[index].identifier, box);
    return found == index && box->visible && !rect_is_empty(box->bounds);
}

bool desktop_icon_box_locked(uint32_t app, control_tree_box_t *box) {
    if (!desktop_base_icon_box_locked(app, box)) return false;
    desktop_place_box(&g_window_server.desktop, app, desktop_viewport(), box);
    return true;
}

static void desktop_blend_asset_pixel_locked(
    const desktop_render_context_t *context,
    int32_t x,
    int32_t y,
    uint32_t pixel) {
    volatile uint32_t *target;
    uint32_t target_width;
    uint32_t target_height;
    uint32_t target_stride;
    uint64_t offset;

    if (context == 0) {
        return;
    }

    target = context->target != 0 ?
        context->target : g_window_server.composite_framebuffer;
    target_width = context->target_width != 0U ?
        context->target_width : g_window_server.display_width;
    target_height = context->target_height != 0U ?
        context->target_height : g_window_server.display_height;
    target_stride = context->target_stride != 0U ?
        context->target_stride : g_window_server.display_stride;

    if (target == 0 ||
        x < (int32_t)context->bounds.x0 ||
        y < (int32_t)context->bounds.y0 ||
        x >= (int32_t)context->bounds.x1 ||
        y >= (int32_t)context->bounds.y1 ||
        x < 0 || y < 0 ||
        x >= (int32_t)target_width ||
        y >= (int32_t)target_height ||
        target_stride < target_width) {
        return;
    }

    offset = (uint64_t)(uint32_t)y * target_stride +
             (uint32_t)x;
    target[offset] =
        control_blend_argb(
            target[offset],
            pixel);
}

bool desktop_alpha_self_test(void) {
    const uint64_t blended_pixels = 64U * 64U;
    uint64_t benchmark_start = telemetry_timestamp();
    uint32_t result = 0x00204060U;

    for (uint64_t index = 0U; index < blended_pixels; ++index) {
        result = control_blend_argb(result, 0x80FF0000U);
    }

    kernel_perf_emit_scope("graphics.alpha_composition", benchmark_start);
    kernel_perf_emit_value("graphics.pixels_blended_frame", blended_pixels);
    return control_blend_argb(0x00112233U, 0x00000000U) == 0x00112233U &&
           control_blend_argb(0x00112233U, 0xFFFFFFFFU) == 0x00FFFFFFU &&
           control_blend_argb(0x00204060U, 0x80FF0000U) == 0x00902030U &&
           result != 0U;
}

/* 桌面缓存和最终合成目标共用相同跨度接口，不把圆角算法复制到桌面。 */
static void desktop_hover_span(void *raw, int64_t x, int64_t y,
    uint32_t width, uint32_t height, uint32_t color) {
    const control_raster_target_t *target = raw;
    control_raster_fill_alpha(target, x, y, width, height, 0U, color);
}

static void desktop_draw_hover_frame_locked(int32_t x, int32_t y,
    uint32_t width, uint32_t height, const desktop_render_context_t *context,
    const control_tree_style_t *style) {
    if (!context || !style) return;
    uint32_t target_width = context->target_width ? context->target_width : g_window_server.display_width;
    uint32_t target_height = context->target_height ? context->target_height : g_window_server.display_height;
    uint32_t stride = context->target_stride ? context->target_stride : g_window_server.display_stride;
    Rect clip;
    if (stride < target_width || !rect_clip_to_extent(context->bounds.x0, context->bounds.y0,
        context->bounds.x1, context->bounds.y1, target_width, target_height, &clip)) return;
    control_raster_target_t target = {context->target ? context->target :
        g_window_server.composite_framebuffer, stride, clip};
    control_raster_border_spans(clip, x, y, width, height, style->radius,
        style->border_width, style->border_color, desktop_hover_span, &target);
}

/* 遍历像素前先求一次交集，构建私有缓存时也如此。 */
typedef struct desktop_icon_clip {
    uint32_t x0, y0, x1, y1;
} desktop_icon_clip_t;

static bool desktop_icon_clip_locked(const desktop_render_context_t *context,
    int32_t x, int32_t y, uint32_t width, uint32_t height, desktop_icon_clip_t *out) {
    if (context == 0 || out == 0 || width == 0U || height == 0U) return false;
    uint32_t target_width = context->target_width ? context->target_width :
                           g_window_server.display_width;
    uint32_t target_height = context->target_height ? context->target_height :
                            g_window_server.display_height;
    int64_t right = context->bounds.x1 < (int64_t)target_width ? context->bounds.x1 : (int64_t)target_width;
    int64_t bottom = context->bounds.y1 < (int64_t)target_height ? context->bounds.y1 : (int64_t)target_height;
    int64_t left = context->bounds.x0, top = context->bounds.y0;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    int64_t end_x = (int64_t)x + width, end_y = (int64_t)y + height;
    if (left < x) left = x;
    if (top < y) top = y;
    if (end_x > right) end_x = right;
    if (end_y > bottom) end_y = bottom;
    if (left >= end_x || top >= end_y) return false;
    *out = (desktop_icon_clip_t){ (uint32_t)(left - x), (uint32_t)(top - y),
                   (uint32_t)(end_x - x), (uint32_t)(end_y - y) };
    return true;
}

static void desktop_draw_cached_icon_locked(const desktop_render_context_t *context,
    int32_t x, int32_t y, uint32_t width, const desktop_icon_clip_t *clip, const uint32_t *pixels) {
    for (uint32_t row = clip->y0; row < clip->y1; ++row)
        for (uint32_t column = clip->x0; column < clip->x1; ++column)
            desktop_blend_asset_pixel_locked(context, x + (int32_t)column,
                y + (int32_t)row, pixels[(uint64_t)row * width + column]);
}

static void desktop_draw_asset_icon_locked(
    const desktop_render_context_t *context,
    int32_t x, int32_t y, uint32_t width, uint32_t height,
    const desktop_asset_image_t *asset,
    uint32_t source_left, uint32_t source_top,
    uint32_t source_width, uint32_t source_height) {
    desktop_icon_clip_t clip;
    if (asset == 0 || asset->pixels == 0 ||
        source_width == 0U || source_height == 0U ||
        source_left >= asset->width || source_top >= asset->height ||
        source_width > asset->width - source_left ||
        source_height > asset->height - source_top ||
        !desktop_icon_clip_locked(context, x, y, width, height, &clip)) {
        return;
    }

    /* 不可变图稿只过滤一次，不在每次悬停或场景修复时重复处理。
     * 缓存槽与应用包数量一致，新增桌面入口不再漏掉图标缓存。 */
    static struct {
        const uint8_t *source;
        uint32_t left, top, width, height;
        uint32_t pixels[DESKTOP_ICON_IMAGE_WIDTH * DESKTOP_ICON_IMAGE_HEIGHT];
    } cache[sizeof(g_desktop_app_assets)/sizeof(g_desktop_app_assets[0])-1U];
    uint32_t *filtered = 0;
    if (width == DESKTOP_ICON_IMAGE_WIDTH && height == DESKTOP_ICON_IMAGE_HEIGHT) {
        for (uint32_t i = 0U; i < sizeof(cache)/sizeof(cache[0]); ++i) {
            if (cache[i].source == asset->pixels && cache[i].left == source_left &&
                cache[i].top == source_top && cache[i].width == source_width &&
                cache[i].height == source_height) {
                filtered = cache[i].pixels;
                break;
            }
            if (cache[i].source != 0) continue;
            for (uint32_t row = 0U; row < height; ++row)
                for (uint32_t column = 0U; column < width; ++column)
                    cache[i].pixels[row * width + column] = desktop_icon_area_pixel(
                        asset->pixels, asset->stride, source_left, source_top,
                        source_width, source_height, width, height, column, row);
            cache[i].source = asset->pixels;
            cache[i].left = source_left; cache[i].top = source_top;
            cache[i].width = source_width; cache[i].height = source_height;
            filtered = cache[i].pixels;
            break;
        }
    }
    if (filtered != 0) {
        desktop_draw_cached_icon_locked(context, x, y, width, &clip, filtered);
        return;
    }
    for (uint32_t row = clip.y0; row < clip.y1; ++row) {
        for (uint32_t column = clip.x0; column < clip.x1; ++column) {
            uint32_t pixel = desktop_icon_area_pixel(asset->pixels, asset->stride,
                    source_left, source_top, source_width, source_height,
                    width, height, column, row);
            desktop_blend_asset_pixel_locked(
                context,
                x + (int32_t)column,
                y + (int32_t)row,
                pixel);
        }
    }
}

static bool desktop_draw_wallpaper_asset_locked(
    const desktop_render_context_t *context) {
    const desktop_asset_image_t *asset = context && context->wallpaper ? context->wallpaper : &g_desktop_wallpaper_asset;
    volatile uint32_t *target;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t display_stride;
    uint32_t *destination;
    uint64_t image_width;
    uint64_t image_height;
    uint64_t x_scale;
    uint64_t y_scale;
    int64_t image_left;
    int64_t image_top;

    if (context == 0 ||
        !atomic_load_explicit(&g_desktop_assets_available,
                              memory_order_acquire) ||
        asset->pixels == 0 || asset->width == 0U || asset->height == 0U) {
        return false;
    }

    target = context->target != 0 ?
        context->target : g_window_server.composite_framebuffer;
    display_width = context->target_width != 0U ?
        context->target_width : g_window_server.display_width;
    display_height = context->target_height != 0U ?
        context->target_height : g_window_server.display_height;
    display_stride = context->target_stride != 0U ?
        context->target_stride : g_window_server.display_stride;
    if (display_width == 0U || display_height == 0U ||
        display_stride < display_width ||
        context->bounds.x0 >= context->bounds.x1 ||
        context->bounds.y0 >= context->bounds.y1) {
        return false;
    }

    /*
    * 此函数仅在构建私有 WB 桌面缓存时使用。将缓存目标保存为普通指针，使编译器能把目标行和步长
    * 保留在寄存器中。旧表达式每个像素都要通过 volatile 指针
    * window_server.composite_framebuffer 访问，导致一次性壁纸构建在 QEMU 上明显卡顿。
     */
    destination = (uint32_t *)(uintptr_t)target;
    if (destination == 0) return false;

    /* 保持图像宽高比并缩放至覆盖整个显示区域。启动卷上有意保留高分辨率源图；因此较小的
     * 显示器会使用缩小并居中的图像。 */
    if ((uint64_t)display_width * asset->height >=
        (uint64_t)display_height * asset->width) {
        image_width = display_width;
        image_height = ((uint64_t)display_width *
                        asset->height + asset->width - 1U) / asset->width;
    } else {
        image_height = display_height;
        image_width = ((uint64_t)display_height *
                       asset->width + asset->height - 1U) / asset->height;
    }
    if (image_width == 0U || image_height == 0U ||
        image_width > UINT32_MAX || image_height > UINT32_MAX) {
        return false;
    }

    image_left = ((int64_t)display_width -
                  (int64_t)image_width) / 2;
    image_top = ((int64_t)display_height -
                 (int64_t)image_height) / 2;
    x_scale = ((uint64_t)asset->width << 32U) / image_width;
    y_scale = ((uint64_t)asset->height << 32U) / image_height;

    for (int32_t y = (int32_t)context->bounds.y0;
         y < (int32_t)context->bounds.y1; ++y) {
        uint64_t output_y = (uint64_t)((int64_t)y - image_top);
        uint32_t source_y = (uint32_t)((output_y * y_scale) >> 32U);
        if (source_y >= asset->height) source_y = asset->height - 1U;
        uint64_t source_x_fixed =
            (uint64_t)((int64_t)context->bounds.x0 - image_left) * x_scale;
        const uint8_t *source_row = asset->pixels +
            (uint64_t)source_y * asset->stride;
        uint32_t *destination_row = destination +
            (uint64_t)(uint32_t)y * display_stride;
        for (int32_t x = (int32_t)context->bounds.x0;
             x < (int32_t)context->bounds.x1; ++x) {
            uint32_t source_x = (uint32_t)(source_x_fixed >> 32U);
            if (source_x >= asset->width) source_x = asset->width - 1U;
            const uint8_t *pixel = source_row +
                (uint64_t)source_x * 4U;
            uint32_t color = ((uint32_t)pixel[0] << 16) |
                             ((uint32_t)pixel[1] << 8) |
                             pixel[2];
            destination_row[(uint32_t)x] = color;
            if (x + 1 < (int32_t)context->bounds.x1) {
                source_x_fixed += x_scale;
            }
        }
    }
    return true;
}

uint32_t desktop_app_at_locked(uint32_t x, uint32_t y) {
    if (!desktop_shell_assets_available() || x > INT32_MAX || y > INT32_MAX) return DESKTOP_APP_NONE;
    /* 与绘制使用相同的放置结果；当前选中项最后绘制、优先命中。 */
    uint32_t top = g_window_server.desktop.top_app;
    for (uint32_t step=0; step<DESKTOP_APP_SLOTS; ++step) {
        uint32_t app = step == 0 ? top : DESKTOP_APP_SLOTS-step;
        if (!app || (step && app == top)) continue;
        control_tree_box_t box;
        if (desktop_icon_box_locked(app, &box) && box.enabled &&
            (int32_t)x >= box.bounds.x0 && (int32_t)x < box.bounds.x1 &&
            (int32_t)y >= box.bounds.y0 && (int32_t)y < box.bounds.y1 &&
            (int32_t)x >= box.clip.x0 && (int32_t)x < box.clip.x1 &&
            (int32_t)y >= box.clip.y0 && (int32_t)y < box.clip.y1) return app;
    }
    return DESKTOP_APP_NONE;
}

void desktop_set_hovered_app_locked(uint32_t app) {
    g_window_server.desktop_hovered_app = app;
}

static void desktop_draw_running_locked(const desktop_render_context_t *context,
    uint32_t app, const control_tree_box_t *box) {
    if (!(context->running_mask & (1U << app))) return;
    bool active = (context->active_mask & (1U << app)) != 0;
    int32_t width = active ? 16 : 6;
    Rect clip = context->bounds;
    if (clip.x0 < box->clip.x0) clip.x0 = box->clip.x0;
    if (clip.y0 < box->clip.y0) clip.y0 = box->clip.y0;
    if (clip.x1 > box->clip.x1) clip.x1 = box->clip.x1;
    if (clip.y1 > box->clip.y1) clip.y1 = box->clip.y1;
    if (!rect_clip_to_extent(clip.x0, clip.y0, clip.x1, clip.y1,
        g_window_server.display_width, g_window_server.display_height, &clip)) return;
    control_raster_target_t target = {g_window_server.composite_framebuffer,
        g_window_server.display_stride, clip};
    control_raster_fill_alpha(&target, (int64_t)box->bounds.x0+
        ((int64_t)box->bounds.x1-box->bounds.x0-width)/2, box->bounds.y1-4,
        (uint32_t)width, 3, 1, 0xff000000U | (active ? OS_THEME_ACCENT : OS_THEME_MUTED));
}

void desktop_mark_icon_box_locked(const control_tree_box_t *box) {
    Rect damage = {box->bounds.x0 > box->clip.x0 ? box->bounds.x0 : box->clip.x0,
        box->bounds.y0 > box->clip.y0 ? box->bounds.y0 : box->clip.y0,
        box->bounds.x1 < box->clip.x1 ? box->bounds.x1 : box->clip.x1,
        box->bounds.y1 < box->clip.y1 ? box->bounds.y1 : box->clip.y1};
    if (!rect_is_empty(damage)) window_mark_rect_locked(damage.x0, damage.y0,
        (uint32_t)(damage.x1 - damage.x0), (uint32_t)(damage.y1 - damage.y0));
}

void desktop_mark_app_locked(uint32_t app) {
    control_tree_box_t box;
    if (desktop_icon_box_locked(app,&box)) desktop_mark_icon_box_locked(&box);
}

static void desktop_draw_icon_locked(const desktop_render_context_t *context,
    uint32_t app, const control_tree_box_t *box) {
    if (!context || !box || !box->visible || !app ||
        app >= sizeof(g_desktop_app_assets) / sizeof(g_desktop_app_assets[0])) return;
    uint32_t width = (uint32_t)(box->bounds.x1 - box->bounds.x0);
    uint32_t height = (uint32_t)(box->bounds.y1 - box->bounds.y0);
    if (!width || !height) return;
    uint32_t image_width = width < DESKTOP_ICON_IMAGE_WIDTH ? width : DESKTOP_ICON_IMAGE_WIDTH;
    uint32_t image_height = height < DESKTOP_ICON_IMAGE_HEIGHT ? height : DESKTOP_ICON_IMAGE_HEIGHT;
    int32_t image_x = box->bounds.x0 + (int32_t)((width - image_width) / 2U);
    int32_t image_y = box->bounds.y0 + (int32_t)((height - image_height) / 2U);
    desktop_render_context_t clipped = *context;
    if (clipped.bounds.x0 < box->clip.x0) clipped.bounds.x0 = box->clip.x0;
    if (clipped.bounds.y0 < box->clip.y0) clipped.bounds.y0 = box->clip.y0;
    if (clipped.bounds.x1 > box->clip.x1) clipped.bounds.x1 = box->clip.x1;
    if (clipped.bounds.y1 > box->clip.y1) clipped.bounds.y1 = box->clip.y1;
    if (rect_is_empty(clipped.bounds)) return;
    if (context->hovered_app == app && width > 16U && height > 4U)
        desktop_draw_hover_frame_locked(box->bounds.x0 + 8, box->bounds.y0 + 2,
            width - 16U, height - 4U, &clipped, &box->style);
    const desktop_asset_image_t *asset = &g_desktop_app_assets[app];
    if (asset->pixels) desktop_draw_asset_icon_locked(&clipped, image_x, image_y,
        image_width, image_height, asset, 0U, 0U, asset->width, asset->height);
    desktop_draw_running_locked(&clipped, app, box);
}

static void desktop_paint_node(void *raw, const os_control_node_t *node,
    const control_tree_box_t *box, Rect damage) {
    if (node->kind != OS_CONTROL_BUTTON || !node->user_data ||
        node->user_data >= sizeof(g_desktop_app_assets)/sizeof(g_desktop_app_assets[0])) return;
    desktop_render_context_t context = *(const desktop_render_context_t *)raw;
    context.bounds = damage;
    desktop_draw_icon_locked(&context, (uint32_t)node->user_data, box);
}

static bool desktop_render_assets_locked(
    const desktop_render_context_t *context) {
    uint32_t left;
    uint32_t right;
    uint32_t top;
    uint32_t bottom;

    if (context == 0) {
        return false;
    }

    left = context->bounds.x0;
    right = context->bounds.x1;
    top = context->bounds.y0;
    bottom = context->bounds.y1;

    if (g_window_server.composite_framebuffer == 0 ||
        left >= right ||
        top >= bottom ||
        !desktop_shell_assets_available() ||
        g_desktop_wallpaper_asset.pixels == 0) {
        return false;
    }

    if (!desktop_draw_wallpaper_asset_locked(context)) {
        return false;
    }

    return context->wallpaper_only || control_tree_paint_view(g_window_server.scene.controls, g_window_server.scene.control_count,
        desktop_viewport(), context->bounds, desktop_paint_node, (void *)context);
}


/*
 * 桌面保留后备表面。
 *
 * 只缓存壁纸；图标位置来自帧快照，不得把旧位置烘焙到壁纸导致拖动残影。
 */
static uint32_t *g_desktop_cache;
static uint32_t g_desktop_cache_width;
static uint32_t g_desktop_cache_height;
static uint32_t g_desktop_cache_stride;
static atomic_bool g_desktop_cache_ready;


/*
 * 在资源工作线程所在 CPU 上构建静态桌面。
 *
 * 合成器绝不能花数十秒缩放大型 PNG。通过上下文目标渲染到私有缓存，而不是临时修改
 * g_window_server.composite_framebuffer；这样生成缓存期间，实时合成器和输入路径仍可独立运行。
 */
bool desktop_shell_build_cache(void) {
    desktop_render_context_t cache_context = {
        .wallpaper_only = true,
        .bounds = {
            .x0 = 0U,
            .y0 = 0U,
            .x1 = 0U,
            .y1 = 0U,
        },
        .hovered_app = DESKTOP_APP_NONE,
        .target = 0,
        .target_width = 0U,
        .target_height = 0U,
        .target_stride = 0U,
    };
    uint32_t display_width = g_window_server.display_width;
    uint32_t display_height = g_window_server.display_height;
    uint32_t display_stride = g_window_server.display_stride;
    uint64_t bytes;

    if (display_width == 0U ||
        display_height == 0U ||
        display_stride < display_width ||
        !desktop_shell_assets_available()) {
        return false;
    }

    if (atomic_load_explicit(&g_desktop_cache_ready,
                             memory_order_acquire) &&
        g_desktop_cache != 0 &&
        g_desktop_cache_width == display_width &&
        g_desktop_cache_height == display_height &&
        g_desktop_cache_stride == display_stride) {
        return true;
    }

    /* 只有完整渲染成功后，资源工作线程才会发布内容。 */
    atomic_store_explicit(&g_desktop_cache_ready, false,
                          memory_order_release);

    if (g_desktop_cache != 0 &&
        (g_desktop_cache_width != display_width ||
         g_desktop_cache_height != display_height ||
         g_desktop_cache_stride != display_stride)) {
        kfree(g_desktop_cache);
        g_desktop_cache = 0;
    }

    bytes = (uint64_t)display_stride *
            display_height * sizeof(uint32_t);
    if (bytes == 0U || bytes > (uint64_t)SIZE_MAX) {
        return false;
    }

    if (g_desktop_cache == 0) {
        g_desktop_cache = (uint32_t *)kzalloc((size_t)bytes, 0U);
        if (g_desktop_cache == 0) {
            return false;
        }
    }

    g_desktop_cache_width = display_width;
    g_desktop_cache_height = display_height;
    g_desktop_cache_stride = display_stride;
    cache_context.bounds.x1 = display_width;
    cache_context.bounds.y1 = display_height;
    cache_context.target = (volatile uint32_t *)g_desktop_cache;
    cache_context.target_width = display_width;
    cache_context.target_height = display_height;
    cache_context.target_stride = display_stride;

    if (!desktop_render_assets_locked(&cache_context)) {
        return false;
    }

    return true;
}

/* 只发布完整的屏幕大小缓存；合成线程在帧边界接管并释放旧缓存。 */
static _Atomic(uint32_t *) g_desktop_pending_wallpaper;
bool desktop_wallpaper_pending(void) {
    return atomic_load_explicit(&g_desktop_pending_wallpaper,memory_order_acquire)!=0;
}
uint32_t *desktop_wallpaper_build_cache(const desktop_asset_image_t *asset) {
    uint32_t w=g_window_server.display_width,h=g_window_server.display_height,stride=g_window_server.display_stride;
    uint64_t bytes=(uint64_t)stride*h*sizeof(uint32_t);
    if (!asset || !w || !h || stride<w || bytes>SIZE_MAX) return 0;
    uint32_t *pixels=kmalloc((size_t)bytes,0);
    if (!pixels) return 0;
    desktop_render_context_t context={.bounds={0,0,(int32_t)w,(int32_t)h},
        .target=pixels,.target_width=w,.target_height=h,.target_stride=stride,
        .wallpaper_only=true,.wallpaper=asset};
    if (!desktop_draw_wallpaper_asset_locked(&context)) {kfree(pixels);return 0;}
    return pixels;
}
kstatus_t desktop_wallpaper_stage(uint32_t *cache) {
    uint32_t *expected=0;
    if (!atomic_compare_exchange_strong_explicit(&g_desktop_pending_wallpaper,&expected,cache,
            memory_order_release,memory_order_relaxed)) return K_EBUSY;
    window_lock();window_mark_dirty_locked();window_unlock();window_server_notify_worker();return K_OK;
}
void desktop_wallpaper_apply_pending(void) {
    uint32_t *next=atomic_exchange_explicit(&g_desktop_pending_wallpaper,0,memory_order_acquire);
    if (!next) return;
    uint32_t *old=g_desktop_cache;
    g_desktop_cache=next;
    g_desktop_cache_width=g_window_server.display_width;
    g_desktop_cache_height=g_window_server.display_height;
    g_desktop_cache_stride=g_window_server.display_stride;
    atomic_store_explicit(&g_desktop_cache_ready,true,memory_order_release);
    kfree(old);
}

/* 在场景锁下发布可见状态和完整损坏区域。较早的帧保留其捕获时的就绪状态，因此不会有帧将
 * 占位瓦片与新就绪的壁纸混合显示。 */
void desktop_shell_publish_cache_locked(void) {
    atomic_store_explicit(&g_desktop_cache_ready, true, memory_order_release);
    window_mark_dirty_locked();
}

/* 合成器只会读取已完整发布的缓存。 */
bool desktop_shell_cache_ready(void) {
    uint32_t display_width = g_window_server.display_width;
    uint32_t display_height = g_window_server.display_height;
    uint32_t display_stride = g_window_server.display_stride;

    if (display_width == 0U || display_height == 0U ||
        display_stride < display_width ||
        !desktop_shell_assets_available() ||
        !atomic_load_explicit(&g_desktop_cache_ready,
                              memory_order_acquire) ||
        g_desktop_cache == 0) {
        return false;
    }

    return g_desktop_cache_width == display_width &&
           g_desktop_cache_height == display_height &&
           g_desktop_cache_stride == display_stride;
}


/*
 * 仅恢复当前损坏的桌面矩形。
 */
static void desktop_copy_cached_region(void) {
    Rect bounds;
    volatile uint32_t *target = g_window_server.composite_framebuffer;
    if (g_desktop_cache == 0 || target == 0 ||
        !rect_clip_to_extent(g_compositor_snapshot.damage_bounds.x0,
            g_compositor_snapshot.damage_bounds.y0,
            g_compositor_snapshot.damage_bounds.x1,
            g_compositor_snapshot.damage_bounds.y1,
            g_window_server.display_width, g_window_server.display_height,
            &bounds)) {
        return;
    }

    /* 与窗口表面共用目标分类，低内存直写回退不能按普通内存复制。 */
    bool private_wb = target != g_window_server.framebuffer;
    for (uint32_t row = (uint32_t)bounds.y0; row < (uint32_t)bounds.y1; ++row) {
        volatile uint32_t *destination = target +
            (uint64_t)row * g_window_server.display_stride + bounds.x0;
        const uint32_t *source = g_desktop_cache +
            (uint64_t)row * g_desktop_cache_stride + bounds.x0;
        compositor_surface_copy_destination(
            private_wb ? (uint32_t *)(uintptr_t)destination : 0,
            private_wb ? 0 : destination, source,
            (uint32_t)(bounds.x1 - bounds.x0));
    }
}


/*
 * 壁纸恢复后按不可变帧状态绘制图标；命中和绘制使用相同的位移规则。
 */
static void desktop_draw_hover_locked(void) {
    uint32_t hovered =
        g_compositor_snapshot.desktop_hovered_app;
    desktop_render_context_t context = {
        .bounds = g_compositor_snapshot.damage_bounds,
        .hovered_app = hovered,
        .running_mask = g_compositor_snapshot.desktop.running_mask,
        .active_mask = g_compositor_snapshot.desktop.active_mask,
    };

    const desktop_interaction_t *state = &g_compositor_snapshot.desktop;
    for (uint32_t step=1; step<=DESKTOP_APP_SLOTS; ++step) {
        uint32_t app = step == DESKTOP_APP_SLOTS ? state->top_app : step;
        if (!app || (step != DESKTOP_APP_SLOTS && app == state->top_app)) continue;
        control_tree_box_t box;
        if (!desktop_base_icon_box_locked(app, &box)) continue;
        desktop_place_box(state, app, desktop_viewport(), &box);
        desktop_draw_icon_locked(&context, app, &box);
    }
}


static void desktop_clear_region_locked(void) {
    uint32_t left = g_compositor_snapshot.damage_bounds.x0;
    uint32_t right = g_compositor_snapshot.damage_bounds.x1;
    uint32_t top = g_compositor_snapshot.damage_bounds.y0;
    uint32_t bottom = g_compositor_snapshot.damage_bounds.y1;

    if (g_window_server.composite_framebuffer == 0 ||
        left >= right ||
        top >= bottom) {
        return;
    }

    for (uint32_t row = top; row < bottom; ++row) {
        compositor_fill_span_wb(
            g_window_server.composite_framebuffer +
                (uint64_t)row * g_window_server.display_stride + left,
            right - left,
            0x000A1020U);
    }
}


/*
 * 桌面快速合成路径。
 */
void desktop_draw_wallpaper_locked(void) {
    if (!g_compositor_snapshot.desktop_cache_ready) {
        /*
         * 图像由专用资源所有者加载。在资源就绪前，只显示确定性的纯色表面；不得回退到已移除的
         * 程序化桌面渲染器。
         */
        desktop_clear_region_locked();
        return;
    }

    desktop_copy_cached_region();
    desktop_draw_hover_locked();
}


void desktop_cycle_window_focus(void) {
    uint32_t first_identifier = 0U;
    uint32_t next_identifier = 0U;
    bool choose_next = false;

    for (uint32_t index = 0U; ; ++index) {
        window_server_snapshot_t snapshot;
        kstatus_t status = window_server_snapshot(index, &snapshot);
        if (status != K_OK) break;
        if (!snapshot.visible || snapshot.identifier == 0U) continue;

        if (first_identifier == 0U) {
            first_identifier = snapshot.identifier;
        }
        if (choose_next) {
            next_identifier = snapshot.identifier;
            break;
        }
        if (snapshot.focused) {
            choose_next = true;
        }
    }

    if (next_identifier == 0U) next_identifier = first_identifier;
    if (next_identifier != 0U) {
        (void)window_server_focus(next_identifier);
    }
}
