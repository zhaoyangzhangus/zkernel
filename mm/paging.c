#include "paging.h"
#include "bootmem.h"
#include "../arch/x86_64/pat.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PTE_PRESENT UINT64_C(0x001)
#define PTE_WRITE   UINT64_C(0x002)
#define PTE_USER    UINT64_C(0x004)
#define PTE_LARGE   UINT64_C(0x080)
/* software-owned bit: only meaningful on non-leaf table entries */
#define PTE_TABLE_PMM_OWNED (UINT64_C(1) << 9)
#define PTE_ADDR    UINT64_C(0x000FFFFFFFFFF000)
#define PDE_2M_ADDR UINT64_C(0x000FFFFFFFE00000)

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

static uint64_t read_cr3(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
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

static inline uint64_t canonical48(uint64_t value)
{
    if ((value & (UINT64_C(1) << 47)) != 0)
        value |= UINT64_C(0xFFFF000000000000);
    return value;
}

static inline uint64_t recursive_address(uint32_t i4, uint32_t i3,
                                         uint32_t i2, uint32_t i1)
{
    return canonical48(((uint64_t)i4 << 39) |
                       ((uint64_t)i3 << 30) |
                       ((uint64_t)i2 << 21) |
                       ((uint64_t)i1 << 12));
}

static inline void invlpg(uint64_t va)
{
    __asm__ volatile("invlpg (%0)" :: "r"((void *)(uintptr_t)va) : "memory");
}

static bool runtime_child(pmm_cpu_t *cpu,
                          uint64_t *parent, uint32_t index,
                          uint64_t child_va, bool user,
                          uint64_t **out)
{
    uint64_t entry = parent[index];

    if ((entry & PTE_PRESENT) != 0) {
        if ((entry & PTE_LARGE) != 0)
            return false;

        if (user && (entry & PTE_USER) == 0)
            parent[index] = entry | PTE_USER;

        *out = (uint64_t *)(uintptr_t)child_va;
        return true;
    }

    pmm_frame_t frame;
    if (!pmm_alloc4k(cpu, &frame))
        return false;

    /*
     * PMM 只返回已经属于普通 RAM direct-map 的 frame，因此新页表先
     * 通过 direct map 清零，再挂到当前页表树。
     */
    zero_page((void *)(uintptr_t)(VM_DIRECT_MAP_BASE + frame));

    /*
     * 这个 child table 来自运行时 PMM，software bit 9 记录 ownership。
     * early/bootstrap 页表不会带这个 bit，因此 unmap 只会回收真正属于
     * PMM 的运行时页表页。
     */
    parent[index] = (frame & PTE_ADDR) |
                    PTE_PRESENT | PTE_WRITE | PTE_TABLE_PMM_OWNED |
                    (user ? PTE_USER : 0);

    /*
     * 若 CPU 曾缓存这个 recursive VA 的 not-present 结果，父项更新后
     * 先失效它，再通过 recursive mapping 访问新页表。
     */
    invlpg(child_va);

    *out = (uint64_t *)(uintptr_t)child_va;
    return true;
}

void paging_drop_low_half_current(void)
{
    uint64_t *pml4 = (uint64_t *)(uintptr_t)
        recursive_address(PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT);

    /*
     * PML4[0..255] 是低 canonical half。early takeover 只在这里建立
     * 过渡 identity mapping；现在所有仍需访问的对象都已切到 direct map。
     */
    for (uint32_t i = 0; i < 256U; ++i)
        pml4[i] = 0;

    write_cr3(read_cr3());
}

bool paging_map_4k_current(pmm_cpu_t *cpu, vaddr_t va,
                           pmm_frame_t frame, uint64_t attrs)
{
    if (cpu == NULL ||
        (va & (PAGE_4K - 1)) != 0 ||
        (frame & (PAGE_4K - 1)) != 0)
        return false;

    uint32_t i4 = (uint32_t)((va >> 39) & 0x1FFU);
    uint32_t i3 = (uint32_t)((va >> 30) & 0x1FFU);
    uint32_t i2 = (uint32_t)((va >> 21) & 0x1FFU);
    uint32_t i1 = (uint32_t)((va >> 12) & 0x1FFU);
    bool user = (attrs & VM_ATTR_USER) != 0;

    uint64_t pml4_va =
        recursive_address(PAGING_RECURSIVE_SLOT, PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT, PAGING_RECURSIVE_SLOT);
    uint64_t pdpt_va =
        recursive_address(PAGING_RECURSIVE_SLOT, PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT, i4);
    uint64_t pd_va =
        recursive_address(PAGING_RECURSIVE_SLOT, PAGING_RECURSIVE_SLOT,
                          i4, i3);
    uint64_t pt_va =
        recursive_address(PAGING_RECURSIVE_SLOT, i4, i3, i2);

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_va;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;

    if (!runtime_child(cpu, pml4, i4, pdpt_va, user, &pdpt) ||
        !runtime_child(cpu, pdpt, i3, pd_va, user, &pd) ||
        !runtime_child(cpu, pd, i2, pt_va, user, &pt))
        return false;

    if ((pt[i1] & PTE_PRESENT) != 0)
        return false;

    uint64_t flags = PTE_PRESENT;
    if ((attrs & VM_ATTR_WRITE) != 0)
        flags |= PTE_WRITE;
    if (user)
        flags |= PTE_USER;

    uint64_t cache = attrs & VM_ATTR_CACHE_MASK;
    pat_memory_type_t pat_type;

    switch (cache) {
    case 0:
    case VM_ATTR_CACHE_WB:
        pat_type = PAT_MEMORY_WB;
        break;
    case VM_ATTR_CACHE_WC:
        pat_type = PAT_MEMORY_WC;
        break;
    case VM_ATTR_CACHE_UC:
        pat_type = PAT_MEMORY_UC;
        break;
    case VM_ATTR_CACHE_WT:
        pat_type = PAT_MEMORY_WT;
        break;
    case VM_ATTR_CACHE_WP:
        pat_type = PAT_MEMORY_WP;
        break;
    case VM_ATTR_CACHE_UC_MINUS:
        pat_type = PAT_MEMORY_UC_MINUS;
        break;
    default:
        return false;
    }

    uint64_t cache_flags;
    if (!pat_page_flags(pat_type, false, &cache_flags))
        return false;

    flags |= cache_flags;
    pt[i1] = (frame & PTE_ADDR) | flags;
    invlpg(va);
    return true;
}





bool paging_can_map_2m_current(vaddr_t va)
{
    if ((va & (PAGE_2M - 1)) != 0)
        return false;


    uint32_t i4 = (uint32_t)((va >> 39) & 0x1ffU);
    uint32_t i3 = (uint32_t)((va >> 30) & 0x1ffU);
    uint32_t i2 = (uint32_t)((va >> 21) & 0x1ffU);


    uint64_t pml4_va =
        recursive_address(PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT);

    uint64_t pdpt_va =
        recursive_address(PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT,
                          i4);

    uint64_t pd_va =
        recursive_address(PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT,
                          i4,
                          i3);


    uint64_t *pml4 =
        (uint64_t *)(uintptr_t)pml4_va;


    uint64_t e4 = pml4[i4];

    if ((e4 & PTE_PRESENT) == 0)
        return true;


    uint64_t *pdpt =
        (uint64_t *)(uintptr_t)pdpt_va;


    uint64_t e3 = pdpt[i3];

    if ((e3 & PTE_PRESENT) == 0)
        return true;


    if (e3 & PTE_LARGE)
        return false;


    uint64_t *pd =
        (uint64_t *)(uintptr_t)pd_va;


    return (pd[i2] & PTE_PRESENT) == 0;
}


bool paging_map_2m_current(pmm_cpu_t *cpu, vaddr_t va,
                           pmm_frame_t frame, uint64_t attrs)
{
    if (cpu == NULL ||
        (va & (PAGE_2M - 1)) != 0 ||
        (frame & (PAGE_2M - 1)) != 0)
        return false;


    uint32_t i4 = (uint32_t)((va >> 39) & 0x1ffU);
    uint32_t i3 = (uint32_t)((va >> 30) & 0x1ffU);
    uint32_t i2 = (uint32_t)((va >> 21) & 0x1ffU);


    uint64_t pml4_va =
        recursive_address(PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT);

    uint64_t pdpt_va =
        recursive_address(PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT,
                          i4);

    uint64_t pd_va =
        recursive_address(PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT,
                          i4,
                          i3);


    uint64_t *pml4 =
        (uint64_t *)(uintptr_t)pml4_va;

    uint64_t *pdpt;
    uint64_t *pd;


    bool user = (attrs & VM_ATTR_USER) != 0;


    if (!runtime_child(cpu, pml4, i4,
                       pdpt_va, user, &pdpt))
        return false;


    if (!runtime_child(cpu, pdpt, i3,
                       pd_va, user, &pd))
        return false;


    if (pd[i2] & PTE_PRESENT)
        return false;


    uint64_t flags =
        PTE_PRESENT |
        PTE_LARGE;


    if (attrs & VM_ATTR_WRITE)
        flags |= PTE_WRITE;


    if (user)
        flags |= PTE_USER;


    uint64_t cache =
        attrs & VM_ATTR_CACHE_MASK;


    pat_memory_type_t pat_type;


    switch(cache) {
    case 0:
    case VM_ATTR_CACHE_WB:
        pat_type = PAT_MEMORY_WB;
        break;

    case VM_ATTR_CACHE_WC:
        pat_type = PAT_MEMORY_WC;
        break;

    case VM_ATTR_CACHE_UC:
        pat_type = PAT_MEMORY_UC;
        break;

    default:
        return false;
    }


    uint64_t cache_flags;

    if (!pat_page_flags(pat_type,
                        true,
                        &cache_flags))
        return false;


    flags |= cache_flags;


    pd[i2] =
        (frame & PDE_2M_ADDR) |
        flags;


    invlpg(va);

    return true;
}


bool paging_map_range_current(pmm_cpu_t *cpu, vaddr_t va,
                              uint64_t pa, uint64_t size,
                              uint64_t attrs)
{
    if (cpu == NULL || size == 0 ||
        (va & (PAGE_4K - 1)) != 0 ||
        (pa & (PAGE_4K - 1)) != 0 ||
        size > UINT64_MAX - (PAGE_4K - 1))
        return false;

    uint64_t bytes = (size + PAGE_4K - 1) & ~(PAGE_4K - 1);

    if (va > UINT64_MAX - (bytes - 1) ||
        pa > UINT64_MAX - (bytes - 1))
        return false;

    for (uint64_t off = 0; off < bytes; off += PAGE_4K) {
        if (!paging_map_4k_current(cpu, va + off, pa + off, attrs))
            return false;
    }

    return true;
}

static bool table_empty(const uint64_t *table)
{
    for (uint32_t i = 0; i < 512U; ++i) {
        if ((table[i] & PTE_PRESENT) != 0)
            return false;
    }
    return true;
}

static bool release_owned_table(pmm_cpu_t *cpu,
                                uint64_t *parent, uint32_t index,
                                uint64_t child_va)
{
    uint64_t entry = parent[index];

    if ((entry & (PTE_PRESENT | PTE_TABLE_PMM_OWNED)) !=
        (PTE_PRESENT | PTE_TABLE_PMM_OWNED))
        return true;

    pmm_frame_t frame = entry & PTE_ADDR;

    /*
     * 先从页表树断开，再失效 recursive alias，最后归还 PMM。
     * 调用者必须在断开前已经确认 child table 为空。
     */
    parent[index] = 0;
    invlpg(child_va);
    write_cr3(read_cr3());
    return pmm_free4k(cpu, frame);
}

bool paging_unmap_current(pmm_cpu_t *cpu, vaddr_t va,
                          uint64_t max_size,
                          paging_unmap_info_t *out_info)
{
    if (cpu == NULL || out_info == NULL ||
        max_size < PAGE_4K ||
        (va & (PAGE_4K - 1)) != 0)
        return false;

    out_info->frame = 0;
    out_info->page_size = PAGE_4K;
    out_info->mapped = false;

    uint32_t i4 = (uint32_t)((va >> 39) & 0x1FFU);
    uint32_t i3 = (uint32_t)((va >> 30) & 0x1FFU);
    uint32_t i2 = (uint32_t)((va >> 21) & 0x1FFU);
    uint32_t i1 = (uint32_t)((va >> 12) & 0x1FFU);

    uint64_t pml4_va =
        recursive_address(PAGING_RECURSIVE_SLOT, PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT, PAGING_RECURSIVE_SLOT);
    uint64_t pdpt_va =
        recursive_address(PAGING_RECURSIVE_SLOT, PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT, i4);
    uint64_t pd_va =
        recursive_address(PAGING_RECURSIVE_SLOT, PAGING_RECURSIVE_SLOT,
                          i4, i3);
    uint64_t pt_va =
        recursive_address(PAGING_RECURSIVE_SLOT, i4, i3, i2);

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_va;
    uint64_t e4 = pml4[i4];
    if ((e4 & PTE_PRESENT) == 0)
        return true;

    uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_va;
    uint64_t e3 = pdpt[i3];
    if ((e3 & PTE_PRESENT) == 0)
        return true;

    /* PMM 当前没有 1G frame pool；绝不部分拆除 1G leaf。 */
    if ((e3 & PTE_LARGE) != 0)
        return false;

    uint64_t *pd = (uint64_t *)(uintptr_t)pd_va;
    uint64_t e2 = pd[i2];
    if ((e2 & PTE_PRESENT) == 0)
        return true;

    if ((e2 & PTE_LARGE) != 0) {
        /*
         * vm_free 必须从 2M leaf 的起点释放整个 leaf；不允许从中间
         * 地址把完整 2M 映射误释放。
         */
        if ((va & (PAGE_2M - 1)) != 0 ||
            max_size < PAGE_2M)
            return false;

        out_info->frame = e2 & PDE_2M_ADDR;
        out_info->page_size = PAGE_2M;
        out_info->mapped = true;

        pd[i2] = 0;
        invlpg(va);

        /*
         * 清掉 2M leaf 后，PD 可能已经空。若它本身来自运行时 PMM，
         * 继续向上回收；bootstrap table 没有 ownership bit，会保留。
         */
        if (table_empty(pd) &&
            (e3 & PTE_TABLE_PMM_OWNED) != 0) {
            if (!release_owned_table(cpu, pdpt, i3, pd_va))
                return false;

            if (table_empty(pdpt) &&
                (e4 & PTE_TABLE_PMM_OWNED) != 0) {
                if (!release_owned_table(cpu, pml4, i4, pdpt_va))
                    return false;
            }
        }

        return true;
    }

    uint64_t *pt = (uint64_t *)(uintptr_t)pt_va;
    uint64_t pte = pt[i1];
    if ((pte & PTE_PRESENT) == 0)
        return true;

    out_info->frame = pte & PTE_ADDR;
    out_info->page_size = PAGE_4K;
    out_info->mapped = true;

    pt[i1] = 0;
    invlpg(va);

    /*
     * 从 PT 开始逐层回收空的运行时页表。每层 parent entry 上的
     * PTE_TABLE_PMM_OWNED 说明 child frame 来自 PMM，可安全归还。
     */
    if (table_empty(pt) &&
        (e2 & PTE_TABLE_PMM_OWNED) != 0) {
        if (!release_owned_table(cpu, pd, i2, pt_va))
            return false;

        if (table_empty(pd) &&
            (e3 & PTE_TABLE_PMM_OWNED) != 0) {
            if (!release_owned_table(cpu, pdpt, i3, pd_va))
                return false;

            if (table_empty(pdpt) &&
                (e4 & PTE_TABLE_PMM_OWNED) != 0) {
                if (!release_owned_table(cpu, pml4, i4, pdpt_va))
                    return false;
            }
        }
    }

    return true;
}

static bool reserve_direct_run(vm_space_t *space,
                               uint64_t start_pa, uint64_t end_pa)
{
    if (start_pa >= end_pa)
        return true;

    return vm_reserve(space,
                      VM_DIRECT_MAP_BASE + start_pa,
                      end_pa - start_pa,
                      VM_REGION_DIRECT_MAP,
                      VM_ATTR_READ | VM_ATTR_WRITE |
                      VM_ATTR_PINNED | VM_ATTR_CACHE_WB);
}

bool paging_register_direct_map(vm_space_t *space, uint64_t direct_span)
{
    if (space == NULL ||
        direct_span == 0 ||
        direct_span > PAGING_RECURSIVE_BASE - VM_DIRECT_MAP_BASE)
        return false;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)
        recursive_address(PAGING_RECURSIVE_SLOT, PAGING_RECURSIVE_SLOT,
                          PAGING_RECURSIVE_SLOT, PAGING_RECURSIVE_SLOT);

    uint64_t pa = 0;
    uint64_t run_start = 0;
    uint64_t run_end = 0;
    bool have_run = false;

    while (pa < direct_span) {
        uint64_t va = VM_DIRECT_MAP_BASE + pa;
        uint32_t i4 = (uint32_t)((va >> 39) & 0x1FFU);
        uint32_t i3 = (uint32_t)((va >> 30) & 0x1FFU);
        uint32_t i2 = (uint32_t)((va >> 21) & 0x1FFU);
        uint32_t i1 = (uint32_t)((va >> 12) & 0x1FFU);

        uint64_t step;
        bool present = false;
        uint64_t e4 = pml4[i4];

        if ((e4 & PTE_PRESENT) == 0) {
            step = PAGE_512G - (va & (PAGE_512G - 1));
        } else {
            uint64_t *pdpt = (uint64_t *)(uintptr_t)
                recursive_address(PAGING_RECURSIVE_SLOT, PAGING_RECURSIVE_SLOT,
                                  PAGING_RECURSIVE_SLOT, i4);
            uint64_t e3 = pdpt[i3];

            if ((e3 & PTE_PRESENT) == 0) {
                step = PAGE_1G - (va & (PAGE_1G - 1));
            } else if ((e3 & PTE_LARGE) != 0) {
                step = PAGE_1G - (va & (PAGE_1G - 1));
                present = true;
            } else {
                uint64_t *pd = (uint64_t *)(uintptr_t)
                    recursive_address(PAGING_RECURSIVE_SLOT, PAGING_RECURSIVE_SLOT,
                                      i4, i3);
                uint64_t e2 = pd[i2];

                if ((e2 & PTE_PRESENT) == 0) {
                    step = PAGE_2M - (va & (PAGE_2M - 1));
                } else if ((e2 & PTE_LARGE) != 0) {
                    step = PAGE_2M - (va & (PAGE_2M - 1));
                    present = true;
                } else {
                    uint64_t *pt = (uint64_t *)(uintptr_t)
                        recursive_address(PAGING_RECURSIVE_SLOT, i4, i3, i2);

                    step = PAGE_4K;
                    present = (pt[i1] & PTE_PRESENT) != 0;
                }
            }
        }

        if (step > direct_span - pa)
            step = direct_span - pa;

        if (present) {
            if (!have_run) {
                run_start = pa;
                have_run = true;
            }
            run_end = pa + step;
        } else if (have_run) {
            if (!reserve_direct_run(space, run_start, run_end))
                return false;
            have_run = false;
        }

        pa += step;
    }

    if (have_run &&
        !reserve_direct_run(space, run_start, run_end))
        return false;

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

    /*
     * direct map 只能占用 PML4[256..509]。
     * [510] 是 recursive window，[511] 留给高半区内核。
     */
    if (maximum == 0 ||
        maximum > PAGING_RECURSIVE_BASE - VM_DIRECT_MAP_BASE)
        return false;

    *out_span = maximum;
    return true;
}

