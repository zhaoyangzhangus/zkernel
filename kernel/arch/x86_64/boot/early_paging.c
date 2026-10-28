#include <kernel/mm_boot.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/mm.h>

/* 早期页表同时提供低端恒等映射、64 TiB 直接映射和高半内核映射。 */
#define DIRECT_MAP_PML4_COUNT 128U
#define DIRECT_MAP_1G_SIZE    (1ULL << 30)
#define DIRECT_MAP_2M_SIZE    (1ULL << 21)
#define DIRECT_PTE_PRESENT    (1ULL << 0)
#define DIRECT_PTE_WRITE      (1ULL << 1)
#define DIRECT_PTE_LARGE      (1ULL << 7)
#define DIRECT_PTE_GLOBAL     (1ULL << 8)
#define DIRECT_PTE_NX         (1ULL << 63)
#define DIRECT_PTE_ADDRESS    0x000FFFFFFFFFF000ULL
#define EFI_MEMORY_WC         0x0000000000000002ULL
#define EFI_MEMORY_WB         0x0000000000000008ULL
static UINT64 g_kernel_pml4[512] __attribute__((aligned(4096)));
static UINT64 g_low_pdpt[4] __attribute__((aligned(4096)));
static UINT64 g_low_pd[4][512] __attribute__((aligned(4096)));
static UINT64 g_direct_pdpt[DIRECT_MAP_PML4_COUNT][512] __attribute__((aligned(4096)));
static UINT64 g_kernel_pdpt[512] __attribute__((aligned(4096)));
static UINT64 g_kernel_pd[512] __attribute__((aligned(4096)));
static UINT64 g_kernel_pt[16][512] __attribute__((aligned(4096)));
static UINT64 g_pml4_physical;
static const OS_BOOT_INFO *g_boot_info;

static UINT64 table_physical(const VOID *table) {
    UINT64 virtual_address = (UINT64)(uintptr_t)table;
    if (g_boot_info == 0 || virtual_address < g_boot_info->KernelVirtualBase ||
        virtual_address - g_boot_info->KernelVirtualBase >= g_boot_info->KernelSize) return 0;
    return g_boot_info->KernelPhysicalBase +
           (virtual_address - g_boot_info->KernelVirtualBase);
}

static BOOLEAN map_kernel_image(void) {
    UINT64 start = g_boot_info->KernelVirtualBase;
    UINT64 end = start + g_boot_info->KernelSize;
    if (end < start) return 0;
    UINT64 pdpt_physical = table_physical(g_kernel_pdpt);
    if (pdpt_physical == 0) return 0;
    g_kernel_pml4[511] = pdpt_physical | 0x03ULL;

    for (UINT64 virtual_address = start; virtual_address < end; virtual_address += 4096ULL) {
        UINTN pml4_index = (UINTN)((virtual_address >> 39) & 0x1FFULL);
        UINTN pdpt_index = (UINTN)((virtual_address >> 30) & 0x1FFULL);
        UINTN pd_index = (UINTN)((virtual_address >> 21) & 0x1FFULL);
        UINTN pt_index = (UINTN)((virtual_address >> 12) & 0x1FFULL);
        if (pml4_index != 511U || pd_index >= 16U) return 0;
        if (g_kernel_pdpt[pdpt_index] == 0) {
            UINT64 pd_table = table_physical(g_kernel_pd);
            if (pd_table == 0) return 0;
            g_kernel_pdpt[pdpt_index] = pd_table | 0x03ULL;
        }
        if (g_kernel_pd[pd_index] == 0) {
            UINT64 pt_table = table_physical(g_kernel_pt[pd_index]);
            if (pt_table == 0) return 0;
            g_kernel_pd[pd_index] = pt_table | 0x03ULL;
        }
        UINT64 physical = g_boot_info->KernelPhysicalBase +
                          (virtual_address - start);
        if (physical < g_boot_info->KernelPhysicalBase) return 0;
        g_kernel_pt[pd_index][pt_index] = physical | 0x03ULL;
    }
    return 1;
}

