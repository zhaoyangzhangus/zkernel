#pragma once

#include <kernel/bootinfo.h>

/* 按序初始化单元和运行时交接流程共用的引导故障处理者。 */
__attribute__((noreturn)) void kernel_halt_forever(void);
__attribute__((noreturn)) void kernel_halt_forever_at(
    const char *file, uint32_t line);

/* 完成高半区交接，并运行初始化后的 Ring0 续执行路径。 */
__attribute__((noreturn)) void kernel_init_runtime_start(
    OS_BOOT_INFO *info, UINT64 framebuffer_virtual_base);
