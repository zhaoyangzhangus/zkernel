#include <kernel/display.h>
#include <kernel/kmem.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include <kernel/vm.h>
#include "internal.h"

/* CPU 端保留表面合成的底层实现。 */

/*
 * 快速 WB -> WB 像素复制。
 *
 * 源数据来自通过物理直接映射访问的共享窗口后备页。目标为普通 RAM 中的合成帧缓冲区。
 *
 * REP MOVSQ 仅使用整数寄存器，适合复制连续扫描线跨度，且不会触及内核 SIMD/FPU 状态。
 */
void compositor_copy_wb_pixels(
    uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels) {

    uint64_t qwords;

    if (destination == 0 ||
        source == 0 ||
        pixels == 0U) {
        return;
    }

    qwords = pixels >> 1U;

    if (qwords != 0U) {
        void *out =
            (void *)destination;

        const void *in =
            (const void *)source;

        uint64_t count = qwords;

        __asm__ volatile (
            "rep movsq"
            : "+D"(out),
              "+S"(in),
              "+c"(count)
            :
            : "memory");
    }

    if ((pixels & 1U) != 0U) {
        destination[pixels - 1U] =
            source[pixels - 1U];
    }
}

bool compositor_copy_self_test(void) {
    uint64_t pixel_count64 =
        (uint64_t)g_window_server.display_width *
        g_window_server.display_height;
    uint32_t pixel_count;
    size_t bytes;
    uint32_t *source;
    uint32_t *destination;
    bool success = false;

    if (pixel_count64 == 0U || pixel_count64 > UINT32_MAX ||
        pixel_count64 > (uint64_t)SIZE_MAX / sizeof(uint32_t)) {
        return false;
    }

    pixel_count = (uint32_t)pixel_count64;
    bytes = (size_t)pixel_count * sizeof(uint32_t);
    /* 源缓冲逐像素初始化，目标由整屏复制完整覆盖，无需提前清零两次。 */
    source = (uint32_t *)kmalloc(bytes, 0);
    destination = (uint32_t *)kmalloc(bytes, 0);
    if (source == 0 || destination == 0) {
        if (source != 0) kfree(source);
        if (destination != 0) kfree(destination);
        return false;
    }

    for (uint32_t index = 0U; index < pixel_count; ++index) {
        source[index] = 0x00100000U | (index & 0x0000FFFFU);
    }

    /* 只反置验证点，防止复用的堆内容恰好匹配源数据而掩盖复制失败。 */
    destination[0] = ~source[0];
    destination[pixel_count / 2U] = ~source[pixel_count / 2U];
    destination[pixel_count - 1U] = ~source[pixel_count - 1U];

    uint64_t benchmark_start = telemetry_timestamp();
    compositor_copy_wb_pixels(destination, source, pixel_count);
    kernel_perf_emit_scope("graphics.fullscreen_copy", benchmark_start);
    kernel_perf_emit_value(
        "graphics.bytes_copied_frame",
        (uint64_t)pixel_count * sizeof(uint32_t));

    success = destination[0] == source[0] &&
              destination[pixel_count / 2U] == source[pixel_count / 2U] &&
              destination[pixel_count - 1U] == source[pixel_count - 1U];
    kfree(destination);
    kfree(source);
    return success;
}


/*
 * 填充私有保留场景。GOP 别名回退路径要求全局合成指针仍为 volatile；调用方仅在确认目标
 * 是普通可缓存 WB RAM 后才会进入此辅助函数。
 */
void compositor_fill_wb_pixels(
    uint32_t *destination,
    uint32_t pixels,
    uint32_t color) {

    uint64_t pair;
    uint64_t qwords;
    void *out;

    if (destination == 0 || pixels == 0U) {
        return;
    }

    if ((((uintptr_t)destination) & 7U) != 0U) {
        *destination++ = color;
        --pixels;
    }

    pair = (uint64_t)color | ((uint64_t)color << 32U);
    qwords = pixels >> 1U;
    out = (void *)destination;

    if (qwords != 0U) {
        uint64_t count = qwords;
        __asm__ volatile (
            "rep stosq"
            : "+D"(out), "+c"(count)
            : "a"(pair)
            : "memory");
    }

    if ((pixels & 1U) != 0U) {
        *(uint32_t *)out = color;
    }
}


/*
 * 快速填充裁剪后的表面背景跨度。
 *
 * 主要用于实时调整大小或无法解析共享后备页时。将两个 XRGB 像素打包到一次 64 位存储中。
 */
void compositor_fill_surface_pixels(
    volatile uint32_t *destination,
    uint32_t pixels,
    uint32_t color) {

    uint64_t pair;

    if (destination == 0 || pixels == 0U) {
        return;
    }

    /*
    * 使用 uint64_t 存储前先对齐目标地址。
     */
    if ((((uintptr_t)destination) & 7U) != 0U) {
        *destination++ = color;
        --pixels;
    }

    pair =
        (uint64_t)color |
        ((uint64_t)color << 32U);

    while (pixels >= 8U) {
        volatile uint64_t *out =
            (volatile uint64_t *)(void *)destination;

        out[0] = pair;
        out[1] = pair;
        out[2] = pair;
        out[3] = pair;

        destination += 8U;
        pixels -= 8U;
    }

    while (pixels >= 2U) {
        *(volatile uint64_t *)(void *)destination =
            pair;

        destination += 2U;
        pixels -= 2U;
    }

    if (pixels != 0U) {
        *destination = color;
    }
}


