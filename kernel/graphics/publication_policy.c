#include <kernel/sched.h>
#include <arch/x86_64/cpu.h>
#include "internal.h"

/* REFACTOR_P7A_PUBLICATION_OWNER：损坏区域和扫描输出发布策略。 */

uint64_t compositor_publication_damage_pixels(void) {
    uint64_t pixels = 0U;

    for (uint32_t region = 0U;
         region < g_compositor_snapshot.damage_count;
         ++region) {
        const Rect *rect =
            &g_compositor_snapshot.damage_rects[region];
        uint32_t width;
        uint32_t height;
        uint64_t area;

        if (rect->x0 >= rect->x1 || rect->y0 >= rect->y1) continue;
        /* 在宽类型中求差，避免极端有符号坐标在面积统计时溢出。 */
        width = (uint32_t)((int64_t)rect->x1 - rect->x0);
        height = (uint32_t)((int64_t)rect->y1 - rect->y0);
        area = (uint64_t)width * height;
        if (UINT64_MAX - pixels < area) return UINT64_MAX;
        pixels += area;
    }
    return pixels;
}

/* 发布一个损坏矩形。调用方在发布期间关闭一次抢占。较大的常规更新按预算检查输入和调度需求，
 * 使新到的 HID 报告能够运行；拖动/移动帧通过 allow_yield=false 保持单次原子事务。 */
typedef struct compositor_publication_budget {
    uint64_t pixels;
    uint64_t start;
    uint64_t duration;
    bool started;
} compositor_publication_budget_t;

static void compositor_publication_rect_budget(const Rect *rect,
    bool allow_yield, bool final_rect, compositor_publication_budget_t *budget) {
    Rect clipped;
    uint32_t width;

    if (rect == 0 || g_window_server.framebuffer == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.display_stride < g_window_server.display_width) {
        return;
    }
    /* 即使上游损坏区域生产者保留了旧几何坐标，最终发布阶段仍负责裁剪。
     * 将有符号位置转换为扫描线索引或计算宽度前先裁剪。 */
    if (!rect_clip_to_extent(rect->x0, rect->y0, rect->x1, rect->y1,
            g_window_server.display_width, g_window_server.display_height,
            &clipped)) return;
    rect = &clipped;
    window_present_cursor_prepare_rect(rect);

    width =
        rect->x1 -
        rect->x0;

    if (allow_yield && !budget->started) {
        budget->start = x86_read_tsc();
        budget->duration = x86_timeout_ns_to_tsc(1000000ULL);
        budget->started = true;
    }

    for (uint32_t row = (uint32_t)rect->y0;
         row < (uint32_t)rect->y1;
         ++row) {
        volatile uint32_t *destination =
            g_window_server.framebuffer +
            (uint64_t)row * g_window_server.display_stride +
            rect->x0;

        /*
         * 此路径保证 composite_framebuffer 是私有保留式 WB 场景；直接帧缓冲后备路径
         * 不会调用 compositor_commit_snapshot()。
         */
        const uint32_t *source =
            (const uint32_t *)(uintptr_t)(
                g_window_server.composite_framebuffer +
                (uint64_t)row * g_window_server.display_stride +
                rect->x0);

        if (!window_present_cursor_publish_span(
                destination,
                source,
                rect->x0,
                row,
                width)) {
            compositor_copy_wc_scanline(
                destination,
                source,
                width);
        }

        if (!allow_yield) {
            continue;
        }

        budget->pixels +=
            width;

        if ((final_rect && row + 1U >= (uint32_t)rect->y1) ||
            (budget->pixels < WINDOW_COMPOSITOR_PUBLICATION_YIELD_PIXELS &&
             x86_read_tsc() - budget->start < budget->duration)) {
            continue;
        }

        /* 没有调度请求或输入时继续完成当前帧，避免主动切走后长期暴露半更新画面。
         * 检查预算保持有界，定时器/唤醒发布抢占请求后仍能及时交出 CPU。 */
        if (!sched_tick_should_preempt() && input_core_pending() == 0U) {
            budget->pixels = 0U;
            budget->start = x86_read_tsc();
            continue;
        }

        /*
         * 按已复制的工作量检查响应需求，而非按行数。狭窄的 64 像素损坏矩形不再每 16 行调用
         * 一次 schedule()，而完整的 2560 像素行也会相应缩短处理量。
         *
         * 在调度点前刷新此 CPU 的 WC 存储：工作线程可能在另一 CPU 上恢复执行。每条扫描线
         * 已保留可见光标，包括 QEMU 固定页路径。调度点前后仍保持不可变场景和单一合成器所有者。
         */
        __asm__ volatile ("sfence" : : : "memory");
        sched_preempt_enable();
        /* 仅让出 CPU 无法清空输入队列，因为输入消费者就是当前线程。
         * 使用已完成的保留场景处理 FIFO 队列前缀中的移动事件。 */
        window_server_pump_publication_motion();
        /* 输入处理可能新唤醒线程；在允许抢占的状态下重新检查实际调度请求。 */
        if (sched_tick_should_preempt()) schedule();
        sched_preempt_disable();

        budget->pixels = 0U;
        budget->start = x86_read_tsc();
    }
}

