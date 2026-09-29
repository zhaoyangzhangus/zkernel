#include "bootmem.h"

#include <stddef.h>
#include <stdint.h>

#define BOOT_PAGE_SIZE UINT64_C(0x1000)

bool boot_alloc_pages(BOOT_INFO *bi, uint64_t pages, uint64_t *out_phys)
{
    if (bi == NULL || out_phys == NULL || pages == 0 ||
        pages > UINT64_MAX / BOOT_PAGE_SIZE)
        return false;

    uint8_t *p = (uint8_t *)(uintptr_t)bi->mmap_addr;

    /*
     * 始终选择能够容纳请求的最低物理地址 Conventional descriptor，
     * 并从它的低地址端向上分配。descriptor 的 physical_start 随分配
     * 前移，因此这些 early pages 会直接从后续 PMM 可见范围中消失。
     */
    BOOT_MEMORY_DESCRIPTOR *lowest = NULL;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        BOOT_MEMORY_DESCRIPTOR *d =
            (BOOT_MEMORY_DESCRIPTOR *)(void *)p;

        if (d->type != MEM_CONVENTIONAL ||
            d->number_of_pages < pages)
            continue;

        if (lowest == NULL ||
            d->physical_start < lowest->physical_start)
            lowest = d;
    }

    if (lowest == NULL)
        return false;

    uint64_t bytes = pages * BOOT_PAGE_SIZE;
    if (lowest->physical_start > UINT64_MAX - bytes)
        return false;

    uint64_t phys = lowest->physical_start;

    lowest->physical_start += bytes;
    lowest->number_of_pages -= pages;
    *out_phys = phys;
    return true;
}

bool boot_release_pages(BOOT_INFO *bi, uint64_t phys, uint64_t pages)
{
    if (bi == NULL || pages == 0 ||
        pages > UINT64_MAX / BOOT_PAGE_SIZE)
        return false;

    uint64_t bytes = pages * BOOT_PAGE_SIZE;
    uint8_t *p = (uint8_t *)(uintptr_t)bi->mmap_addr;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        BOOT_MEMORY_DESCRIPTOR *d =
            (BOOT_MEMORY_DESCRIPTOR *)(void *)p;

        if (d->type != MEM_CONVENTIONAL ||
            d->number_of_pages > UINT64_MAX / BOOT_PAGE_SIZE)
            continue;

        if (phys > UINT64_MAX - bytes ||
            phys + bytes != d->physical_start)
            continue;

        if (d->number_of_pages > UINT64_MAX - pages)
            return false;

        d->physical_start = phys;
        d->number_of_pages += pages;
        return true;
    }

    return false;
}
