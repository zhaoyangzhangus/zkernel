#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_core_hooks {
    void (*write)(const CHAR8 *text);
    void (*halt)(void);
} kernel_init_core_hooks_t;

/* 在存储和调度初始化前，运行分配器/核心内存检查。 */
BOOLEAN kernel_init_core(const kernel_init_core_hooks_t *hooks);