void compositor_publication_rect(const Rect *rect, bool allow_yield) {
    compositor_publication_budget_t budget = {0};
    compositor_publication_rect_budget(rect, allow_yield, true, &budget);
}

/* 整批矩形共用预算，避免大量小块各自重置计数而长期推迟输入处理与抢占。 */
void compositor_publication_rects(const Rect *rects, uint32_t count,
    bool allow_yield) {
    compositor_publication_budget_t budget = {0};
    if (rects == 0) return;
    for (uint32_t index = 0U; index < count; ++index) {
        compositor_publication_rect_budget(&rects[index], allow_yield,
            index + 1U == count, &budget);
    }
}

/*
 * 从已完成的保留 WB 场景中裁剪并发布一个矩形。
 *
 * 此辅助函数仅用于下方 GOP 拖动残影抑制预处理。之后仍会执行常规事务，并以其结果作为最终
 * 可见帧的依据。
 */
static void compositor_publication_drag_exposure_rect(
    int64_t left,
    int64_t top,
    int64_t right,
    int64_t bottom) {

    Rect rect;

    if (!rect_clip_to_extent(left, top, right, bottom,
            g_window_server.display_width, g_window_server.display_height,
            &rect)) return;

    compositor_publication_rect(
        &rect,
        false);
}


/*
 * GOP 不支持翻页：CPU 将大型窗口移动事务复制到可见帧缓冲区时，显示引擎仍可能同时扫描它。
 *
 * 视觉上最严重的瑕疵，是数 MB 的事务仍在进行时，OLD\NEW 尾边依然可见。保留的 WB 场景中
 * 已经包含该处最终的背景/下层窗口像素。
 *
 * 先只发布旧位置上暴露的条带。高度为 800 像素的窗口移动 1 像素，仅增加约 3.2 KiB 的 WC
 * 流量，却能在完整事务开始前移除旧轮廓边缘。
 *
 * 这并不能让 GOP 实现原子更新；完整方案仍是原生扫描输出/翻页。这只是针对 GOP 的低成本
 * 正确性改进。
 */
void compositor_publication_drag_old_exposure(void) {
    const compositor_snapshot_t *snapshot =
        &g_compositor_snapshot;

    int64_t old_left;
    int64_t old_top;
    int64_t old_right;
    int64_t old_bottom;

    int64_t new_left;
    int64_t new_top;
    int64_t new_right;
    int64_t new_bottom;

    int64_t overlap_left;
    int64_t overlap_top;
    int64_t overlap_right;
    int64_t overlap_bottom;

    if (!snapshot->drag_blit_valid ||
        snapshot->drag_width == 0U ||
        snapshot->drag_height == 0U) {
        return;
    }

    old_left = snapshot->drag_old_x;
    old_top = snapshot->drag_old_y;
    old_right =
        old_left + (int64_t)snapshot->drag_width;
    old_bottom =
        old_top + (int64_t)snapshot->drag_height;

    new_left = snapshot->drag_new_x;
    new_top = snapshot->drag_new_y;
    new_right =
        new_left + (int64_t)snapshot->drag_width;
    new_bottom =
        new_top + (int64_t)snapshot->drag_height;

    overlap_left =
        old_left > new_left ?
            old_left :
            new_left;

    overlap_top =
        old_top > new_top ?
            old_top :
            new_top;

    overlap_right =
        old_right < new_right ?
            old_right :
            new_right;

    overlap_bottom =
        old_bottom < new_bottom ?
            old_bottom :
            new_bottom;

    if (overlap_left >= overlap_right ||
        overlap_top >= overlap_bottom) {

        compositor_publication_drag_exposure_rect(
            old_left,
            old_top,
            old_right,
            old_bottom);

        return;
    }

    /* OLD \ NEW 横向条带，覆盖旧矩形的完整高度。 */
    if (new_left > old_left) {
        compositor_publication_drag_exposure_rect(
            old_left,
            old_top,
            overlap_left,
            old_bottom);
    } else if (new_left < old_left) {
        compositor_publication_drag_exposure_rect(
            overlap_right,
            old_top,
            old_right,
            old_bottom);
    }

    /*
    * OLD \ NEW 纵向条带仅覆盖横向重叠范围，因此不会与上方横向条带重复发布。
     */
    if (new_top > old_top) {
        compositor_publication_drag_exposure_rect(
            overlap_left,
            old_top,
            overlap_right,
            overlap_top);
    } else if (new_top < old_top) {
        compositor_publication_drag_exposure_rect(
            overlap_left,
            overlap_bottom,
            overlap_right,
            old_bottom);
    }
}

