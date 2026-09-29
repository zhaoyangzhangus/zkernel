#include "bootmem.h"

#include <stddef.h>
#include <stdint.h>

#define BOOT_PAGE_SIZE UINT64_C(0x1000)

bool boot_alloc_pages(BOOT_INFO *bi, uint64_t pages, uint64_t *out_phys)
{
    if (bi == NULL || out_phys == NULL || pages == 0 ||
        pages > UINT64_MAX / BOOT_PAGE_SIZE)
        return false;

    /*
     * UEFI memory map 按 physical_start 从低到高排列。
     * 从末尾反向扫描，找到第一个能容纳请求的 Conventional descriptor，
     * 再从它的高地址端向下切出 pages。
     */
    for (uint32_t i = bi->mmap_desc_count; i != 0; --i) {
        uint8_t *p = (uint8_t *)(uintptr_t)bi->mmap_addr +
                     (uint64_t)(i - 1) * bi->mmap_desc_size;
        BOOT_MEMORY_DESCRIPTOR *d =
            (BOOT_MEMORY_DESCRIPTOR *)(void *)p;

        if (d->type != MEM_CONVENTIONAL ||
            d->number_of_pages < pages)
            continue;

        uint64_t remain = d->number_of_pages - pages;
        uint64_t offset = remain * BOOT_PAGE_SIZE;

        if (d->physical_start > UINT64_MAX - offset)
            return false;

        uint64_t phys = d->physical_start + offset;

        d->number_of_pages = remain;
        *out_phys = phys;
        return true;
    }

    return false;
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