/*
 * 复制一段水平窗口表面跨度。
 *
 * 仅当跨度跨越 4K VM_OBJECT_SHARED 后备页时才会拆分。因此每约 1024 个 XRGB8888 像素
 * 查找一次页面，而非每个像素都查找。
 *
 * cached_page/cached_base 会跨扫描线保留，因此同一页面同时包含上一行尾部和下一行开头时，
 * 无需重复解析。
 */

/*
 * 窗口表面的常驻直接映射页表。
 *
 * VM_OBJECT_SHARED 页面在所属对象存续期间不会移动。合成器快照会持有活动窗口的引用，
 * 因此可在多帧合成期间安全复用有效的直接映射。
 *
 * 绝不缓存缺失页面。如果 Ring3 之后为新页面触发缺页，下一次合成器查找会看到空槽并正常
 * 解析该页面。
 */
struct compositor_surface_page_cache {
    shared_section_t *section;
    uint32_t page_count;
    uint32_t reserved;

    /*
    * 柔性数组：
     *
     *     pages[page_index] -> 内核直接映射地址
     *
    * NULL 表示“尚未解析”，而非“页面永久不存在”。
     */
    uint8_t *pages[];
};


/*
 * 返回或创建每个窗口对应的直接映射页表。
 *
 * 分配失败不会造成错误：合成器会直接回退到常规 VM 查找路径。
 */
compositor_surface_page_cache_t *
compositor_surface_cache_get(const compositor_window_view_t *view) {
    if (view == 0 || view->reference == 0 || view->section == 0 ||
        view->buffer_size == 0U) return 0;

    window_server_window_t *reference = view->reference;
    compositor_surface_page_cache_t *cache = reference->compositor_cache;
    /* 同一稳定分区直接复用页表；窗口快照持有引用，页面不会迁移。 */
    if (cache != 0 && cache->section == view->section) return cache;

    uint64_t bytes = reference->buffer_size;
    if (bytes == 0U) return 0;
    /* 减一后取整，避免向字节数加上页内偏移时溢出。 */
    uint64_t pages = ((bytes - 1U) >> PAGE_SHIFT) + 1U;
    if (pages > UINT32_MAX) return 0;

    if (cache != 0) {
        reference->compositor_cache = 0;
        kfree(cache);
    }

    /* 页数限制为 uint32_t，64 位 size_t 足以表示整个指针数组。 */
    cache = kzalloc(sizeof(*cache) + (size_t)pages * sizeof(cache->pages[0]), 0);
    if (cache == 0) return 0;
    cache->section = view->section;
    cache->page_count = (uint32_t)pages;
    reference->compositor_cache = cache;
    return cache;
}


/*
 * vm_object_shared_page_direct() 的缓存封装。
 *
 * 快速命中路径：
 *
 *     page_index -> 指针数组 -> 直接映射地址
 *
 * 不获取 VM 锁，也不遍历后备页链表。
 */

/*
 * 表面通常按升序跨越共享页进行渲染。一旦确认下一页已有有效的直接映射缓存项，就在当前页
 * 耗尽前将下一页的首条缓存行预取到 L1。
 *
 * 这只是性能提示。此处绝不解析或分配缺失页面。
 */
static inline void compositor_prefetch_surface_page(
    const compositor_surface_page_cache_t *cache,
    uint64_t page_index) {

    uint8_t *next;

    if (cache == 0 ||
        page_index + 1U >= cache->page_count) {
        return;
    }

    next =
        cache->pages[page_index + 1U];

    if (next == 0) {
        return;
    }

    __asm__ volatile (
        "prefetcht0 (%0)"
        :
        : "r"(next));
}


/*
 * 每次窗口/损坏区域处理时，仅解析一次行间不变的表面状态。
 * 对较窄的保留内容拖动条带尤其重要：即使每行只复制一两个像素，相邻行也可能访问不同的
 * 4K 页面。
 */


/*
 * 有效的缓存命中：
 *     cache->pages[page_index]
 * 不必针对每页重新读取 reference->compositor_cache 或比较分区。
 */
uint8_t *compositor_surface_page_resolve(
    const compositor_surface_source_context_t *context,
    uint64_t page_index) {

    compositor_surface_page_cache_t *cache;
    uint8_t *base = 0;

    if (context == 0 || !context->readable || context->object == 0) {
        return 0;
    }

    cache = context->cache;

    if (cache != 0 && page_index < cache->page_count) {
        base = cache->pages[page_index];
        if (base != 0) {
            compositor_prefetch_surface_page(cache, page_index);
            return base;
        }
    }

    if (page_index > (UINT64_MAX >> PAGE_SHIFT)) return 0;

    if (vm_object_shared_page_direct(
            context->object,
            page_index << PAGE_SHIFT,
            true,
            &base) != K_OK ||
        base == 0) {
        return 0;
    }

    if (cache != 0 && page_index < cache->page_count) {
        cache->pages[page_index] = base;
        compositor_prefetch_surface_page(cache, page_index);
    }

    return base;
}


void compositor_surface_fill_destination(
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    uint32_t pixels,
    uint32_t color) {

    if (wb_destination != 0) {
        compositor_fill_wb_pixels(wb_destination, pixels, color);
    } else {
        compositor_fill_surface_pixels(device_destination, pixels, color);
    }
}


void compositor_surface_copy_destination(
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    const uint32_t *source,
    uint32_t pixels) {

    if (wb_destination != 0) {
        compositor_copy_wb_pixels(wb_destination, source, pixels);
    } else {
        display_core_publish_xrgb8888_span(
            device_destination, source, pixels);
    }
}


/*
 * source_row_offset 由调用方增量维护。常规热路径现在只需计算页索引并查询直接映射页数组。
 * 常规热路径现在只需计算页索引并查询直接映射页数组。
 */