static bool map_kernel_image(paging_builder_t *b,
                             const BOOT_INFO *bi)
{
    if (bi->kernel_base == 0 ||
        bi->kernel_virt_base == 0 ||
        bi->kernel_size == 0 ||
        (bi->kernel_base & (PAGE_4K - 1)) != 0 ||
        (bi->kernel_virt_base & (PAGE_4K - 1)) != 0 ||
        (bi->kernel_virt_base >> 48) != UINT64_C(0xFFFF) ||
        bi->kernel_size > UINT64_MAX - (PAGE_4K - 1))
        return false;

    uint64_t bytes =
        (bi->kernel_size + PAGE_4K - 1) & ~(PAGE_4K - 1);

    /*
     * kernel_end 是 exclusive end，因此这里要求加 bytes 本身不溢出。
     * 当前固定 VMA 还有完整 2 GiB 空间，正常内核远小于这个上限。
     */
    if (bi->kernel_base > UINT64_MAX - (bytes - 1) ||
        bi->kernel_virt_base > UINT64_MAX - bytes)
        return false;

    uint64_t kernel_end = bi->kernel_virt_base + bytes;

    /*
     * 高半区内核位于 PML4[511]，recursive window 固定在 [510]。
     * 两者必须完全分离。
     */
    uint64_t recursive_end =
        PAGING_RECURSIVE_BASE + PAGING_RECURSIVE_SIZE;

    if (bi->kernel_virt_base < recursive_end &&
        PAGING_RECURSIVE_BASE < kernel_end)
        return false;

    return map_range(b, bi->kernel_virt_base,
                     bi->kernel_base, bytes);
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

    /*
     * PML4[510] 指回 PML4 自身，供接管 CR3 后动态遍历/修改页表。
     * 该 512 GiB VA slot 会在 VM 初始化时永久保留。
     */
    b.pml4[PAGING_RECURSIVE_SLOT] =
        (b.root_phys & PTE_ADDR) | PTE_PRESENT | PTE_WRITE;

    if (!map_all_usable(&b, bi, false))
        return -5;

    if (!map_all_usable(&b, bi, true))
        return -6;

    /*
     * 内核映像自身使用独立的高半区 VMA；direct map 只是物理内存别名，
     * 不能代替真正的链接地址映射。
     */
    if (!map_kernel_image(&b, bi))
        return -7;

    b.info.table_pages = b.used_pages;

    write_cr3(b.root_phys);

    *out_info = b.info;
    return 0;
}
