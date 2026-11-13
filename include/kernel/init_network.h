#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_network_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} kernel_init_network_hooks_t;

/* 校验网络协议栈，并发布网络运行边界。 */
BOOLEAN kernel_init_network(const kernel_init_network_hooks_t *hooks);
