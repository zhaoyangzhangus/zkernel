#include <kernel/console_backend.h>
#include <kernel/console.h>
#include <kernel/realtest.h>
#include <kernel/sched.h>

#include <arch/x86_64/cpu.h>

#include <console_font_a8.h>
#include <stdint.h>
#include <stdatomic.h>

#ifndef OS_DEBUG_SERIAL
#define OS_DEBUG_SERIAL 0
#endif

/* 控制台是固定单元格终端。字形源数据以 A8 格式在编译期生成，因此启动日志不依赖 VFS 或 TTF 解析器。 */
#define GOP_DEBUG_GLYPH_WIDTH  OS_CONSOLE_FONT_WIDTH
#define GOP_DEBUG_GLYPH_HEIGHT OS_CONSOLE_FONT_HEIGHT
#define GOP_DEBUG_TAB_SPACES   4U
#define GOP_DEBUG_MAX_COLUMNS  512U
#define GOP_DEBUG_MAX_ROWS     128U
#define GOP_DEBUG_BUFFER_CELLS (GOP_DEBUG_MAX_COLUMNS * GOP_DEBUG_MAX_ROWS)

typedef struct gop_debug_console {
    volatile UINT32 *framebuffer;
    UINT32 width;
    UINT32 height;
    UINT32 pixels_per_scanline;
    UINT32 format;
    UINT32 masks[4];
    UINT32 columns;
    UINT32 rows;
    UINT32 cursor_column;
    UINT32 cursor_row;
    UINT32 foreground;
    UINT32 background;
    UINT32 dirty_first_row;
    UINT32 dirty_last_row;
    BOOLEAN buffer_initialized;
    BOOLEAN dirty;
    BOOLEAN full_redraw;
} gop_debug_console_t;

static gop_debug_console_t g_gop_debug_console;
static atomic_uint g_gop_debug_console_ready;
static atomic_flag g_gop_debug_console_lock = ATOMIC_FLAG_INIT;
static UINT8 g_gop_debug_text_buffer[GOP_DEBUG_BUFFER_CELLS];
static UINT32 g_gop_debug_color_buffer[GOP_DEBUG_BUFFER_CELLS];

#if OS_DEBUG_SERIAL
/* COM1 按字节串行工作，并由所有 CPU 共用。每次诊断调用必须保持连续；否则 SMP 工作线程会交错输出字节，
 * 破坏 QEMU 测试使用的故障标记。 */
static atomic_flag g_serial_lock = ATOMIC_FLAG_INIT;
static BOOLEAN g_serial_line_open;
/* 静态存储可确保两个原子变量都正确初始化为零，兼容不提供 ATOMIC_VAR_INIT 的独立运行环境 stdatomic.h。
 * 并发启用前会立即设置所有者哨兵。 */
static atomic_uint g_serial_owner_cpu;
static atomic_uint g_serial_concurrency_ready;

static UINT64 serial_lock(BOOLEAN *owned) {
    UINT64 flags;
    uint32_t cpu;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    __asm__ volatile ("cli" : : : "memory");

    /* 架构 CPU 本地存储建立前，整个写入期间都要关闭中断。SMP 启用后，获取所有权后也要继续关闭中断：
     * IRQ 不得将诊断字节插入另一 CPU 正在输出的记录。发生竞争的 CPU 会在尝试获取锁之间短暂开启中断，
     * 以便等待期间 TLB/IPI 仍可送达。 */
    if (atomic_load_explicit(&g_serial_concurrency_ready,
                             memory_order_acquire) == 0U) {
        *owned = true;
        while (atomic_flag_test_and_set_explicit(&g_serial_lock,
                                                 memory_order_acquire)) {
            __asm__ volatile ("pause");
        }
        return flags;
    }

    cpu = x86_current_cpu_index();
    if (cpu != UINT32_MAX &&
        atomic_load_explicit(&g_serial_owner_cpu,
                             memory_order_acquire) == cpu) {
        *owned = false;
        return flags;
    }
    for (;;) {
        if (!atomic_flag_test_and_set_explicit(&g_serial_lock,
                                               memory_order_acquire)) {
            break;
        }
        /* 等待共享 COM1 锁时保持 TLB/IPI 可送达。页表所有者可能正等待此 CPU 确认，之后才能释放自己的
         * 映射锁。 */
        if ((flags & (1ULL << 9)) != 0U) {
            __asm__ volatile ("sti" : : : "memory");
        }
        __asm__ volatile ("pause");
        /* 获取锁的操作本身必须在关闭中断时执行；否则成功完成原子交换后、中断屏蔽前的指令窗口中可能进入
         * IRQ，并因争用同一把锁而死锁。 */
        __asm__ volatile ("cli" : : : "memory");
    }
    atomic_store_explicit(&g_serial_owner_cpu, cpu, memory_order_release);
    *owned = true;
    return flags;
}

