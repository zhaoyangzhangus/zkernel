/*
 * pmm.h —— 早期物理页分配器
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

/* 成功返回 0；bootinfo/memory map 不合法时返回 -1。 */
int pmm_init(BOOT_INFO *boot_info);

/*
 * 分配 page_count 个物理连续的 4 KiB 页。
 * 成功返回首个物理地址，失败返回 0。
 */
uint64_t pmm_alloc_pages(uint64_t page_count);

static inline uint64_t pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

#endif /* __KERNEL_MM_PMM_H__ */
