#include <kernel/sched.h>
#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/display.h>
#include <kernel/gpu.h>
#include <kernel/realtest.h>
#include "internal.h"

/* REFACTOR_P7A_COMPOSITOR_OWNER：帧处理协调。 */

#ifndef OS_REALTEST
#define OS_REALTEST 0
#endif

static kstatus_t g_compositor_publication_status;
static atomic_uint_fast64_t g_compositor_native_retry_deadline;

bool compositor_native_retry_ready(void) {
    uint64_t deadline = atomic_load_explicit(
        &g_compositor_native_retry_deadline, memory_order_acquire);
    return deadline == 0U || (int64_t)(x86_read_tsc() - deadline) >= 0;
}

uint64_t compositor_native_retry_wait_ns(void) {
    return compositor_native_retry_ready() ? UINT64_MAX : 16000000ULL;
}

#if OS_REALTEST
static uint64_t compositor_display_probe_hash(
    const volatile uint32_t *pixels,
    uint32_t stride,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height) {
    uint64_t hash = 1469598103934665603ULL;

    if (pixels == 0 || stride == 0U || width == 0U || height == 0U) {
        return 0U;
    }

    for (uint32_t row = 0U; row < height; ++row) {
        for (uint32_t column = 0U; column < width; ++column) {
            uint32_t pixel = pixels[(uint64_t)(y + row) * stride +
                                    x + column];
            for (uint32_t byte = 0U; byte < sizeof(pixel); ++byte) {
                hash ^= (uint8_t)(pixel >> (byte * 8U));
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

static void compositor_report_display_probe(void) {
    static bool reported;
    const compositor_window_view_t *view = 0;
    uint32_t surface_x;
    uint32_t surface_y;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    const volatile uint32_t *scene;
    const volatile uint32_t *scanout;
    volatile uint32_t *probe_pixel;
    uint64_t scene_hash;
    uint64_t scanout_hash;
    uint32_t center_offset;
    uint32_t probe_saved;
    uint32_t probe_observed;
    Rect damage0 = { 0U, 0U, 0U, 0U };

    if (reported || g_window_server.composite_framebuffer == 0 ||
        g_window_server.framebuffer == 0) {
        return;
    }

    for (uint32_t index = 0U;
         index < g_compositor_snapshot.window_count;
         ++index) {
        const compositor_window_view_t *candidate =
            &g_compositor_snapshot.windows[index];
        if (candidate->width == 800U && candidate->height == 600U &&
            candidate->x >= 0 && candidate->y >= 0 &&
            window_client_decorations(candidate->flags)) {
            view = candidate;
            break;
        }
    }
    if (view == 0) return;

    surface_x = (uint32_t)view->x + window_client_offset_x(view->flags);
    surface_y = (uint32_t)view->y + window_client_offset_y(view->flags);
    width = view->width;
    height = view->height;
    stride = g_window_server.display_stride;
    if (surface_x > g_window_server.display_width ||
        surface_y > g_window_server.display_height ||
        width > g_window_server.display_width - surface_x ||
        height > g_window_server.display_height - surface_y ||
        stride < g_window_server.display_width) {
        return;
    }

    scene = g_window_server.composite_framebuffer +
            (uint64_t)surface_y * stride + surface_x;
    scanout = g_window_server.framebuffer +
              (uint64_t)surface_y * stride + surface_x;
    scene_hash = compositor_display_probe_hash(scene, stride, 0U, 0U,
                                                width, height);
    scanout_hash = compositor_display_probe_hash(scanout, stride, 0U, 0U,
                                                  width, height);
    center_offset = (height / 2U) * stride + width / 2U;
    probe_pixel = (volatile uint32_t *)(uintptr_t)(scanout + center_offset);
    probe_saved = *probe_pixel;
    compositor_copy_wc_scanline(
        probe_pixel,
        (const uint32_t *)(uintptr_t)(scene + center_offset),
        1U);
    __asm__ volatile ("sfence" : : : "memory");
    probe_observed = *probe_pixel;
    *probe_pixel = probe_saved;
    __asm__ volatile ("sfence" : : : "memory");
    if (g_compositor_snapshot.damage_count != 0U) {
        damage0 = g_compositor_snapshot.damage_rects[0];
    }
    reported = true;
    kernel_serial_printf_serial_only(
        "OS_DISPLAY_SURFACE_PROBE x=%u y=%u width=%u height=%u "
        "scene_hash=%llx scanout_hash=%llx scene0=%x scanout0=%x "
        "scene_center=%x scanout_center=%x damage_count=%u "
        "d0=%u,%u,%u,%u plan=%u/%u copy_probe=%x\r\n",
        surface_x, surface_y, width, height,
        (unsigned long long)scene_hash,
        (unsigned long long)scanout_hash,
        (unsigned)scene[0],
        (unsigned)scanout[0],
        (unsigned)scene[center_offset],
        (unsigned)scanout[center_offset],
        g_compositor_snapshot.damage_count,
        damage0.x0, damage0.y0, damage0.x1, damage0.y1,
        g_compositor_render_plan_valid ? 1U : 0U,
        g_compositor_render_plan_count,
        probe_observed);
}
#endif

/* Linux Xcursor/Adwaita top_left_arrow 光标，24x24，热点为 (3,1)。
 * 源数据是 ARGB 光标 surface；逐像素按与 Linux 软件光标相同的 source-over 规则合成。 */








/*
 * 返回一个标题栏按钮的屏幕坐标矩形。
 *
 * 从右向左的槽位顺序：
 *
 *     close      = 0（关闭）
 *     maximize   = 1（最大化）
 *     minimize   = 2（最小化）
 */




static uint32_t compositor_native_cursor_blend(uint32_t background,
                                               uint32_t cursor) {
    uint32_t alpha = cursor >> 24U;
    uint32_t color = cursor & 0x00FFFFFFU;
    uint32_t inverse;

    if (alpha == 0U) return background;
    if (alpha >= 255U) return color;

    inverse = 255U - alpha;
    return (((((background >> 16U) & 0xFFU) * inverse +
              ((color >> 16U) & 0xFFU) * alpha + 127U) / 255U) << 16U) |
           (((((background >> 8U) & 0xFFU) * inverse +
              ((color >> 8U) & 0xFFU) * alpha + 127U) / 255U) << 8U) |
           ((((background & 0xFFU) * inverse +
              (color & 0xFFU) * alpha + 127U) / 255U));
}


/*
 * 原生扫描输出会消费一个分配对象，而保留式合成器场景刻意使用普通 WB 缓冲区。
 * 提交前先将场景快照复制到新的分配对象。这是明确的正确性边界：此函数返回后，
 * 合成器可能立即开始绘制下一帧，但原生 KAPI 的延迟工作线程仍可能在复制已提交的源数据。
 */
static kstatus_t compositor_submit_native_snapshot(void) {
    gpu_allocation_t *buffer = 0;
    gpu_fence_t *fence = 0;
    const uint32_t *scene;
    uint32_t width = g_window_server.display_width;
    uint32_t height = g_window_server.display_height;
    uint32_t stride = g_window_server.display_stride;
    uint64_t row_bytes;
    uint64_t frame_bytes;
    void *mapped = 0;
    kstatus_t status;
    static bool accepted_reported;

    if (!display_core_has_native_scanout(0U) ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.composite_framebuffer ==
            g_window_server.framebuffer ||
        width == 0U || height == 0U || stride < width) {
        return K_EINVAL;
    }

    row_bytes = (uint64_t)stride * sizeof(uint32_t);
    if (row_bytes == 0U ||
        (uint64_t)height > UINT64_MAX / row_bytes) {
        return K_EOVERFLOW;
    }
    frame_bytes = row_bytes * height;

    status = gpu_allocation_create(frame_bytes, &buffer);
    if (status != K_OK || buffer == 0) {
        return status == K_OK ? K_ENOMEM : status;
    }

    status = gpu_fence_create(&fence);
    if (status != K_OK || fence == 0) {
        object_put(buffer);
        return status == K_OK ? K_ENOMEM : status;
    }

    status = gpu_allocation_cpu_map(buffer, 0U, frame_bytes, &mapped);
    if (status != K_OK || mapped == 0) {
        object_put(fence);
        object_put(buffer);
        return status == K_OK ? K_EIO : status;
    }

    scene = (const uint32_t *)(uintptr_t)g_window_server.composite_framebuffer;
    for (uint32_t row = 0U; row < height; ++row) {
        __builtin_memcpy(
            (uint8_t *)mapped + (uint64_t)row * row_bytes,
            scene + (uint64_t)row * stride,
            (size_t)row_bytes);
    }

    /* 保留场景不包含光标。原生扫描输出没有直接 GOP 光标叠加层，因此将快照中的光标绘入此私有副本。 */
    {
        int64_t origin_x =
            (int64_t)g_compositor_snapshot.pointer_x -
            window_cursor_hotspot_x(g_compositor_snapshot.cursor_shape);
        int64_t origin_y =
            (int64_t)g_compositor_snapshot.pointer_y -
            window_cursor_hotspot_y(g_compositor_snapshot.cursor_shape);
        int64_t left = origin_x < 0 ? 0 : origin_x;
        int64_t top = origin_y < 0 ? 0 : origin_y;
        int64_t right = origin_x + WINDOW_CURSOR_WIDTH;
        int64_t bottom = origin_y + WINDOW_CURSOR_HEIGHT;
        uint32_t *frame = (uint32_t *)mapped;

        if (right > (int64_t)width) right = width;
        if (bottom > (int64_t)height) bottom = height;
        if (left < right && top < bottom) {
            for (int64_t y = top; y < bottom; ++y) {
                for (int64_t x = left; x < right; ++x) {
                    uint32_t cursor_x = (uint32_t)(x - origin_x);
                    uint32_t cursor_y = (uint32_t)(y - origin_y);
                    uint64_t offset = (uint64_t)y * stride + (uint64_t)x;
                    frame[offset] = compositor_native_cursor_blend(
                        frame[offset],
                        window_cursor_pixel(g_compositor_snapshot.cursor_shape,
                                            cursor_x, cursor_y));
                }
            }
        }
    }

    gpu_allocation_cpu_unmap(buffer, mapped, frame_bytes);
    mapped = 0;

    status = display_commit_submit(
        0U, buffer, 0U, row_bytes, width, height, 1U,
        0, 0U, fence, 1U);
    if (status != K_OK) {
        object_put(fence);
        object_put(buffer);
        return status;
    }

    /* 队列接受不代表已经显示。保持快照不可变，并在原生释放 fence 真正完成前不发送 ACK。 */
    uint64_t deadline = x86_read_tsc() + x86_timeout_ns_to_tsc(300000000ULL);
    do {
        display_core_poll_scanout();
        status = gpu_fence_wait(fence, 1U, 16000000ULL);
    } while (status == K_ETIMEDOUT &&
             (int64_t)(x86_read_tsc() - deadline) < 0);
    if (status != K_OK) {
        object_put(fence);
        object_put(buffer);
        return status;
    }

    /* 已提交的快照包含当前指针。此路径不会修改直接 GOP 光标状态；下一原生帧始终是完整场景快照。 */
    g_window_server.presented_pointer_x =
        g_compositor_snapshot.pointer_x;
    g_window_server.presented_pointer_y =
        g_compositor_snapshot.pointer_y;
    g_window_server.presented_pointer_valid = true;
    g_window_server.presented_cursor_shape = g_compositor_snapshot.cursor_shape;

    if (!accepted_reported) {
        accepted_reported = true;
        realtest_checkpoint(
            "OS_NVKMS_NATIVE_COMPOSITOR_ACCEPTED");
    }

    object_put(fence);
    object_put(buffer);
    return K_OK;
}


void compositor_commit_snapshot(void) {
    bool commit_as_move_transaction;
    bool allow_yield;
    kstatus_t native_status;

    if (g_window_server.framebuffer == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.composite_framebuffer ==
            g_window_server.framebuffer ||
        g_compositor_snapshot.damage_count == 0U) {
        return;
    }

    /* 原生扫描输出拥有完整帧。不要再通过固件 GOP 映射发布相同场景，因为它不是当前活动的 GB205 目标。 */
    if (display_core_has_native_scanout(0U)) {
        display_core_poll_scanout();
        native_status = compositor_submit_native_snapshot();
        g_compositor_publication_status = native_status;
        atomic_store_explicit(&g_compositor_native_retry_deadline,
            native_status == K_OK ? 0U :
                x86_read_tsc() + x86_timeout_ns_to_tsc(16000000ULL),
            memory_order_release);
        return;
    }

    commit_as_move_transaction =
        g_compositor_snapshot.dragging_identifier != 0U;

    /* 光标大小及其他小型更新保持原子性（24x24 复制中没有有意义的调度点），
     * 大型普通客户端重绘则按有界行块允许抢占，拖动/移动保持原子事务。
     * QEMU 和真机均在发布过程中逐行保留可见光标，
     * 合成期间则固定不可变场景。 */
    allow_yield = !commit_as_move_transaction &&
                  compositor_publication_damage_pixels() >
                  WINDOW_COMPOSITOR_ATOMIC_PIXELS;

    /*
    * 仅 GOP 发布阶段不可抢占。所有耗时的场景合成已在普通 WB 内存中完成。
     */
    sched_preempt_disable();

    if (commit_as_move_transaction) {
        /*
         * 开始较大的可见包围事务前，先擦除保留拖动区域仅存在于旧位置的尾边。额外流量
         * 通常只有 delta_x*height + delta_y*overlap_width 个像素。
         */
        compositor_publication_drag_old_exposure();

        Rect transaction;
        if (compositor_snapshot_damage_bounds(&g_compositor_snapshot, &transaction))
            compositor_publication_rect(&transaction, false);
    } else {
        /* 普通损坏直接发布，避免重复复制坐标及无符号判断漏掉屏幕边缘。 */
        compositor_publication_rects(g_compositor_snapshot.damage_rects,
            g_compositor_snapshot.damage_count, allow_yield);
    }

    /*
    * 扫描输出观察该事务前，必须完成所有 WC 存储。
     */
    __asm__ volatile (
        "sfence"
        :
        :
        : "memory");

    sched_preempt_enable();
}


/*
 * 仅当此场景发布可能覆盖当前显示器上可见的软件光标像素时才返回 true。
 *
 * 如果指针在合成期间或之后移动，即使 force=false，compositor_present_cursor_direct
 * 也会发现坐标变化。因此仅当场景损坏区域与先前显示的光标矩形相交时才需要强制路径。
 */
/*
 * 设备内存发布由显示核心负责。
 *
 * P5 仍负责损坏区域选择、行划分和事务策略；此底层调用只执行架构/输出相关的 WB 到
 * 扫描输出跨度写入。这使合成器无需依赖 MOVDIR64B/MOVNTI 细节，并让 DISPLAY_COMMIT
 * 使用同一条优化后的 GOP 路径。
 */
const uint32_t g_linux_cursor_argb[WINDOW_CURSOR_WIDTH * WINDOW_CURSOR_HEIGHT] = {
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x02000000U, 0x45414141U, 0x03000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x01000000U, 0x09000000U, 0xF7F5F5F5U, 0x4B414141U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x02000000U, 0x15000000U, 0xFFFFFFFFU, 0xF7EAEAEAU, 0x4E414141U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1A000000U, 0xFFFFFFFFU, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFDC1C1C1U, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xF8F5F5F5U, 0x4C414141U, 0x03000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF414141U, 0xFF404040U, 0xFF000000U, 0xFF4D4D4DU, 0xE8CBCBCBU, 0x5F000000U, 0x59000000U, 0x54000000U, 0x3F000000U, 0x1A000000U, 0x05000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF414141U, 0xFCF3F3F3U, 0xF7BABABAU, 0xFF000000U, 0xFF010101U, 0xF7D6D6D6U, 0x65414141U, 0x1E000000U, 0x1A000000U, 0x15000000U, 0x09000000U, 0x03000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF3D3D3DU, 0xF9E9E9E9U, 0x90464646U, 0xF4E1E1E1U, 0xFF323232U, 0xFF000000U, 0xFE626262U, 0xCDB5B5B5U, 0x0E000000U, 0x04000000U, 0x02000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xF9EAEAEAU, 0x82404040U, 0x43000000U, 0xA5737373U, 0xF9A5A5A5U, 0xFF000000U, 0xFF070707U, 0xF8E5E5E5U, 0x3E2C2C2CU, 0x03000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1A000000U, 0xF9F4F4F4U, 0x7F404040U, 0x30000000U, 0x1E000000U, 0x460E0E0EU, 0xF8EBEBEBU, 0xFF1F1F1FU, 0xFF000000U, 0xFD787878U, 0xB79F9F9FU, 0x09000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x02000000U, 0x15000000U, 0x6F404040U, 0x2D000000U, 0x11000000U, 0x0A000000U, 0x21000000U, 0xB4898989U, 0xFB8E8E8EU, 0xFF000000U, 0xFF101010U, 0xF9F0F0F0U, 0x19070707U, 0x02000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x01000000U, 0x09000000U, 0x19000000U, 0x0D000000U, 0x04000000U, 0x03000000U, 0x11000000U, 0x521A1A1AU, 0xF9ECECECU, 0xFF1F1F1FU, 0xFF202020U, 0xFAF0F0F0U, 0x1E060606U, 0x03000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x02000000U, 0x05000000U, 0x03000000U, 0x01000000U, 0x01000000U, 0x07000000U, 0x23000000U, 0x99636363U, 0xFAF0F0F0U, 0xFAF0F0F0U, 0x96626262U, 0x17000000U, 0x02000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x02000000U, 0x0F000000U, 0x31000000U, 0x53050505U, 0x52050505U, 0x31000000U, 0x0E000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x01000000U, 0x04000000U, 0x0E000000U, 0x17000000U, 0x17000000U, 0x0E000000U, 0x04000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
};

/* 光标栅格化由 cursor_occlusion.c 负责。
    * 仅裁剪一次：
     *
    *     光标矩形
    *       ∩ 屏幕范围
    *       ∩ 当前损坏区域
     *
    * 旧实现会遍历全部 24x24 个源像素，并在 compositor_blend_pixel_locked() 中重复
    * 执行这些边界检查。
     */
compositor_snapshot_t g_compositor_snapshot;

/* REFACTOR_P7B_SNAPSHOT_TEST_OWNER: test fixtures must not write snapshots. */
void compositor_snapshot_test_clear_damage_tiles(void) {
    for (uint32_t word = 0U;
         word < WINDOW_DAMAGE_TILE_WORDS;
         ++word) {
        g_compositor_snapshot.damage_tiles[word] = 0U;
    }
}

bool compositor_snapshot_test_set_damage_tile(uint32_t tile_index) {
    uint32_t word;
    uint32_t bit;

    if (tile_index >= WINDOW_DAMAGE_TILE_WORDS * 64U) {
        return false;
    }

    word = tile_index / 64U;
    bit = tile_index % 64U;
    g_compositor_snapshot.damage_tiles[word] |=
        1ULL << bit;
    return true;
}

void compositor_reset_state_locked(void) {
    g_window_server.composing = false;
}

/* REFACTOR_P7B_FRAME_OWNER: owns the snapshot -> plan -> render -> finish order. */
void compositor_frame_run(void) {
    bool compose;

    g_compositor_publication_status = K_OK;

    desktop_wallpaper_apply_pending();
    /* 仅在窗口锁内捕获不可变场景。 */
    window_lock();
    compose = compositor_snapshot_begin_locked();
    window_unlock();

    if (!compose) return;
    desktop_prepare_text();
    compositor_snapshot_plan();
    if (g_compositor_snapshot.animations) g_compositor_render_plan_valid = false;
    compositor_render_snapshot();
    compositor_snapshot_finish();
}

/* 遮挡淘汰和帧结束共用释放顺序；只能在窗口锁外调用。 */
static void compositor_release_window_snapshot(compositor_window_view_t *view) {
    if (view->reference != 0) {
        window_surface_snapshot_release(view);
        window_control_document_put(view->controls);
        view->controls = 0;
        caption_text_put(view->caption);
        view->caption = 0;
        object_put(view->reference);
        view->reference = 0;
    }
    view->section = 0;
}


/*
 * P16 捕获阶段。
 *
 * 调用时持有 window_lock。此处禁止执行耗时的规划工作。
 */
/* 常规帧和退场动画共用引用、控件文档及前台槽位的捕获规则。 */
void compositor_capture_window_locked(window_server_window_t *window, compositor_window_view_t *view) {
    *view = (compositor_window_view_t){0};
    object_get(window);
    view->reference = window;
    view->controls = window->controls;
    window_control_document_get(view->controls);
    view->caption=window->caption;
    caption_text_get(view->caption);
    view->control_hover = window->control_hover;
    view->control_pressed = window->control_pressed;
    view->control_focus = window->control_focus;
    view->control_key_target = window->control_key_target;
    view->control_text_target=window->control_text_target;
    view->control_text_anchor=window->control_text_anchor;
    view->control_text_end=window->control_text_end;
    view->section = window->section;
    view->identifier = window->identifier;
    view->x = window->x; view->y = window->y;
    view->width = window->width; view->height = window->height;
    view->flags = window->flags; view->background = window->background;
    view->buffer_size = window->buffer_size;
    view->owner_address = window->owner_address;
    view->present_sequence = window->present_sequence;
    view->resize_pending = window->resize_pending;
    view->maximized = window->maximized || window->fullscreen;
    window_surface_snapshot_capture_locked(view);
    for (uint32_t i=0; i<sizeof(view->title); ++i) view->title[i] = window->title[i];
    view->title[sizeof(view->title)-1U] = 0;
}

bool compositor_snapshot_begin_locked(void) {
    compositor_snapshot_t *snapshot =
        &g_compositor_snapshot;

    uint32_t region_count;

    if (!g_window_server.kernel_ready ||
        !g_window_server.dirty ||
        g_window_server.composing ||
        g_window_server.framebuffer == 0) {
        return false;
    }

    desktop_capture_state_locked();

    if (g_window_server.damage_full ||
        g_window_server.damage_tiles_active) {
        region_count = 1U;
    } else {
        region_count =
            g_window_server.damage_count;
    }

    if (region_count == 0U) {
        window_damage_clear_pending_locked();
        return false;
    }

    g_window_server.composing = true;

    snapshot->window_count = 0U;
    snapshot->desktop_cache_ready = desktop_shell_cache_ready();
    snapshot->desktop = g_window_server.desktop;

    snapshot->theme = window_server_theme();
    snapshot->focused_identifier =
        g_window_server.focused_identifier;
    snapshot->title_pressed_identifier = g_window_server.title_pressed_identifier;
    snapshot->title_pressed_button = g_window_server.title_pressed_button;
    snapshot->hovered_window_identifier = g_window_server.scene.hovered_window_identifier;
    snapshot->hovered_title_button = g_window_server.scene.hovered_title_button;

    snapshot->pointer_x =
        g_window_server.pointer_x;

    snapshot->pointer_y =
        g_window_server.pointer_y;
    snapshot->cursor_shape = window_input_cursor_shape_locked();

    snapshot->desktop_hovered_app =
        g_window_server.desktop_hovered_app;

    snapshot->dragging_identifier =
        g_window_server.dragging_identifier;

    snapshot->drag_blit_valid =
        g_window_server.drag_blit_valid;
    window_animation_snapshot_locked();

    snapshot->drag_old_x =
        g_window_server.drag_old_x;

    snapshot->drag_old_y =
        g_window_server.drag_old_y;

    snapshot->drag_new_x =
        g_window_server.drag_new_x;

    snapshot->drag_new_y =
        g_window_server.drag_new_y;

    snapshot->drag_width =
        g_window_server.drag_width;

    snapshot->drag_height =
        g_window_server.drag_height;

    snapshot->damage_full_captured =
        g_window_server.damage_full;

    snapshot->damage_tiles_captured =
        !g_window_server.damage_full &&
        g_window_server.damage_tiles_active;

    if (snapshot->damage_full_captured) {
        snapshot->damage_count = 0U;
    } else if (snapshot->damage_tiles_captured) {
        snapshot->damage_count = 0U;

        for (uint32_t word = 0U;
             word < WINDOW_DAMAGE_TILE_WORDS;
             ++word) {
            snapshot->damage_tiles[word] =
                g_window_server.damage_tiles[word];
        }
    } else {
        snapshot->damage_count =
            g_window_server.damage_count;

        for (uint32_t index = 0U;
             index < snapshot->damage_count;
             ++index) {
            snapshot->damage_rects[index] =
                g_window_server.damage_rects[index];
        }
    }

    /*
    * 按 Z 顺序一次性捕获可见窗口。对象引用可确保释放锁后，复制得到的所有分区指针仍有效。
     */
    const Rect capture_bounds = compositor_registry_damage_bounds_locked();
    for (uint32_t index = 0U;
         index < g_window_server.scene.count;
         ++index) {

        window_server_window_t *window =
            g_window_server.scene.children[index];

        compositor_window_view_t *view;

        if (window == 0 ||
            window->minimized ||
            (window->flags & OS_WINDOW_VISIBLE) == 0U) {
            continue;
        }

        /*
         * 即使拖动窗口刚刚完全移出屏幕，也要保留它以记录已提交的几何信息。其他窗口至少
         * 必须与保守估算的生产者损坏范围相交。
         */
        if (window->identifier !=
                snapshot->dragging_identifier &&
            !compositor_registry_window_intersects_damage_bounds_locked(
                window, &capture_bounds)) {
            continue;
        }

        if (snapshot->window_count >=
            WINDOW_SERVER_MAX_WINDOWS) {
            break;
        }

        view =
            &snapshot->windows[
                snapshot->window_count++];

        compositor_capture_window_locked(window, view);
        window_animation_capture_locked(view);
    }

    /*
    * 立即轮换生产者状态。
     *
    * 此处不要清除 damage_tiles[]。damage_tiles_active 变为 false 后，位图便不可访问；
    * 下次复用前 enable_tiles 会将其清零。这样可从锁临界区中移除 256 次存储。
     */
    /*
    * 此代数据现在专属于不可变快照。解锁后的任何生产者都会看到干净状态，并发布新的边沿。
     */
    window_damage_rotate_locked();

    return true;
}


/*
 * P16 规划阶段。调用时不持有 window_lock。
 */
void compositor_snapshot_plan(void) {
    compositor_snapshot_t *snapshot =
        &g_compositor_snapshot;

    uint32_t original_window_count;
    uint32_t first_window_index;
    uint32_t write_index = 0U;

    if (snapshot->damage_full_captured) {
        snapshot->damage_count = 1U;

        snapshot->damage_rects[0] =
            (Rect){
                .x0 = 0U,
                .y0 = 0U,
                .x1 =
                    g_window_server.display_width,
                .y1 =
                    g_window_server.display_height,
            };

    } else if (snapshot->damage_tiles_captured) {

        snapshot->damage_count =
            compositor_snapshot_tiles_to_rects(
                snapshot,
                WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS);
    }

    if (snapshot->damage_count == 0U) {
        snapshot->damage_count = 1U;

        snapshot->damage_rects[0] =
            (Rect){
                .x0 = 0U,
                .y0 = 0U,
                .x1 =
                    g_window_server.display_width,
                .y1 =
                    g_window_server.display_height,
            };
    }

    if (!snapshot->drag_blit_valid) {
        compositor_collapse_drag_damage(
            snapshot);
    }

    first_window_index = snapshot->animations ? 0U :
        compositor_snapshot_occlusion_floor(
            snapshot);

    original_window_count =
        snapshot->window_count;

    for (uint32_t index = 0U;
         index < original_window_count;
         ++index) {

        compositor_window_view_t *view =
            &snapshot->windows[index];

        bool keep =
            index >= first_window_index &&
            (view->identifier ==
                 snapshot->dragging_identifier ||
             compositor_view_intersects_snapshot_damage(
                 view, snapshot));

        if (!keep) {
            compositor_release_window_snapshot(view);
            continue;
        }

        if (write_index != index) {
            snapshot->windows[write_index] =
                *view;

            view->reference = 0;
            view->controls = 0;
            view->caption = 0;
            view->section = 0;
        }

        ++write_index;
    }

    snapshot->window_count =
        write_index;

    /* 瓦片掩码编码此数组中的索引。必须在筛除和压缩后再构建，否则露出的下层窗口可能被
     * 误认为已移动的顶层窗口，导致其旧像素永远无法恢复。这样也能避免为遮挡下限淘汰的
     * 窗口计算掩码。 */
    (void)compositor_tile_metadata_build(snapshot);

    /*
    * 使用同一份瓦片掩码生成空间渲染计划；失败时执行精确遮挡检查。
     */
    if (!snapshot->animations) (void)compositor_build_render_plan(snapshot);

    snapshot->damage_full_captured =
        false;

    snapshot->damage_tiles_captured =
        false;
}

/* 仅让出调度并不能处理输入，因为合成器本身才是输入消费者。每个条带的所有图层绘制完成后
 * 才处理鼠标移动，避免光标从保留 RAM 中只绘制了一半的窗口/背景恢复。原生、拖动和
 * 直接显存事务仍沿用现有路径。 */
static void compositor_render_region_bounded(
    const compositor_render_span_t *span, bool service_motion) {
    Rect bounds = g_compositor_snapshot.damage_bounds;
    uint32_t width = bounds.x1 > bounds.x0 ?
        (uint32_t)((int64_t)bounds.x1 - bounds.x0) : 0U;
    uint32_t height = bounds.y1 > bounds.y0 ?
        (uint32_t)((int64_t)bounds.y1 - bounds.y0) : 0U;
    if (!service_motion || width == 0U || height == 0U ||
        (uint64_t)width * height <= WINDOW_COMPOSITOR_ATOMIC_PIXELS) {
        compositor_region_locked(span);
        /* 小区域同样可能连续出现；必须在完整图层边界服务输入，不能等整帧结束。 */
        if (service_motion && width != 0U && height != 0U)
            window_server_pump_publication_motion();
        return;
    }
    uint32_t rows = WINDOW_COMPOSITOR_ATOMIC_PIXELS / width;
    if (rows == 0U) rows = 1U;
    if (rows > 32U) rows = 32U;
    for (uint32_t top = bounds.y0; top < (uint32_t)bounds.y1;) {
        uint32_t count = (uint32_t)bounds.y1 - top;
        if (count > rows) count = rows;
        g_compositor_snapshot.damage_bounds.y0 = top;
        g_compositor_snapshot.damage_bounds.y1 = top + count;
        compositor_region_locked(span);
        top += count;
        window_server_pump_publication_motion();
    }
    g_compositor_snapshot.damage_bounds = bounds;
}

void compositor_render_snapshot(void) {
    bool direct_framebuffer =
        g_window_server.composite_framebuffer ==
        g_window_server.framebuffer;
    bool service_motion = !direct_framebuffer &&
        g_compositor_snapshot.dragging_identifier == 0U &&
        !display_core_has_native_scanout(0U);

    /*
     * 普通路径：
     *     合成帧缓冲区（RAM）-> 可抢占
     *     GOP 发布           -> 一个短暂的不可抢占事务
     *
     * 低内存回退路径没有私有合成缓冲区，因此必须保留原子渲染，避免暴露中间绘制状态。
     */
    if (direct_framebuffer) {
        sched_preempt_disable();
    }

    if (!direct_framebuffer) {
        compositor_blit_drag_overlap();
    }

    if (g_compositor_render_plan_valid) {
        /*
         * P19 热路径：遮挡处理和规划已经完成。
         */
        for (uint32_t index = 0U;
             index < g_compositor_render_plan_count;
             ++index) {

            const compositor_render_span_t *span =
                &g_compositor_render_plan[index];

            g_compositor_snapshot.damage_bounds.x0 =
                span->rect.x0;

            g_compositor_snapshot.damage_bounds.y0 =
                span->rect.y0;

            g_compositor_snapshot.damage_bounds.x1 =
                span->rect.x1;

            g_compositor_snapshot.damage_bounds.y1 =
                span->rect.y1;

            compositor_render_region_bounded(span, service_motion);
        }
    } else {
        /*
         * 简单帧或规划异常溢出：完全沿用 P18 路径。
         */
        for (uint32_t index = 0U;
             index < g_compositor_snapshot.damage_count;
             ++index) {

            const Rect *rect =
                &g_compositor_snapshot.damage_rects[index];

            g_compositor_snapshot.damage_bounds.x0 =
                rect->x0;

            g_compositor_snapshot.damage_bounds.y0 =
                rect->y0;

            g_compositor_snapshot.damage_bounds.x1 =
                rect->x1;

            g_compositor_snapshot.damage_bounds.y1 =
                rect->y1;

            compositor_render_region_bounded(0, service_motion);
        }
    }

    if (direct_framebuffer) {
        __asm__ volatile (
            "sfence"
            :
            :
            : "memory");

        sched_preempt_enable();
    } else {
        compositor_commit_snapshot();
#if OS_REALTEST
        if (!display_core_has_native_scanout(0U)) {
            compositor_report_display_probe();
        }
#endif

        /* GOP 路径预先发布光标包围盒，
         * 普通复制跳过该区域；这里只需处理发布后到达的指针变化。 */
        if (!display_core_has_native_scanout(0U)) {
            /* GOP 发布已先合成包围盒并跳过该区域，不再擦除后强制重画。 */
            compositor_present_cursor_direct(false);
        }
    }
}

void compositor_snapshot_finish(void) {
    _Static_assert(WINDOW_SERVER_MAX_WINDOWS <= 64U, "窗口 ACK 掩码容量不足");
    uint64_t present_ack_changed = 0U;
    /*
    * 此快照的场景渲染和发布均已完成。
     *
    * 允许后续 HID 拖动尝试复用保留内容前，先记录当前实际显示的精确几何信息。逻辑窗口
    * 状态可能已在另一 CPU 上前进，因此只有这些快照坐标可安全地作为 memmove 源。
     */
    window_lock();

    for (uint32_t index = 0U;
         index < g_compositor_snapshot.window_count;
         ++index) {

        compositor_window_view_t *view =
            &g_compositor_snapshot.windows[index];

        window_server_window_t *reference =
            view->reference;

        if (reference == 0 || g_compositor_publication_status != K_OK) {
            continue;
        }

        reference->compositor_presented_x =
            view->x;

        reference->compositor_presented_y =
            view->y;

        reference->compositor_presented_width =
            window_outer_width(
                view->width,
                view->flags);

        reference->compositor_presented_height =
            window_outer_height(
                view->height,
                view->flags);

        reference->compositor_presented_valid =
            true;
        window_animation_presented_locked(view);

        if (atomic_load_explicit(&reference->compositor_presented_sequence,
                                 memory_order_relaxed) != view->present_sequence)
            present_ack_changed |= 1ULL << index;
        atomic_store_explicit(
            &reference->compositor_presented_sequence,
            view->present_sequence,
            memory_order_release);
    }

    /*
    * drag_blit_valid 仅描述待处理的快照事务。提交后将其清除；下一次移动会根据
    * compositor_presented_* 以及 dirty/composing 状态独立判断是否可以复用。
     */
    if (g_compositor_publication_status != K_OK) {
        /* 恢复完整的生产者损坏区域；失败帧不会推进 ACK 或保留的显示几何状态，同时也要
         * 保留更新的生产者损坏信息。 */
        window_mark_dirty_locked();
    }
    window_input_set_drag_blit_valid_locked(false);

    g_window_server.composing = false;

    window_unlock();

    /*
    * 持有 window_lock 时不要获取窗口事件队列锁。被 window_event_read() 阻塞的客户端会
    * 在持有队列锁时检查条件，随后还需获取 window_lock；此处唤醒会反转锁顺序，并在新窗口
    * 打开时导致死锁。每个窗口先唤醒再释放快照引用，确保事件队列在唤醒期间有效；
    * 两项操作共用一次遍历，不必再次扫描所有窗口。
     */
    for (uint32_t index = 0U;
         index < g_compositor_snapshot.window_count;
         ++index) {
        compositor_window_view_t *view =
            &g_compositor_snapshot.windows[index];

        /* 此窗口的 PRESENT 栅栏未变化时，光标帧或其他窗口帧不得唤醒它的事件和 epoll
         * 订阅者。输入与关闭通知仍走各自独立的唤醒路径。 */
        if (view->reference != 0 && (present_ack_changed & (1ULL << index))) {
            (void)wake_all(&view->reference->event_waitq);
        }
        /* 引用释放仍在窗口锁外，避免销毁对象延长场景临界区。 */
        compositor_release_window_snapshot(view);
    }

    g_compositor_snapshot.window_count = 0U;
    g_compositor_snapshot.damage_count = 0U;

    compositor_render_plan_reset();

    /*
    * 此处不要清除生产者的 dirty/damage 状态。
     *
    * 此快照渲染期间，输入、调整大小、WINDOW_UPDATE 或进程退出可能已生成另一帧。
     */
}

void compositor_region_locked(
    const compositor_render_span_t *planned_span) {
    uint32_t damage_left =
        g_compositor_snapshot.damage_bounds.x0;

    uint32_t damage_top =
        g_compositor_snapshot.damage_bounds.y0;

    uint32_t damage_right =
        g_compositor_snapshot.damage_bounds.x1;

    uint32_t damage_bottom =
        g_compositor_snapshot.damage_bounds.y1;

    if (damage_left >= damage_right ||
        damage_top >= damage_bottom) {
        return;
    }

    /*
    * 桌面渲染现在也基于快照损坏区域、指针和悬停状态，不会读取可变场景状态。
     */
    uint32_t first_window = 0U;

    if (g_compositor_snapshot.animations) {
        desktop_draw_wallpaper_locked();
        planned_span = 0;
    } else if (planned_span != 0) {
        /*
         * P20 规划路径：是否需要渲染桌面已在开始渲染前确定。此处不再查询遮挡状态，也不再
         * 反向扫描 Z 顺序。
         */
        if (planned_span->first_window == UINT32_MAX) {
            desktop_draw_wallpaper_locked();
        }
    } else {
        /*
         * 简单帧/规划溢出回退路径完全沿用 P18。
         */
        first_window =
            compositor_occlusion_floor_for_damage();

        if (first_window == UINT32_MAX) {
            first_window =
                compositor_topmost_damage_cover();
        }

        if (first_window == UINT32_MAX) {
            desktop_draw_wallpaper_locked();
            first_window = 0U;
        }
    }


    if (planned_span != 0) {
        /*
         * P20 热路径。
         *
         * 启用 BMI 时，GCC/Clang 会将 __builtin_ctzll() 降低为 TZCNT，否则降低为 BSF。
         * mask &= mask-1 会移除已选窗口对应的位。始终处理最低置位，因此可保持 Z 顺序。
         */
        uint64_t mask =
            planned_span->window_mask;

        while (mask != 0U) {
            uint32_t index =
                (uint32_t)__builtin_ctzll(mask);

            mask &= mask - 1U;

            if (index >=
                g_compositor_snapshot.window_count) {
                continue;
            }

            compositor_animation_window(
                &g_compositor_snapshot.windows[index]);
        }

    } else {
        /*
         * P18 回退路径：仅简单帧或异常帧会在此执行候选扫描和逐窗口几何相交检查。
         */
        for (uint32_t index = first_window;
             index < g_compositor_snapshot.window_count;
             ++index) {

            const compositor_window_view_t *window =
                &g_compositor_snapshot.windows[index];

            if ((window->flags & OS_WINDOW_VISIBLE) == 0U ||
                (!g_compositor_snapshot.animations &&
                 !compositor_window_intersects_damage_locked(window))) {
                continue;
            }

            compositor_animation_window(
                window);
        }
    }

    compositor_animation_layers();
    desktop_draw_menu();

    /*
    * 私有 WB 合成不得绘制光标，以便拖动时复用这些像素。低内存直接帧缓冲回退路径无法
    * 保留干净场景，因此在那里继续使用旧的场景内光标绘制方式。
     */
    if (g_window_server.composite_framebuffer ==
        g_window_server.framebuffer) {
        compositor_cursor_locked();
    }

    /*
    * 暂时不要将此区域提供给扫描输出。
     *
    * 所有脏区域会先在 composite_framebuffer 中完成绘制。每个区域都准备就绪后，
    * compositor_commit_snapshot() 才会发布完整帧事务。
    */
}
