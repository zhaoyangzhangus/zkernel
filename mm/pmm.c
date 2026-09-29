#include "pmm.h"

#include <stddef.h>
#include <stdint.h>

static BOOT_INFO *g_boot_info;

int pmm_init(BOOT_INFO *bi)
{
    if (bi == NULL || bi->mmap_addr == 0 ||
        bi->mmap_desc_size < sizeof(BOOT_MEMORY_DESCRIPTOR) ||
        bi->mmap_desc_count == 0 ||
        (uint64_t)bi->mmap_desc_count > bi->mmap_size / bi->mmap_desc_size)
        return -1;

    g_boot_info = bi;
    return 0;
}

uint64_t pmm_alloc(uint64_t size)
{
    if (g_boot_info == NULL || size == 0 ||
        size > UINT64_MAX - (PMM_PAGE_SIZE - 1))
        return PMM_ALLOC_FAILED;

    uint64_t pages = (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t bytes = pages * PMM_PAGE_SIZE;
    uint8_t *p = (uint8_t *)(uintptr_t)g_boot_info->mmap_addr;

    for (uint32_t i = 0; i < g_boot_info->mmap_desc_count;
         ++i, p += g_boot_info->mmap_desc_size) {
        BOOT_MEMORY_DESCRIPTOR *d = (BOOT_MEMORY_DESCRIPTOR *)(void *)p;

        if (d->type != MEM_CONVENTIONAL || d->number_of_pages < pages)
            continue;

        uint64_t base = d->physical_start;
        d->physical_start = base + bytes;
        d->number_of_pages -= pages;
        return base;
    }

    return PMM_ALLOC_FAILED;
}