BOOLEAN arch_enable_kernel_paging(const OS_BOOT_INFO *boot_info) {
    if (boot_info == 0 || boot_info->KernelPhysicalBase == 0 ||
        boot_info->KernelVirtualBase == 0 || boot_info->KernelSize == 0 ||
        (boot_info->KernelSize & 0xFFFULL) != 0) return 0;
    g_boot_info = boot_info;
    for (UINTN i = 0; i < 512U; ++i) {
        g_kernel_pml4[i] = 0;
        g_kernel_pdpt[i] = 0;
        g_kernel_pd[i] = 0;
    }
    for (UINTN i = 0; i < 4U; ++i) g_low_pdpt[i] = 0;
    for (UINTN pml4 = 0; pml4 < DIRECT_MAP_PML4_COUNT; ++pml4) {
        for (UINTN pdpt = 0; pdpt < 512U; ++pdpt) {
            UINT64 physical = ((UINT64)pml4 * 512ULL + pdpt) * (1ULL << 30);
            g_direct_pdpt[pml4][pdpt] = physical | 0x83ULL;
        }
    }
    for (UINTN i = 0; i < 16U * 512U; ++i) ((UINT64 *)g_kernel_pt)[i] = 0;

    for (UINTN table = 0; table < 4U; ++table) {
        UINT64 pd_physical = table_physical(g_low_pd[table]);
        if (pd_physical == 0) return 0;
        g_low_pdpt[table] = pd_physical | 0x03ULL;
        for (UINTN entry = 0; entry < 512U; ++entry) {
            UINT64 physical = ((UINT64)table * 512ULL + entry) * 0x200000ULL;
            g_low_pd[table][entry] = physical | 0x83ULL;
        }
    }

    UINT64 low_pdpt_physical = table_physical(g_low_pdpt);
    g_pml4_physical = table_physical(g_kernel_pml4);
    if (low_pdpt_physical == 0 || g_pml4_physical == 0) return 0;
    g_kernel_pml4[0] = low_pdpt_physical | 0x03ULL;
    /* 0xffff800000000000 对应 PML4[256]，每个 PDPT 项映射 1 GiB。 */
    for (UINTN pml4 = 0; pml4 < DIRECT_MAP_PML4_COUNT; ++pml4) {
        UINT64 direct_pdpt = table_physical(g_direct_pdpt[pml4]);
        if (direct_pdpt == 0) return 0;
        g_kernel_pml4[256U + pml4] = direct_pdpt | 0x03ULL;
    }
    if (!map_kernel_image()) return 0;

    __asm__ volatile ("mov %0, %%cr3" : : "r"(g_pml4_physical) : "memory");
    x86_paging_arch_init();
    return 1;
}

static UINT64 *allocate_direct_table(page_t **allocated, UINT64 *physical_out) {
    page_t *page = page_alloc(0, PAGE_ALLOC_ZERO);
    if (page == 0) return 0;
    paddr_t physical = page_to_phys(page);
    UINT64 *table = (UINT64 *)phys_to_direct(physical);
    if (physical.value == UINT64_MAX || table == 0) {
        page_free(page);
        return 0;
    }
    page->owner = PAGE_OWNER_PAGETABLE;
    /* 已提交的直接映射页表在内核整个生命周期内持续有效。
     * 必须避免普通页面释放路径回收它们，否则过期的页面池位图可能会把
     * 仍在使用的页表页交给 kmalloc，进而破坏映射。 */
    page->flags |= PAGE_PINNED;
    page->private_data = (UINT64)(uintptr_t)*allocated;
    *allocated = page;
    *physical_out = physical.value;
    return table;
}

static UINT64 *ensure_direct_table(UINT64 *entry, page_t **allocated) {
    if ((*entry & DIRECT_PTE_PRESENT) != 0) {
        if ((*entry & DIRECT_PTE_LARGE) != 0) return 0;
        return (UINT64 *)phys_to_direct(paddr_make(*entry & DIRECT_PTE_ADDRESS));
    }
    UINT64 physical;
    UINT64 *table = allocate_direct_table(allocated, &physical);
    if (table == 0) return 0;
    *entry = physical | DIRECT_PTE_PRESENT | DIRECT_PTE_WRITE;
    return table;
}

