#include "paging.h"
#include "bootmem.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PTE_PRESENT UINT64_C(0x001)
#define PTE_WRITE   UINT64_C(0x002)
#define PTE_LARGE   UINT64_C(0x080)
#define PTE_ADDR    UINT64_C(0x000FFFFFFFFFF000)

#define PAGE_4K UINT64_C(0x1000)
#define PAGE_2M UINT64_C(0x200000)
#define PAGE_1G UINT64_C(0x40000000)
#define PAGE_512G UINT64_C(0x8000000000)

#define CR4_LA57 (UINT64_C(1) << 12)

typedef struct {
    BOOT_INFO *bi;
    uint64_t used_pages;

    uint64_t *pml4;
    uint64_t root_phys;

    bool allow_1g;
    paging_info_t info;
} paging_builder_t;

static void zero_page(void *ptr)
{
    uint64_t *p = ptr;
    for (uint32_t i = 0; i < PAGE_4K / sizeof(uint64_t); ++i)
        p[i] = 0;
}

static bool usable_ram_type(uint32_t type)
{
    return type == MEM_LOADER_CODE ||
           type == MEM_LOADER_DATA ||
           type == MEM_BOOT_SERVICES_CODE ||
           type == MEM_BOOT_SERVICES_DATA ||
           type == MEM_CONVENTIONAL;
}

static uint64_t read_cr4(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static void write_cr3(uint64_t phys)
{
    __asm__ volatile("mov %0, %%cr3" :: "r"(phys) : "memory");
}

static bool cpu_has_1g_pages(void)
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    eax = UINT32_C(0x80000000);
    ecx = 0;
    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    if (eax < UINT32_C(0x80000001))
        return false;

    eax = UINT32_C(0x80000001);
    ecx = 0;
    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));

    return (edx & (UINT32_C(1) << 26)) != 0;
}

static bool descriptor_bytes(const BOOT_MEMORY_DESCRIPTOR *d,
                             uint64_t *out_bytes)
{
    if (d->number_of_pages == 0 ||
        d->number_of_pages > UINT64_MAX / PAGE_4K)
        return false;

    uint64_t bytes = d->number_of_pages * PAGE_4K;
    if (d->physical_start > UINT64_MAX - bytes)
        return false;

    *out_bytes = bytes;
    return true;
}

static bool alloc_table(paging_builder_t *b,
                        uint64_t **out_table, uint64_t *out_phys)
{
    uint64_t phys;
    if (!boot_alloc_pages(b->bi, 1, &phys))
        return false;

    ++b->used_pages;

    uint64_t *table = (uint64_t *)(uintptr_t)phys;
    zero_page(table);

    *out_table = table;
    *out_phys = phys;
    return true;
}

static bool child_table(paging_builder_t *b, uint64_t *parent,
                        uint32_t index, uint64_t **out)
{
    uint64_t entry = parent[index];

    if ((entry & PTE_PRESENT) != 0) {
        if ((entry & PTE_LARGE) != 0)
            return false;

        *out = (uint64_t *)(uintptr_t)(entry & PTE_ADDR);
        return true;
    }

    uint64_t *table;
    uint64_t phys;
    if (!alloc_table(b, &table, &phys))
        return false;

    parent[index] = phys | PTE_PRESENT | PTE_WRITE;
    *out = table;
    return true;
}

static bool map_4k(paging_builder_t *b, uint64_t va, uint64_t pa)
{
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;

    uint32_t i4 = (uint32_t)((va >> 39) & 0x1FFU);
    uint32_t i3 = (uint32_t)((va >> 30) & 0x1FFU);
    uint32_t i2 = (uint32_t)((va >> 21) & 0x1FFU);
    uint32_t i1 = (uint32_t)((va >> 12) & 0x1FFU);

    if (!child_table(b, b->pml4, i4, &pdpt) ||
        !child_table(b, pdpt, i3, &pd) ||
        !child_table(b, pd, i2, &pt))
        return false;

    if ((pt[i1] & PTE_PRESENT) != 0)
        return false;

    pt[i1] = (pa & PTE_ADDR) | PTE_PRESENT | PTE_WRITE;
    ++b->info.leaf_4k;
    return true;
}

static bool map_2m(paging_builder_t *b, uint64_t va, uint64_t pa)
{
    uint64_t *pdpt;
    uint64_t *pd;

    uint32_t i4 = (uint32_t)((va >> 39) & 0x1FFU);
    uint32_t i3 = (uint32_t)((va >> 30) & 0x1FFU);
    uint32_t i2 = (uint32_t)((va >> 21) & 0x1FFU);

    if (!child_table(b, b->pml4, i4, &pdpt) ||
        !child_table(b, pdpt, i3, &pd))
        return false;

    if ((pd[i2] & PTE_PRESENT) != 0)
        return false;

    pd[i2] = (pa & ~(PAGE_2M - 1)) |
             PTE_PRESENT | PTE_WRITE | PTE_LARGE;
    ++b->info.leaf_2m;
    return true;
}

