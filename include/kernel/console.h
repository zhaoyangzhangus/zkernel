#pragma once

#include <kernel/base.h>

/*
 * 内核与驱动共享的调试输出接口。默认只绘制到 GOP 控制台；构建时
 * 显式设置 OS_DEBUG_SERIAL=1 才会额外镜像到 COM1。
 * 保留 serial 名称是为了兼容现有驱动调用点。
 */
void kernel_serial_write(const char *text);
void kernel_serial_write_u32(uint32_t value);
void kernel_serial_write_u32_serial_only(uint32_t value);
void kernel_serial_enable_concurrency(void);

/* 内部串口事务原语。带位置信息的阶段记录可与 printf 共用 COM1 锁，而无需绘制到 GOP。 */
typedef struct kernel_serial_guard {
    uint64_t flags;
    uint8_t owned;
    uint8_t preempt_disabled;
} kernel_serial_guard_t;

void kernel_serial_guard_enter(kernel_serial_guard_t *guard);
void kernel_serial_guard_leave(kernel_serial_guard_t *guard);
void kernel_serial_write_guarded(const char *text);
void kernel_serial_write_record_guarded(const char *text);
void kernel_serial_write_serial_only(const char *text);
int kernel_serial_printf_serial_only(const char *format, ...);

/* 内核终端是 12x24 固定单元格的 A8 控制台。printk 会先格式化完整记录，再写入字符/颜色缓冲区；
 * 后端随后会在一个事务中将脏行渲染到 GOP。 */
int printk(const char *format, ...);
int printf(const char *format, ...);
void kernel_console_refresh(void);
void kernel_console_set_color(uint32_t color);
