#ifndef __KERNEL_MM_BOOTMEM_H__
#define __KERNEL_MM_BOOTMEM_H__

#include <stdbool.h>
#include <stdint.h>

#include "../boot/bootinfo.h"

/*
 * ExitBootServices 后使用 UEFI memory map 做最早期物理页保留。
 * UEFI memory map 按物理地址升序排列；从 map 末尾反向找到第一个
 * 能容纳请求的 Conventional descriptor，并从其高地址端向下切出
 * 连续 pages。
 *
 * descriptor 的 number_of_pages 会立即缩短，因此后续 PMM 不会再次看到这些页。
 */
bool boot_alloc_pages(BOOT_INFO *bi, uint64_t pages, uint64_t *out_phys);

/*
 * 归还紧贴某个 Conventional descriptor 高端的连续页。
 * 仅供尚未启动 PMM 的 early 阶段使用。
 */
bool boot_release_pages(BOOT_INFO *bi, uint64_t phys, uint64_t pages);

#endif