static UINT64 direct_leaf(UINT64 physical, enum x86_cache_mode cache_mode,
                          BOOLEAN large) {
    UINT64 entry = physical | DIRECT_PTE_PRESENT | DIRECT_PTE_WRITE |
                   DIRECT_PTE_GLOBAL | x86_pte_cache_bits(cache_mode, large != 0);
    if (large) entry |= DIRECT_PTE_LARGE;
    if (x86_boot_cpu_features.nx) entry |= DIRECT_PTE_NX;
    return entry;
}

static BOOLEAN map_direct_chunk(UINT64 roots[DIRECT_MAP_PML4_COUNT],
                                UINT64 physical, UINT64 size,
                                enum x86_cache_mode cache_mode,
                                page_t **allocated) {
    UINT64 virtual_address = X86_64_DIRECT_MAP_BASE + physical;
    UINTN pml4_index = (UINTN)((virtual_address >> 39) & 0x1FFULL);
    UINTN pdpt_index = (UINTN)((virtual_address >> 30) & 0x1FFULL);
    UINTN pd_index = (UINTN)((virtual_address >> 21) & 0x1FFULL);
    UINTN pt_index = (UINTN)((virtual_address >> 12) & 0x1FFULL);
    if (pml4_index < 256U || pml4_index >= 256U + DIRECT_MAP_PML4_COUNT) return 0;

    UINT64 *pdpt = ensure_direct_table(&roots[pml4_index - 256U], allocated);
    if (pdpt == 0) return 0;
    if (size == DIRECT_MAP_1G_SIZE) {
        if (pdpt[pdpt_index] != 0) return 0;
        pdpt[pdpt_index] = direct_leaf(physical, cache_mode, 1);
        return 1;
    }

    UINT64 *pd = ensure_direct_table(&pdpt[pdpt_index], allocated);
    if (pd == 0) return 0;
    if (size == DIRECT_MAP_2M_SIZE) {
        if (pd[pd_index] != 0) return 0;
        pd[pd_index] = direct_leaf(physical, cache_mode, 1);
        return 1;
    }

    UINT64 *pt = ensure_direct_table(&pd[pd_index], allocated);
    if (pt == 0 || pt[pt_index] != 0 || size != PAGE_SIZE) return 0;
    pt[pt_index] = direct_leaf(physical, cache_mode, 0);
    return 1;
}

static VOID release_direct_tables(page_t *allocated) {
    while (allocated != 0) {
        page_t *next = (page_t *)(uintptr_t)allocated->private_data;
        allocated->private_data = 0;
        allocated->flags &= ~PAGE_PINNED;
        page_free(allocated);
        allocated = next;
    }
}

static VOID commit_direct_tables(page_t *allocated) {
    while (allocated != 0) {
        page_t *next = (page_t *)(uintptr_t)allocated->private_data;
        allocated->private_data = 0;
        allocated = next;
    }
}

static enum x86_cache_mode ram_cache_mode(UINT64 attributes) {
    if ((attributes & EFI_MEMORY_WB) != 0 || attributes == 0) return X86_CACHE_WB;
    if ((attributes & EFI_MEMORY_WC) != 0) return X86_CACHE_WC;
    return X86_CACHE_UC;
}

