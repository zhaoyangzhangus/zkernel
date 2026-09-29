#include "paging.h"

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

#define CR4_LA57 (UINT64_C(1) << 12)

typedef struct {
    BOOT_MEMORY_DESCRIPTOR *donor;
    uint64_t donor_start;
    uint64_t donor_pages;

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

static BOOT_MEMORY_DESCRIPTOR *find_donor(BOOT_INFO *bi)
{
    BOOT_MEMORY_DESCRIPTOR *best = NULL;
    uint8_t *p = (uint8_t *)(uintptr_t)bi->mmap_addr;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        BOOT_MEMORY_DESCRIPTOR *d =
            (BOOT_MEMORY_DESCRIPTOR *)(void *)p;

        if (d->type != MEM_CONVENTIONAL || d->number_of_pages == 0)
            continue;

        if (best == NULL || d->number_of_pages > best->number_of_pages)
            best = d;
    }

    return best;
}

/*
 * 页表 bootstrap allocator：
 * 从同一个最大 Conventional descriptor 的高地址端向下拿 4K page。
 * descriptor 立即缩短，因此后面的 pmm_init() 永远看不到这些页。
 *
 * donor 的原始范围另存一份；建图时仍映射原始完整范围，所以页表页本身
 * 也包含在 identity/direct map 中。
 */
static bool alloc_table(paging_builder_t *b,
                        uint64_t **out_table, uint64_t *out_phys)
{
    BOOT_MEMORY_DESCRIPTOR *d = b->donor;

    if (d == NULL || d->number_of_pages == 0)
        return false;

    --d->number_of_pages;
    uint64_t phys =
        d->physical_start + d->number_of_pages * PAGE_4K;

    uint64_t *table = (uint64_t *)(uintptr_t)phys;
    zero_page(table);

    ++b->info.table_pages;
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
    if ((va & (PAGE_4K - 1)) != 0 ||
        (pa & (PAGE_4K - 1)) != 0 ||
        (bytes & (PAGE_4K - 1)) != 0)
        return false;

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

static bool descriptor_bytes(uint64_t pages, uint64_t *out_bytes)
{
    if (pages == 0 || pages > UINT64_MAX / PAGE_4K)
        return false;

    *out_bytes = pages * PAGE_4K;
    return true;
}

static bool original_descriptor_range(const paging_builder_t *b,
                                      const BOOT_MEMORY_DESCRIPTOR *d,
                                      uint64_t *out_start,
                                      uint64_t *out_bytes)
{
    uint64_t pages = d->number_of_pages;

    if (d == b->donor)
        pages = b->donor_pages;

    if (!descriptor_bytes(pages, out_bytes))
        return false;

    *out_start = d->physical_start;
    return *out_start <= UINT64_MAX - *out_bytes;
}

static bool map_all_usable(paging_builder_t *b,
                           const BOOT_INFO *bi, bool direct)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_addr;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)p;

        if (!usable_ram_type(d->type))
            continue;

        uint64_t start;
        uint64_t bytes;
        if (!original_descriptor_range(b, d, &start, &bytes))
            return false;

        uint64_t va = start;

        if (direct) {
            if (start >= VM_KERNEL_SIZE ||
                bytes > VM_KERNEL_SIZE - start)
                return false;

            va = VM_DIRECT_MAP_BASE + start;
        }

        if (!map_range(b, va, start, bytes))
            return false;
    }

    return true;
}

static bool find_direct_span(const paging_builder_t *b,
                             const BOOT_INFO *bi,
                             uint64_t *out_span)
{
    uint64_t maximum = 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_addr;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)p;

        if (!usable_ram_type(d->type))
            continue;

        uint64_t start;
        uint64_t bytes;
        if (!original_descriptor_range(b, d, &start, &bytes))
            return false;

        uint64_t end = start + bytes;
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

    /* 当前先固定 4-level paging，不在 long mode 中途切 LA57。 */
    if ((read_cr4() & CR4_LA57) != 0)
        return -2;

    paging_builder_t b = {0};

    b.donor = find_donor(bi);
    if (b.donor == NULL)
        return -3;

    b.donor_start = b.donor->physical_start;
    b.donor_pages = b.donor->number_of_pages;
    b.allow_1g = cpu_has_1g_pages();
    b.info.has_1g_pages = b.allow_1g ? 1U : 0U;

    if (!find_direct_span(&b, bi, &b.info.direct_span))
        return -4;

    if (!alloc_table(&b, &b.pml4, &b.root_phys))
        return -5;

    b.info.root_phys = b.root_phys;

    /*
     * 当前 RIP/RSP/BOOT_INFO 仍是低地址，所以先建立所有 usable RAM 的
     * identity mapping。之后再建立相同 RAM 的 high-half direct map。
     *
     * donor 使用分配前的原始范围，因此刚切出的所有页表页本身也在两套
     * mapping 中。
     */
    if (!map_all_usable(&b, bi, false))
        return -6;

    if (!map_all_usable(&b, bi, true))
        return -7;

    /*
     * 到这里所有可用 RAM（包括新页表页）都已经由自己的页表覆盖。
     * 切 CR3 后不再依赖 UEFI page tables。
     */
    write_cr3(b.root_phys);

    *out_info = b.info;
    return 0;
}
