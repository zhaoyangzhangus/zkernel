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
     * 优先选择最大的 Conventional descriptor，减少把很多小 range
     * 切碎。页从 descriptor 高地址端取，保持 physical_start 不变，
     * 对后续 1G/2M 大页映射和 PMM seed 更友好。
     */
    BOOT_MEMORY_DESCRIPTOR *best = NULL;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        BOOT_MEMORY_DESCRIPTOR *d =
            (BOOT_MEMORY_DESCRIPTOR *)(void *)p;

        if (d->type != MEM_CONVENTIONAL ||
            d->number_of_pages < pages)
            continue;

        if (best == NULL || d->number_of_pages > best->number_of_pages)
            best = d;
    }

    if (best == NULL)
        return false;

    uint64_t remain = best->number_of_pages - pages;
    uint64_t offset = remain * BOOT_PAGE_SIZE;

    if (best->physical_start > UINT64_MAX - offset)
        return false;

    uint64_t phys = best->physical_start + offset;

    best->number_of_pages = remain;
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

        uint64_t current_bytes = d->number_of_pages * BOOT_PAGE_SIZE;
        if (d->physical_start > UINT64_MAX - current_bytes)
            continue;

        uint64_t end = d->physical_start + current_bytes;
        if (end != phys)
            continue;

        if (d->number_of_pages > UINT64_MAX - pages)
            return false;

        d->number_of_pages += pages;
        return true;
    }

    return false;
}
