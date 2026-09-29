#ifndef __KERNEL_MM_BOOTMEM_H__
#define __KERNEL_MM_BOOTMEM_H__

#include <stdbool.h>
#include <stdint.h>

#include "../boot/bootinfo.h"

/*
 * ExitBootServices 后使用 UEFI memory map 做最早期物理页保留。
 * 从 Conventional descriptor 的高地址端一次切出连续 pages。
 *
 * descriptor 会立即缩短，因此后续 PMM 不会再次看到这些页。
 */
bool boot_alloc_pages(BOOT_INFO *bi, uint64_t pages, uint64_t *out_phys);

/*
 * 归还紧贴某个 Conventional descriptor 高端的连续页。
 * 仅供 early bootstrap 在尚未启动 PMM 时收回过量预留。
 */
bool boot_release_pages(BOOT_INFO *bi, uint64_t phys, uint64_t pages);

#endif