static void serial_unlock(UINT64 flags, BOOLEAN owned) {
    if (!owned) {
        if ((flags & (1ULL << 9)) != 0U) {
            __asm__ volatile ("sti" : : : "memory");
        }
        return;
    }
    __asm__ volatile ("cli" : : : "memory");
    atomic_store_explicit(&g_serial_owner_cpu, UINT32_MAX,
                          memory_order_release);
    atomic_flag_clear_explicit(&g_serial_lock, memory_order_release);
    if ((flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    }
}
#endif

static UINT32 gop_debug_component(UINT8 value, UINT32 mask) {
    UINT32 shift = 0U;
    UINT32 width = 0U;
    UINT32 maximum;
    if (mask == 0U) return 0U;
    while (shift < 32U && ((mask >> shift) & 1U) == 0U) ++shift;
    while (shift + width < 32U && ((mask >> (shift + width)) & 1U) != 0U) {
        ++width;
    }
    if (width == 0U || width >= 32U) return mask;
    maximum = (1U << width) - 1U;
    return ((((UINT32)value * maximum + 127U) / 255U) << shift) & mask;
}

static UINT32 gop_debug_color(UINT32 pixel) {
    UINT8 red = (UINT8)(pixel >> 16);
    UINT8 green = (UINT8)(pixel >> 8);
    UINT8 blue = (UINT8)pixel;
    if (g_gop_debug_console.format == OS_PIXEL_BGRR) {
        return (UINT32)blue | ((UINT32)green << 8) | ((UINT32)red << 16);
    }
    if (g_gop_debug_console.format == OS_PIXEL_BITMASK) {
        return gop_debug_component(red, g_gop_debug_console.masks[0]) |
               gop_debug_component(green, g_gop_debug_console.masks[1]) |
               gop_debug_component(blue, g_gop_debug_console.masks[2]);
    }
    return pixel & 0x00FFFFFFU;
}

static UINT32 gop_debug_blend(UINT32 foreground, UINT32 background,
                               UINT8 alpha) {
    UINT32 inverse = 255U - alpha;
    UINT32 red = (((foreground >> 16U) & 0xFFU) * alpha +
                  ((background >> 16U) & 0xFFU) * inverse + 127U) / 255U;
    UINT32 green = (((foreground >> 8U) & 0xFFU) * alpha +
                    ((background >> 8U) & 0xFFU) * inverse + 127U) / 255U;
    UINT32 blue = ((foreground & 0xFFU) * alpha +
                   (background & 0xFFU) * inverse + 127U) / 255U;
    return (red << 16U) | (green << 8U) | blue;
}

static void gop_debug_mark_row_locked(UINT32 row) {
    if (row >= g_gop_debug_console.rows) return;
    if (!g_gop_debug_console.dirty) {
        g_gop_debug_console.dirty_first_row = row;
        g_gop_debug_console.dirty_last_row = row + 1U;
        g_gop_debug_console.dirty = 1;
        return;
    }
    if (row < g_gop_debug_console.dirty_first_row) {
        g_gop_debug_console.dirty_first_row = row;
    }
    if (row + 1U > g_gop_debug_console.dirty_last_row) {
        g_gop_debug_console.dirty_last_row = row + 1U;
    }
}

static void gop_debug_mark_full_locked(void) {
    g_gop_debug_console.dirty_first_row = 0U;
    g_gop_debug_console.dirty_last_row = g_gop_debug_console.rows;
    g_gop_debug_console.dirty = 1;
    g_gop_debug_console.full_redraw = 1;
}

static void gop_debug_clear_text_locked(void) {
    UINT32 cells = g_gop_debug_console.columns * g_gop_debug_console.rows;
    for (UINT32 index = 0U; index < cells; ++index) {
        g_gop_debug_text_buffer[index] = ' ';
        g_gop_debug_color_buffer[index] = g_gop_debug_console.foreground;
    }
}

static void gop_debug_fill_rows_locked(UINT32 first_row, UINT32 last_row) {
    UINT32 color = gop_debug_color(g_gop_debug_console.background);
    if (last_row > g_gop_debug_console.height) {
        last_row = g_gop_debug_console.height;
    }
    for (UINT32 y = first_row; y < last_row; ++y) {
        volatile UINT32 *row = g_gop_debug_console.framebuffer +
            (UINT64)y * g_gop_debug_console.pixels_per_scanline;
        for (UINT32 x = 0U; x < g_gop_debug_console.width; ++x) {
            row[x] = color;
        }
    }
}

static void gop_debug_scroll_locked(void) {
    UINT32 row_width = g_gop_debug_console.columns;
    UINT32 rows = g_gop_debug_console.rows;

    if (rows <= 1U) {
        gop_debug_clear_text_locked();
        gop_debug_mark_full_locked();
        g_gop_debug_console.cursor_row = 0U;
        return;
    }
    for (UINT32 row = 1U; row < rows; ++row) {
        UINT32 source = (row - 1U) * row_width;
        UINT32 destination = row * row_width;
        for (UINT32 column = 0U; column < row_width; ++column) {
            g_gop_debug_text_buffer[destination + column] =
                g_gop_debug_text_buffer[source + column];
            g_gop_debug_color_buffer[destination + column] =
                g_gop_debug_color_buffer[source + column];
        }
    }
    UINT32 last = (rows - 1U) * row_width;
    for (UINT32 column = 0U; column < row_width; ++column) {
        g_gop_debug_text_buffer[last + column] = ' ';
        g_gop_debug_color_buffer[last + column] =
            g_gop_debug_console.foreground;
    }
    g_gop_debug_console.cursor_row = rows - 1U;
    gop_debug_mark_full_locked();
}

static void gop_debug_newline_locked(void) {
    g_gop_debug_console.cursor_column = 0U;
    if (g_gop_debug_console.cursor_row + 1U >= g_gop_debug_console.rows) {
        gop_debug_scroll_locked();
    } else {
        ++g_gop_debug_console.cursor_row;
    }
}

static void gop_debug_put_char_locked(UINT8 character) {
    UINT32 index;

    if (g_gop_debug_console.cursor_row >= g_gop_debug_console.rows ||
        g_gop_debug_console.cursor_column >= g_gop_debug_console.columns) {
        return;
    }
    index = g_gop_debug_console.cursor_row * g_gop_debug_console.columns +
            g_gop_debug_console.cursor_column;
    g_gop_debug_text_buffer[index] = character;
    g_gop_debug_color_buffer[index] = g_gop_debug_console.foreground;
    gop_debug_mark_row_locked(g_gop_debug_console.cursor_row);
    ++g_gop_debug_console.cursor_column;
    if (g_gop_debug_console.cursor_column >= g_gop_debug_console.columns) {
        gop_debug_newline_locked();
    }
}

static void gop_debug_draw_cell_locked(UINT32 column, UINT32 row) {
    UINT32 cell = row * g_gop_debug_console.columns + column;
    const UINT8 *glyph = kernel_console_font_glyph(
        g_gop_debug_text_buffer[cell]);
    UINT32 x = column * GOP_DEBUG_GLYPH_WIDTH;
    UINT32 y = row * GOP_DEBUG_GLYPH_HEIGHT;
    UINT32 foreground = g_gop_debug_color_buffer[cell];

    for (UINT32 glyph_row = 0U; glyph_row < GOP_DEBUG_GLYPH_HEIGHT;
         ++glyph_row) {
        volatile UINT32 *destination = g_gop_debug_console.framebuffer +
            (UINT64)(y + glyph_row) *
                g_gop_debug_console.pixels_per_scanline + x;
        for (UINT32 glyph_column = 0U; glyph_column < GOP_DEBUG_GLYPH_WIDTH;
             ++glyph_column) {
            UINT8 alpha = glyph[glyph_row * GOP_DEBUG_GLYPH_WIDTH +
                                glyph_column];
            UINT32 pixel = alpha == 0U ? g_gop_debug_console.background :
                gop_debug_blend(foreground, g_gop_debug_console.background,
                                alpha);
            destination[glyph_column] = gop_debug_color(pixel);
        }
    }
}

static void gop_debug_render_locked(void) {
    UINT32 first_row;
    UINT32 last_row;

    if (!g_gop_debug_console.dirty) return;
    if (g_gop_debug_console.full_redraw) {
        gop_debug_fill_rows_locked(0U, g_gop_debug_console.height);
    }
    first_row = g_gop_debug_console.dirty_first_row;
    last_row = g_gop_debug_console.dirty_last_row;
    if (last_row > g_gop_debug_console.rows) last_row = g_gop_debug_console.rows;
    for (UINT32 row = first_row; row < last_row; ++row) {
        for (UINT32 column = 0U; column < g_gop_debug_console.columns;
             ++column) {
            gop_debug_draw_cell_locked(column, row);
        }
    }
    __asm__ volatile ("sfence" : : : "memory");
    g_gop_debug_console.dirty = 0;
    g_gop_debug_console.full_redraw = 0;
}

static void gop_debug_write_locked(const CHAR8 *text) {
    while (text != 0 && *text != 0) {
        UINT8 character = (UINT8)*text++;
        if (character == '\r') {
            g_gop_debug_console.cursor_column = 0U;
        } else if (character == '\n') {
            gop_debug_newline_locked();
        } else if (character == '\t') {
            for (UINT32 i = 0U; i < GOP_DEBUG_TAB_SPACES; ++i) {
                gop_debug_put_char_locked(' ');
            }
        } else if (character == '\b') {
            if (g_gop_debug_console.cursor_column != 0U) {
                UINT32 index;
                --g_gop_debug_console.cursor_column;
                index = g_gop_debug_console.cursor_row *
                        g_gop_debug_console.columns +
                        g_gop_debug_console.cursor_column;
                g_gop_debug_text_buffer[index] = ' ';
                g_gop_debug_color_buffer[index] =
                    g_gop_debug_console.foreground;
                gop_debug_mark_row_locked(g_gop_debug_console.cursor_row);
            }
        } else {
            if (character < OS_CONSOLE_FONT_FIRST ||
                character > OS_CONSOLE_FONT_LAST) character = '?';
            gop_debug_put_char_locked(character);
        }
    }
    /* 所有字符/颜色缓冲区准备完成后，才开始写入 GOP。 */
    gop_debug_render_locked();
}

static BOOLEAN gop_debug_lock(BOOLEAN allow_wait) {
    if (!allow_wait) {
        return atomic_flag_test_and_set_explicit(&g_gop_debug_console_lock,
                                                 memory_order_acquire) == 0;
    }
    while (atomic_flag_test_and_set_explicit(&g_gop_debug_console_lock,
                                             memory_order_acquire)) {
        __asm__ volatile ("pause");
    }
    return 1;
}

static void gop_debug_unlock(void) {
    atomic_flag_clear_explicit(&g_gop_debug_console_lock, memory_order_release);
}

BOOLEAN kernel_console_init(const OS_BOOT_INFO *info,
                            UINT64 framebuffer_virtual) {
    UINT64 required_pixels;
    UINT32 columns;
    UINT32 rows;
    UINT32 old_columns;
    UINT32 old_rows;
    BOOLEAN reset_buffer;
    if (info == 0 || framebuffer_virtual == 0U || info->FrameBufferWidth == 0U ||
        info->FrameBufferHeight == 0U ||
        info->FrameBufferPixelsPerScanLine < info->FrameBufferWidth ||
        info->FrameBufferFormat > OS_PIXEL_BITMASK ||
        info->FrameBufferWidth < GOP_DEBUG_GLYPH_WIDTH ||
        info->FrameBufferHeight < GOP_DEBUG_GLYPH_HEIGHT) {
        return 0;
    }
    required_pixels = (UINT64)info->FrameBufferPixelsPerScanLine * info->FrameBufferHeight;
    if (required_pixels > UINT64_MAX / sizeof(UINT32) ||
        required_pixels * sizeof(UINT32) > info->FrameBufferSize) {
        return 0;
    }

    columns = info->FrameBufferWidth / GOP_DEBUG_GLYPH_WIDTH;
    rows = info->FrameBufferHeight / GOP_DEBUG_GLYPH_HEIGHT;
    if (columns == 0U || rows == 0U) return 0;
    if (columns > GOP_DEBUG_MAX_COLUMNS) columns = GOP_DEBUG_MAX_COLUMNS;
    if (rows > GOP_DEBUG_MAX_ROWS) rows = GOP_DEBUG_MAX_ROWS;

    g_gop_debug_console.framebuffer =
        (volatile UINT32 *)(uintptr_t)framebuffer_virtual;
    g_gop_debug_console.width = info->FrameBufferWidth;
    g_gop_debug_console.height = info->FrameBufferHeight;
    g_gop_debug_console.pixels_per_scanline = info->FrameBufferPixelsPerScanLine;
    g_gop_debug_console.format = info->FrameBufferFormat;
    for (UINT32 i = 0U; i < 4U; ++i) g_gop_debug_console.masks[i] = info->FrameBufferMask[i];
    old_columns = g_gop_debug_console.columns;
    old_rows = g_gop_debug_console.rows;
    g_gop_debug_console.foreground = 0x00F2F5F7U;
    g_gop_debug_console.background = 0x00081018U;
    reset_buffer = !g_gop_debug_console.buffer_initialized;
    if (!reset_buffer && (old_columns != columns || old_rows != rows)) {
        reset_buffer = 1;
    }
    g_gop_debug_console.columns = columns;
    g_gop_debug_console.rows = rows;
    if (reset_buffer) {
        gop_debug_clear_text_locked();
        g_gop_debug_console.cursor_column = 0U;
        g_gop_debug_console.cursor_row = 0U;
        g_gop_debug_console.buffer_initialized = 1;
    }
    gop_debug_mark_full_locked();
    gop_debug_render_locked();
    atomic_store_explicit(&g_gop_debug_console_ready, 1U, memory_order_release);
    return 1;
}

BOOLEAN kernel_console_init_early(const OS_BOOT_INFO *info) {
    if (info == 0 || info->FrameBufferBase > 0xFFFFFFFFULL ||
        info->FrameBufferSize > 0x100000000ULL - info->FrameBufferBase) {
        return 0;
    }
    return kernel_console_init(info, info->FrameBufferBase);
}

void kernel_console_refresh(void) {
    if (atomic_load_explicit(&g_gop_debug_console_ready,
                             memory_order_acquire) == 0U) {
        return;
    }
    sched_preempt_disable();
    (void)gop_debug_lock(1);
    gop_debug_mark_full_locked();
    gop_debug_render_locked();
    gop_debug_unlock();
    sched_preempt_enable();
}

void kernel_console_set_color(uint32_t color) {
    if (atomic_load_explicit(&g_gop_debug_console_ready,
                             memory_order_acquire) == 0U) {
        return;
    }
    sched_preempt_disable();
    (void)gop_debug_lock(1);
    g_gop_debug_console.foreground = color & 0x00FFFFFFU;
    gop_debug_unlock();
    sched_preempt_enable();
}

void kernel_console_disable(void) {
    if (atomic_load_explicit(&g_gop_debug_console_ready,
                             memory_order_acquire) == 0U) {
        return;
    }
    sched_preempt_disable();
    (void)gop_debug_lock(1);
    atomic_store_explicit(&g_gop_debug_console_ready, 0U,
                          memory_order_release);
    gop_debug_unlock();
    sched_preempt_enable();
}

#if OS_DEBUG_SERIAL
static void serial_out(UINT16 port, UINT8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static UINT8 serial_in(UINT16 port) {
    UINT8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
#endif

void kernel_serial_guard_enter(kernel_serial_guard_t *guard) {
    if (guard == 0) return;
#if OS_DEBUG_SERIAL
    guard->preempt_disabled =
        atomic_load_explicit(&g_serial_concurrency_ready,
                             memory_order_acquire) != 0U;
    if (guard->preempt_disabled) sched_preempt_disable();
    guard->owned = false;
    guard->flags = serial_lock(&guard->owned);
#else
    guard->flags = 0U;
    guard->owned = false;
    guard->preempt_disabled = false;
#endif
}

void kernel_serial_guard_leave(kernel_serial_guard_t *guard) {
    if (guard == 0) return;
#if OS_DEBUG_SERIAL
    serial_unlock(guard->flags, guard->owned);
    if (guard->preempt_disabled) sched_preempt_enable();
#endif
}

void kernel_serial_write_guarded(const char *text) {
    if (text == 0) return;
#if OS_DEBUG_SERIAL
    while (*text != 0) {
        for (UINTN tries = 0; tries < 100000U &&
             (serial_in(0x3FD) & 0x20U) == 0; ++tries) { }
        UINT8 value = (UINT8)*text++;
        serial_out(0x3F8, value);
        g_serial_line_open = value != '\r' && value != '\n';
    }
#endif
}

void kernel_serial_write_record_guarded(const char *text) {
    if (text == 0) return;
#if OS_DEBUG_SERIAL
    /* 其他诊断信息由多次 printf 调用拼接而成。即使某条部分消息尚未结束，也要在物理行边界输出带位置
     * 信息的记录。 */
    if (g_serial_line_open) kernel_serial_write_guarded("\r\n");
    kernel_serial_write_guarded(text);
#endif
}

void kernel_serial_write_serial_only(const char *text) {
    if (text == 0) return;
    realtest_capture(text);
#if OS_DEBUG_SERIAL
    kernel_serial_guard_t guard;
    kernel_serial_guard_enter(&guard);
    kernel_serial_write_guarded(text);
    kernel_serial_guard_leave(&guard);
#endif
}

void kernel_serial_write(const char *text) {
    if (text == 0) return;
    realtest_capture(text);
#if OS_DEBUG_SERIAL
    kernel_serial_guard_t serial_guard;
    kernel_serial_guard_enter(&serial_guard);
#endif
    BOOLEAN gop_ready = atomic_load_explicit(&g_gop_debug_console_ready,
                                             memory_order_acquire) != 0U;
    BOOLEAN gop_lock_held = 0;
    BOOLEAN gop_preempt_disabled = 0;
    if (gop_ready) {
#if OS_DEBUG_SERIAL
        gop_lock_held = gop_debug_lock((serial_guard.flags & (1ULL << 9)) != 0U);
#else
        UINT64 flags;
        sched_preempt_disable();
        gop_preempt_disabled = 1;
        __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
        gop_lock_held = gop_debug_lock((flags & (1ULL << 9)) != 0U);
#endif
    }
#if OS_DEBUG_SERIAL
    kernel_serial_write_guarded(text);
#endif
    if (gop_lock_held) gop_debug_write_locked((const CHAR8 *)text);
    if (gop_lock_held) gop_debug_unlock();
    if (gop_preempt_disabled) sched_preempt_enable();
#if OS_DEBUG_SERIAL
    kernel_serial_guard_leave(&serial_guard);
#endif
}

void kernel_serial_enable_concurrency(void) {
#if OS_DEBUG_SERIAL
    atomic_store_explicit(&g_serial_owner_cpu, UINT32_MAX,
                          memory_order_relaxed);
    atomic_store_explicit(&g_serial_concurrency_ready, 1U,
                          memory_order_release);
#endif
}

void kernel_serial_write_u32(uint32_t value) {
    (void)printf("%u", value);
}

void kernel_serial_write_u32_serial_only(uint32_t value) {
    (void)kernel_serial_printf_serial_only("%u", value);
}

void kernel_console_serial_init(void) {
#if OS_DEBUG_SERIAL
    serial_out(0x3F9, 0x00);
    serial_out(0x3FB, 0x80);
    serial_out(0x3F8, 0x01);
    serial_out(0x3F9, 0x00);
    serial_out(0x3FB, 0x03);
    serial_out(0x3FA, 0xC7);
    serial_out(0x3FC, 0x0B);
#endif
}