static bool map_1g(paging_builder_t *b, uint64_t va, uint64_t pa)
{
    uint64_t *pdpt;

    uint32_t i4 = (uint32_t)((va >> 39) & 0x1FFU);
    uint32_t i3 = (uint32_t)((va >> 30) & 0x1FFU);

    if (!child_table(b, b->pml4, i4, &pdpt))
        return false;

    if ((pdpt[i3] & PTE_PRESENT) != 0)
        return false;

    pdpt[i3] = (pa & ~(PAGE_1G - 1)) |
               PTE_PRESENT | PTE_WRITE | PTE_LARGE;
    ++b->info.leaf_1g;
    return true;
}

static bool map_range(paging_builder_t *b,
                      uint64_t va, uint64_t pa, uint64_t bytes)
{
    while (bytes != 0) {
        if (b->allow_1g &&
            (va & (PAGE_1G - 1)) == 0 &&
            (pa & (PAGE_1G - 1)) == 0 &&
            bytes >= PAGE_1G) {
            if (!map_1g(b, va, pa))
                return false;
            va += PAGE_1G;
            pa += PAGE_1G;
            bytes -= PAGE_1G;
            continue;
        }

        if ((va & (PAGE_2M - 1)) == 0 &&
            (pa & (PAGE_2M - 1)) == 0 &&
            bytes >= PAGE_2M) {
            if (!map_2m(b, va, pa))
                return false;
            va += PAGE_2M;
            pa += PAGE_2M;
            bytes -= PAGE_2M;
            continue;
        }

        if (!map_4k(b, va, pa))
            return false;

        va += PAGE_4K;
        pa += PAGE_4K;
        bytes -= PAGE_4K;
    }

    return true;
}

static bool map_one_extent(paging_builder_t *b,
                           uint64_t start, uint64_t bytes,
                           bool direct)
{
    uint64_t va = start;

    if (direct) {
        if (start >= VM_KERNEL_SIZE ||
            bytes > VM_KERNEL_SIZE - start)
            return false;
        va = VM_DIRECT_MAP_BASE + start;
    }

    return map_range(b, va, start, bytes);
}

static bool map_all_usable(paging_builder_t *b,
                           const BOOT_INFO *bi, bool direct)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_addr;

    bool have_extent = false;
    uint32_t extent_type = 0;
    uint64_t extent_start = 0;
    uint64_t extent_bytes = 0;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)p;

        if (!usable_ram_type(d->type) || d->number_of_pages == 0) {
            if (have_extent) {
                if (!map_one_extent(b, extent_start, extent_bytes, direct))
                    return false;
                have_extent = false;
                extent_bytes = 0;
            }
            continue;
        }

        uint64_t bytes;
        if (!descriptor_bytes(d, &bytes))
            return false;

        if (!have_extent) {
            have_extent = true;
            extent_type = d->type;
            extent_start = d->physical_start;
            extent_bytes = bytes;
            continue;
        }

        uint64_t extent_end = extent_start + extent_bytes;

        if (d->type == extent_type &&
            d->physical_start == extent_end) {
            if (extent_bytes > UINT64_MAX - bytes)
                return false;
            extent_bytes += bytes;
            continue;
        }

        if (!map_one_extent(b, extent_start, extent_bytes, direct))
            return false;

        extent_type = d->type;
        extent_start = d->physical_start;
        extent_bytes = bytes;
    }

    if (have_extent &&
        !map_one_extent(b, extent_start, extent_bytes, direct))
        return false;

    return true;
}


static bool find_direct_span(const BOOT_INFO *bi,
                             uint64_t *out_span)
{
    uint64_t maximum = 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_addr;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)p;

        if (!usable_ram_type(d->type) || d->number_of_pages == 0)
            continue;

        uint64_t bytes;
        if (!descriptor_bytes(d, &bytes))
            return false;

        uint64_t end = d->physical_start + bytes;
        if (end > maximum)
            maximum = end;
    }

    if (maximum == 0 || maximum > VM_KERNEL_SIZE)
        return false;

    *out_span = maximum;
    return true;
}

int paging_early_takeover(BOOT_INFO *bi, paging_info_t *out_info)
{
    if (bi == NULL || out_info == NULL ||
        bi->mmap_addr == 0 ||
        bi->mmap_desc_size < sizeof(BOOT_MEMORY_DESCRIPTOR) ||
        bi->mmap_desc_count == 0 ||
        (uint64_t)bi->mmap_desc_count >
            bi->mmap_size / bi->mmap_desc_size)
        return -1;

    if ((read_cr4() & CR4_LA57) != 0)
        return -2;

    bool allow_1g = cpu_has_1g_pages();

    paging_builder_t b = {0};
    b.bi = bi;
    b.allow_1g = allow_1g;
    b.info.has_1g_pages = allow_1g ? 1U : 0U;

    /*
     * direct_span 在任何页表分配修改 Conventional descriptor 之前确定。
     * 页表本身永久保留，不会进入后续 PMM。
     */
    if (!find_direct_span(bi, &b.info.direct_span))
        return -3;

    /*
     * 每缺一个页表页就直接 boot_alloc_pages(1)。
     * boot allocator 从最高 Conventional 地址向下分配，不做预估、
     * 不预留大块，也不回收页表页。
     */
    if (!alloc_table(&b, &b.pml4, &b.root_phys))
        return -4;

    b.info.root_phys = b.root_phys;

    if (!map_all_usable(&b, bi, false))
        return -5;

    if (!map_all_usable(&b, bi, true))
        return -6;

    b.info.table_pages = b.used_pages;

    write_cr3(b.root_phys);

    *out_info = b.info;
    return 0;
}
