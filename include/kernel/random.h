#pragma once

#include <kernel/bootinfo.h>

/* 用户态启动前，使用 UEFI RNG 为内核随机数流提供种子。 */
void random_init(const OS_BOOT_INFO *boot_info);