BOOLEAN arch_rebuild_ram_direct_map(const OS_BOOT_INFO *boot_info) {
    static UINT64 new_roots[DIRECT_MAP_PML4_COUNT];
    if (boot_info == 0 || x86_current_root_table().value != g_pml4_physical ||
        !direct_map_prepare_ram_ranges(boot_info)) return 0;

    UINT64 direct_span = X86_64_DIRECT_MAP_END - X86_64_DIRECT_MAP_BASE + 1ULL;
    UINT64 boot_physical = boot_info->BootInfoPhysicalBase != 0 ?
                           boot_info->BootInfoPhysicalBase : (UINT64)(uintptr_t)boot_info;
    UINT64 map_capacity = boot_info->MemoryMapBufferSize != 0 ?
                          boot_info->MemoryMapBufferSize : boot_info->MemoryMapSize;
    if (!direct_map_range_is_ram(paddr_make(g_pml4_physical), PAGE_SIZE) ||
        !direct_map_range_is_ram(paddr_make(boot_info->KernelPhysicalBase),
                                 (size_t)boot_info->KernelSize) ||
        !direct_map_range_is_ram(paddr_make(boot_physical), sizeof(*boot_info)) ||
        !direct_map_range_is_ram(paddr_make(boot_info->MemoryMap), (size_t)map_capacity) ||
        !direct_map_range_is_ram(paddr_make(boot_info->BootstrapStackBase),
                                 (size_t)boot_info->BootstrapStackSize) ||
        !direct_map_range_is_ram(paddr_make(boot_info->ApTrampolineBase),
                                 (size_t)boot_info->ApTrampolineSize)) return 0;

    for (UINTN i = 0; i < DIRECT_MAP_PML4_COUNT; ++i) new_roots[i] = 0;
    page_t *allocated = 0;
    const UINT8 *map = (const UINT8 *)(uintptr_t)boot_info->MemoryMap;
    UINT64 descriptor_size = boot_info->MemoryDescriptorSize;
    for (UINT64 offset = 0;
         offset <= boot_info->MemoryMapSize - descriptor_size;
         offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *descriptor =
            (const EFI_MEMORY_DESCRIPTOR *)(map + offset);
        if (descriptor->NumberOfPages == 0 ||
            descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE) continue;
        UINT64 bytes = descriptor->NumberOfPages * PAGE_SIZE;
        UINT64 physical = descriptor->PhysicalStart;
        if (physical >= direct_span || physical > UINT64_MAX - bytes) continue;
        UINT64 end = physical + bytes;
        if (end > direct_span) end = direct_span;
        if (end <= physical ||
            !direct_map_range_is_ram(paddr_make(physical), (size_t)(end - physical))) continue;

        enum x86_cache_mode cache_mode = ram_cache_mode(descriptor->Attribute);
        while (physical < end) {
            UINT64 remaining = end - physical;
            UINT64 chunk = PAGE_SIZE;
            if ((physical & (DIRECT_MAP_1G_SIZE - 1ULL)) == 0 &&
                remaining >= DIRECT_MAP_1G_SIZE) {
                chunk = DIRECT_MAP_1G_SIZE;
            } else if ((physical & (DIRECT_MAP_2M_SIZE - 1ULL)) == 0 &&
                       remaining >= DIRECT_MAP_2M_SIZE) {
                chunk = DIRECT_MAP_2M_SIZE;
            }
            if (!map_direct_chunk(new_roots, physical, chunk, cache_mode, &allocated)) {
                release_direct_tables(allocated);
                return 0;
            }
            physical += chunk;
        }
    }

    /* 所有新表均已脱离当前树构造完成；一次性替换后重载 CR3，不留下半成品窗口。 */
    commit_direct_tables(allocated);
    for (UINTN i = 0; i < DIRECT_MAP_PML4_COUNT; ++i) {
        g_kernel_pml4[256U + i] = new_roots[i];
    }
    __asm__ volatile ("mov %0, %%cr3" : : "r"(g_pml4_physical) : "memory");
    direct_map_enable_validation();
    return phys_to_direct(paddr_make(boot_physical)) != 0;
}

UINT64 arch_identity_pml4_address(void) {
    return g_pml4_physical;
}

BOOLEAN arch_drop_identity_mapping(void) {
    if (g_pml4_physical == 0 ||
        x86_current_root_table().value != g_pml4_physical) return 0;

    g_kernel_pml4[0] = 0;
    /* 重载 CR3，确保本 CPU 不再命中低端恒等映射留下的 TLB 项。 */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(g_pml4_physical) : "memory");
    return g_kernel_pml4[0] == 0;
}
