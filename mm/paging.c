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
    uint64_t block_phys;
    uint64_t block_pages;
    uint64_t used_pages;

    uint64_t *pml4;
    uint64_t root_phys;

    bool allow_1g;
    paging_info_t info;
} paging_builder_t;

typedef struct {
    uint64_t pdpt;
    uint64_t pd;
    uint64_t pt;
} table_count_t;

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

/*
 * 对单个连续 range 按实际 1G->2M->4K 策略做 dry-run。
 * 这里只统计这个 range 独立需要的 lower-level table 数，因此不同
 * descriptor 落在同一个 table 时会略微重复计数；这是安全上界。
 */
static bool estimate_range(uint64_t va, uint64_t pa, uint64_t bytes,
                           bool allow_1g, table_count_t *out)
{
    uint64_t last_i4 = UINT64_MAX;
    uint64_t last_pd = UINT64_MAX;
    uint64_t last_pt = UINT64_MAX;

    while (bytes != 0) {
        uint64_t i4 = va >> 39;
        if (i4 != last_i4) {
            ++out->pdpt;
            last_i4 = i4;
        }

        if (allow_1g &&
            (va & (PAGE_1G - 1)) == 0 &&
            (pa & (PAGE_1G - 1)) == 0 &&
            bytes >= PAGE_1G) {
            va += PAGE_1G;
            pa += PAGE_1G;
            bytes -= PAGE_1G;
            continue;
        }

        uint64_t pd_key = va >> 30;
        if (pd_key != last_pd) {
            ++out->pd;
            last_pd = pd_key;
        }

        if ((va & (PAGE_2M - 1)) == 0 &&
            (pa & (PAGE_2M - 1)) == 0 &&
            bytes >= PAGE_2M) {
            va += PAGE_2M;
            pa += PAGE_2M;
            bytes -= PAGE_2M;
            continue;
        }

        uint64_t pt_key = va >> 21;
        if (pt_key != last_pt) {
            ++out->pt;
            last_pt = pt_key;
        }

        va += PAGE_4K;
        pa += PAGE_4K;
        bytes -= PAGE_4K;
    }

    return true;
}

static bool estimate_one_extent(uint64_t start, uint64_t bytes,
                                bool allow_1g, table_count_t *count)
{
    if (!estimate_range(start, start, bytes, allow_1g, count))
        return false;

    if (start >= VM_KERNEL_SIZE ||
        bytes > VM_KERNEL_SIZE - start)
        return false;

    return estimate_range(VM_DIRECT_MAP_BASE + start,
                          start, bytes, allow_1g, count);
}

static bool estimate_tables(const BOOT_INFO *bi, bool allow_1g,
                            uint64_t *out_pages)
{
    table_count_t count = {0};
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
                if (!estimate_one_extent(extent_start, extent_bytes,
                                         allow_1g, &count))
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

        /*
         * 只合并：
         *   1. UEFI type 相同；
         *   2. 物理地址严格连续。
         *
         * 不跨 type 合并，即使两个 descriptor 在物理上相邻。
         */
        if (d->type == extent_type &&
            d->physical_start == extent_end) {
            if (extent_bytes > UINT64_MAX - bytes)
                return false;
            extent_bytes += bytes;
            continue;
        }

        if (!estimate_one_extent(extent_start, extent_bytes,
                                 allow_1g, &count))
            return false;

        extent_type = d->type;
        extent_start = d->physical_start;
        extent_bytes = bytes;
    }

    if (have_extent &&
        !estimate_one_extent(extent_start, extent_bytes,
                             allow_1g, &count))
        return false;

    /*
     * +1 PML4 root。
     * 统计仍采用安全上界，实际未使用页在建表后归还 boot allocator。
     */
    uint64_t total = UINT64_C(1) + count.pdpt + count.pd + count.pt;
    if (total > UINT64_MAX - 16)
        return false;

    *out_pages = total + 16;
    return true;
}


static bool alloc_table(paging_builder_t *b,
                        uint64_t **out_table, uint64_t *out_phys)
{
    if (b->used_pages >= b->block_pages)
        return false;

    uint64_t index = b->block_pages - b->used_pages - 1;
    uint64_t phys = b->block_phys + index * PAGE_4K;
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


static bool map_bootstrap_block(paging_builder_t *b, bool direct)
{
    uint64_t bytes = b->block_pages * PAGE_4K;
    uint64_t va = b->block_phys;

    if (direct) {
        if (b->block_phys >= VM_KERNEL_SIZE ||
            bytes > VM_KERNEL_SIZE - b->block_phys)
            return false;
        va = VM_DIRECT_MAP_BASE + b->block_phys;
    }

    return map_range(b, va, b->block_phys, bytes);
}

static bool find_direct_span(const BOOT_INFO *bi,
                             uint64_t bootstrap_phys,
                             uint64_t bootstrap_pages,
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

    if (bootstrap_pages != 0) {
        uint64_t bytes = bootstrap_pages * PAGE_4K;
        if (bootstrap_phys > UINT64_MAX - bytes)
            return false;
        uint64_t end = bootstrap_phys + bytes;
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

    uint64_t reserve_pages;
    if (!estimate_tables(bi, allow_1g, &reserve_pages))
        return -3;

    uint64_t block_phys;
    if (!boot_alloc_pages(bi, reserve_pages, &block_phys))
        return -4;

    paging_builder_t b = {0};
    b.block_phys = block_phys;
    b.block_pages = reserve_pages;
    b.allow_1g = allow_1g;
    b.info.has_1g_pages = allow_1g ? 1U : 0U;

    if (!find_direct_span(bi, block_phys, reserve_pages,
                          &b.info.direct_span))
        return -5;

    if (!alloc_table(&b, &b.pml4, &b.root_phys))
        return -6;

    b.info.root_phys = b.root_phys;

    /*
     * boot_alloc_pages 已从 Conventional memory map 中切走整块页表内存。
     * 先映射剩余全部 usable RAM，再显式映射 bootstrap page-table block，
     * 因而切 CR3 后页表自身也仍可通过 identity/direct map 访问。
     */
    if (!map_all_usable(&b, bi, false) ||
        !map_bootstrap_block(&b, false))
        return -7;

    if (!map_all_usable(&b, bi, true) ||
        !map_bootstrap_block(&b, true))
        return -8;

    b.info.table_pages = b.used_pages;

    /*
     * estimate_tables() 是安全上界。页表从预留 block 高端向下使用，
     * 因而未使用的低端前缀正好紧贴原 Conventional descriptor，
     * 可以在启动 PMM 前无损归还。
     */
    uint64_t unused = b.block_pages - b.used_pages;
    if (unused != 0 &&
        !boot_release_pages(bi, b.block_phys, unused))
        return -9;

    write_cr3(b.root_phys);

    *out_info = b.info;
    return 0;
}
