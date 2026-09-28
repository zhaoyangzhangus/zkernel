/*
 * pmm.h —— 早期物理内存分配器
 *
 * 第一版只支持分配，不支持释放。UEFI memory map 本身就是 free-range 元数据：
 * 每次分配成功后直接推进对应 MEM_CONVENTIONAL 描述符的 physical_start，
 * 并减少 number_of_pages，不建立 bitmap、frame array 或 free list。
 */
#ifndef __KERNEL_MM_PMM_H__
#define __KERNEL_MM_PMM_H__

#include <stdint.h>

#include "../boot/bootinfo.h"

#define PMM_PAGE_SIZE 4096ULL
#define PMM_ALLOC_FAILED UINT64_MAX

/* 成功返回 0；bootinfo/memory map 不合法时返回 -1。 */
int pmm_init(BOOT_INFO *boot_info);

/*
 * 分配至少 size 字节的物理连续内存。
 * 实际占用大小自动向上对齐到 4 KiB 页边界。
 * 成功返回首个物理地址；失败返回 PMM_ALLOC_FAILED。
 * 物理地址 0 是合法分配结果，不能用作失败值。
 */
uint64_t pmm_alloc(uint64_t size);

#endif /* __KERNEL_MM_PMM_H__ */
