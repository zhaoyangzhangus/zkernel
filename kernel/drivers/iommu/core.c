#include <arch/x86_64/acpi.h>
#include <arch/x86_64/paging.h>
#include <kernel/iommu.h>
#include <kernel/kmem.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>

/*
 * 这是一个最小的 Intel VT-d 后端。当前硬件模型使用 legacy root/context
 * 表和四级 4 KiB I/O 页表；它覆盖了 PCI DMA 所需的隔离、映射和撤销映射。
 * 多个 DRHD 保留在 ACPI 结果中，但本实现先绑定首个 DRHD，后续再扩展
 * 到按 PCI segment 选择 IOMMU 单元。
 */
#define IOMMU_MMIO_VA          (X86_64_MMIO_BASE + 0x03000000ULL)
#define IOMMU_MMIO_PAGES       16U
#define IOMMU_MAX_DOMAINS      256U
#define IOMMU_ROOT_ENTRIES     256U
#define IOMMU_CONTEXT_ENTRIES  256U
#define IOMMU_PAGE_ENTRIES     512U
#define IOMMU_PTE_ADDRESS       0x000FFFFFFFFFF000ULL


#define VTD_REG_CAP             0x0008U
#define VTD_REG_ECAP            0x0010U
#define VTD_REG_GCMD            0x0018U
#define VTD_REG_GSTS            0x001CU
#define VTD_REG_RTADDR          0x0020U
#define VTD_REG_CCMD            0x0028U
#define VTD_GCMD_TE             (1U << 31)
#define VTD_GCMD_SRTP           (1U << 30)
#define VTD_GSTS_TES            (1U << 31)
#define VTD_GSTS_RTPS           (1U << 30)
#define VTD_CCMD_ICC            (1ULL << 63)
#define VTD_CCMD_CIRG_GLOBAL    (1ULL << 61)
#define VTD_ENTRY_PRESENT       (1ULL << 0)
#define VTD_ENTRY_READ          (1ULL << 0)
#define VTD_ENTRY_WRITE         (1ULL << 1)
#define VTD_CONTEXT_AW_4LEVEL   2ULL
#define VTD_CONTEXT_TT_TRANSLATE 0ULL
#define VTD_MAX_IOVA            (1ULL << 48)

typedef struct iommu_domain {
    bool used;
    uint16_t domain_id;
    uint16_t segment;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    struct device *device;
    page_t *root_page;
} iommu_domain_t;

typedef struct iommu_unit {
    volatile uint8_t *registers;
    uint64_t base;
    uint64_t capabilities;
    uint64_t extended_capabilities;
    page_t *root_table_page;
    page_t *context_pages[256];
    iommu_domain_t domains[IOMMU_MAX_DOMAINS];
    bool present;
    bool enabled;
} iommu_unit_t;

static iommu_unit_t g_iommu;
static spinlock_t g_iommu_lock;
static atomic_uint g_iommu_init_state;
static bool g_iommu_has_dmar;

static uint32_t iommu_read32(uint32_t offset) {
    return *(volatile const uint32_t *)(g_iommu.registers + offset);
}

static void iommu_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(g_iommu.registers + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
}

static uint64_t iommu_read64(uint32_t offset) {
    return *(volatile const uint64_t *)(g_iommu.registers + offset);
}

static void iommu_write64(uint32_t offset, uint64_t value) {
    *(volatile uint64_t *)(g_iommu.registers + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
}

static void iommu_lock(void) {
    /* DMA 可能已固定调用方，因此此保护机制必须能与其嵌套使用。 */
    sched_preempt_disable();
    spinlock_lock(&g_iommu_lock);
}

static void iommu_unlock(void) {
    spinlock_unlock(&g_iommu_lock);
    sched_preempt_enable();
}

static bool iommu_wait_register(uint32_t offset, uint32_t mask, bool set) {
    for (uint32_t pass = 0; pass < 1000000U; ++pass) {
        bool value = (iommu_read32(offset) & mask) != 0;
        if (value == set) return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool iommu_map_registers(uint64_t physical) {
    paddr_t root = x86_current_root_table();
    for (uint32_t index = 0; index < IOMMU_MMIO_PAGES; ++index) {
        kstatus_t status = x86_map_page(root, IOMMU_MMIO_VA +
                                        (uint64_t)index * PAGE_SIZE,
                                        paddr_make(physical +
                                                   (uint64_t)index * PAGE_SIZE),
                                        X86_PAGE_WRITE | X86_PAGE_GLOBAL,
                                        X86_CACHE_UC);
        if (status != K_OK) {
            for (uint32_t done = 0; done < index; ++done) {
                (void)x86_unmap_page(root, IOMMU_MMIO_VA +
                                     (uint64_t)done * PAGE_SIZE, 0);
            }
            return false;
        }
    }
    g_iommu.registers = (volatile uint8_t *)(uintptr_t)IOMMU_MMIO_VA;
    return true;
}

static void iommu_unmap_registers(void) {
    paddr_t root = x86_current_root_table();
    for (uint32_t index = 0; index < IOMMU_MMIO_PAGES; ++index) {
        (void)x86_unmap_page(root, IOMMU_MMIO_VA + (uint64_t)index * PAGE_SIZE, 0);
    }
    g_iommu.registers = 0;
}

static void iommu_free_page(page_t **page) {
    if (page == 0 || *page == 0) return;
    page_free(*page);
    *page = 0;
}

static bool iommu_program_translation(void) {
    uint64_t physical = page_to_phys(g_iommu.root_table_page).value;
    if (physical == UINT64_MAX || (physical & (PAGE_SIZE - 1ULL)) != 0) return false;
    iommu_write64(VTD_REG_RTADDR, physical);
    uint32_t command = iommu_read32(VTD_REG_GCMD);
    iommu_write32(VTD_REG_GCMD, command | VTD_GCMD_SRTP);
    if (!iommu_wait_register(VTD_REG_GSTS, VTD_GSTS_RTPS, true)) return false;
    command = iommu_read32(VTD_REG_GCMD);
    iommu_write32(VTD_REG_GCMD, command | VTD_GCMD_TE);
    return iommu_wait_register(VTD_REG_GSTS, VTD_GSTS_TES, true);
}

static bool iommu_global_invalidate(void) {
    iommu_write64(VTD_REG_CCMD, VTD_CCMD_ICC | VTD_CCMD_CIRG_GLOBAL);
    for (uint32_t pass = 0; pass < 1000000U; ++pass) {
        if ((iommu_read64(VTD_REG_CCMD) & VTD_CCMD_ICC) == 0) return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static iommu_domain_t *iommu_find_domain(struct device *device) {
    for (uint32_t index = 0; index < IOMMU_MAX_DOMAINS; ++index) {
        if (g_iommu.domains[index].used && g_iommu.domains[index].device == device) {
            return &g_iommu.domains[index];
        }
    }
    return 0;
}

static iommu_domain_t *iommu_find_domain_bdf(uint16_t segment, uint8_t bus,
                                             uint8_t slot, uint8_t function) {
    for (uint32_t index = 0; index < IOMMU_MAX_DOMAINS; ++index) {
        iommu_domain_t *domain = &g_iommu.domains[index];
        if (domain->used && domain->segment == segment && domain->bus == bus &&
            domain->slot == slot && domain->function == function) return domain;
    }
    return 0;
}

static uint64_t *iommu_page_table(page_t *page) {
    if (page == 0) return 0;
    return (uint64_t *)phys_to_direct(page_to_phys(page));
}

static uint64_t *iommu_next_table(uint64_t *entry, bool allocate) {
    if (entry == 0) return 0;
    if ((*entry & VTD_ENTRY_PRESENT) != 0) {
        return (uint64_t *)phys_to_direct(paddr_make(*entry & IOMMU_PTE_ADDRESS));
    }
    if (!allocate) return 0;
    page_t *page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (page == 0) return 0;
    page->owner = PAGE_OWNER_PAGETABLE;
    paddr_t physical = page_to_phys(page);
    uint64_t *table = (uint64_t *)phys_to_direct(physical);
    if (table == 0 || physical.value == UINT64_MAX) {
        page_free(page);
        return 0;
    }
    *entry = physical.value | VTD_ENTRY_READ | VTD_ENTRY_WRITE;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return table;
}

static uint64_t *iommu_pte_for(iommu_domain_t *domain, uint64_t iova,
                               bool allocate) {
    uint64_t *table = iommu_page_table(domain != 0 ? domain->root_page : 0);
    if (table == 0 || iova >= VTD_MAX_IOVA) return 0;
    table = iommu_next_table(&table[(iova >> 39) & 0x1FFU], allocate);
    table = iommu_next_table(table != 0 ? &table[(iova >> 30) & 0x1FFU] : 0,
                             allocate);
    table = iommu_next_table(table != 0 ? &table[(iova >> 21) & 0x1FFU] : 0,
                             allocate);
    if (table == 0) return 0;
    return &table[(iova >> 12) & 0x1FFU];
}

/* domain 根表只拥有中间页表；叶子 PTE 指向设备 DMA 页，不能由这里释放。 */
static void iommu_free_table(page_t *page, uint32_t level) {
    uint64_t *table;
    if (page == 0) return;
    table = iommu_page_table(page);
    if (table != 0 && level != 0U) {
        for (uint32_t index = 0U; index < IOMMU_PAGE_ENTRIES; ++index) {
            uint64_t entry = table[index];
            if ((entry & VTD_ENTRY_PRESENT) == 0U) continue;
            page_t *child = phys_to_page(paddr_make(entry & IOMMU_PTE_ADDRESS));
            table[index] = 0U;
            if (child != 0) iommu_free_table(child, level - 1U);
        }
    }
    page_free(page);
}

static kstatus_t iommu_map_one(iommu_domain_t *domain, uint64_t iova,
                               paddr_t physical, uint32_t access) {
    if (domain == 0 || (physical.value & (PAGE_SIZE - 1ULL)) != 0 ||
        physical.value > IOMMU_PTE_ADDRESS ||
        (access & (IOMMU_MAP_DEVICE_READ | IOMMU_MAP_DEVICE_WRITE)) == 0) {
        return K_EINVAL;
    }
    uint64_t *pte = iommu_pte_for(domain, iova, true);
    if (pte == 0 || (*pte & VTD_ENTRY_PRESENT) != 0) return K_EBUSY;
    uint64_t value = physical.value;
    if ((access & IOMMU_MAP_DEVICE_READ) != 0) value |= VTD_ENTRY_READ;
    if ((access & IOMMU_MAP_DEVICE_WRITE) != 0) value |= VTD_ENTRY_WRITE;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *pte = value;
    return K_OK;
}

bool iommu_init(void) {
    unsigned expected = 0U;
    if (!atomic_compare_exchange_strong_explicit(&g_iommu_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        while (atomic_load_explicit(&g_iommu_init_state, memory_order_acquire) == 1U) {
            __asm__ volatile ("pause");
        }
        return atomic_load_explicit(&g_iommu_init_state, memory_order_acquire) == 2U;
    }

    const x86_acpi_platform_t *platform = x86_acpi_platform();
    g_iommu_has_dmar = platform != 0 && platform->iommu_count != 0;
    bool success = true;
    spinlock_init(&g_iommu_lock);
    if (g_iommu_has_dmar) {
        const x86_acpi_iommu_t *description = &platform->iommus[0];
        g_iommu.base = description->base;
        g_iommu.present = iommu_map_registers(g_iommu.base);
        if (g_iommu.present) {
            g_iommu.capabilities = iommu_read64(VTD_REG_CAP);
            g_iommu.extended_capabilities = iommu_read64(VTD_REG_ECAP);
            uint32_t sagaw = (uint32_t)((g_iommu.capabilities >> 8) & 0x1FU);
            if ((sagaw & (1U << 2)) == 0) g_iommu.present = false;
        }
        if (g_iommu.present) {
            g_iommu.root_table_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
            if (g_iommu.root_table_page == 0) g_iommu.present = false;
            else g_iommu.root_table_page->owner = PAGE_OWNER_PAGETABLE;
        }
        if (g_iommu.present && !iommu_program_translation()) g_iommu.present = false;
        if (!g_iommu.present) {
            success = false;
            iommu_free_page(&g_iommu.root_table_page);
            iommu_unmap_registers();
        } else {
            g_iommu.enabled = true;
        }
    }
    atomic_store_explicit(&g_iommu_init_state, success ? 2U : 3U, memory_order_release);
    return success;
}

bool iommu_available(void) {
    return g_iommu_has_dmar;
}

bool iommu_hardware_enabled(void) {
    return g_iommu.enabled;
}

kstatus_t iommu_attach_pci_device(struct device *device, uint16_t segment,
                                  uint8_t bus, uint8_t slot, uint8_t function) {
    if (device == 0 || slot >= 32U || function >= 8U) return K_EINVAL;
    if (!g_iommu.enabled) return K_OK;
    iommu_lock();
    if (iommu_find_domain(device) != 0) {
        iommu_unlock();
        return K_OK;
    }
    if (iommu_find_domain_bdf(segment, bus, slot, function) != 0) {
        iommu_unlock();
        return K_EBUSY;
    }
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (platform == 0 || platform->iommu_count == 0 ||
        (platform->iommus[0].segment != segment &&
         (platform->iommus[0].flags & 1U) == 0)) {
        iommu_unlock();
        return K_ENOENT;
    }
    iommu_domain_t *domain = 0;
    for (uint32_t index = 0; index < IOMMU_MAX_DOMAINS; ++index) {
        if (!g_iommu.domains[index].used) {
            domain = &g_iommu.domains[index];
            domain->used = true;
            domain->domain_id = (uint16_t)(index + 1U);
            break;
        }
    }
    if (domain == 0) {
        iommu_unlock();
        return K_ENOMEM;
    }
    domain->root_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (domain->root_page == 0) {
        domain->used = false;
        iommu_unlock();
        return K_ENOMEM;
    }
    domain->root_page->owner = PAGE_OWNER_PAGETABLE;
    domain->device = device;
    domain->segment = segment;
    domain->bus = bus;
    domain->slot = slot;
    domain->function = function;

    page_t **context_page = &g_iommu.context_pages[bus];
    if (*context_page == 0) {
        *context_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
        if (*context_page == 0) {
            page_free(domain->root_page);
            domain->root_page = 0;
            domain->device = 0;
            domain->used = false;
            iommu_unlock();
            return K_ENOMEM;
        }
        (*context_page)->owner = PAGE_OWNER_PAGETABLE;
        uint64_t *root = (uint64_t *)phys_to_direct(
            page_to_phys(g_iommu.root_table_page));
        root[bus] = page_to_phys(*context_page).value | VTD_ENTRY_PRESENT;
        __atomic_thread_fence(__ATOMIC_RELEASE);
    }
    uint64_t *context = (uint64_t *)phys_to_direct(page_to_phys(*context_page));
    uint32_t context_index = (uint32_t)slot * 8U + function;
    uint64_t *entry = &context[context_index * 2U];
    *entry = page_to_phys(domain->root_page).value |
             (VTD_CONTEXT_TT_TRANSLATE << 2) | VTD_ENTRY_PRESENT;
    entry[1] = VTD_CONTEXT_AW_4LEVEL | ((uint64_t)domain->domain_id << 8);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    bool invalidated = iommu_global_invalidate();
    iommu_unlock();
    return invalidated ? K_OK : K_EIO;
}

kstatus_t iommu_detach_device(struct device *device) {
    iommu_domain_t *domain;
    uint8_t bus;
    bool bus_still_used = false;
    if (device == 0) return K_EINVAL;
    if (!g_iommu.enabled) return K_OK;
    iommu_lock();
    domain = iommu_find_domain(device);
    if (domain == 0) {
        iommu_unlock();
        return K_ENOENT;
    }
    bus = domain->bus;
    for (uint32_t index = 0U; index < IOMMU_MAX_DOMAINS; ++index) {
        if (g_iommu.domains[index].used &&
            &g_iommu.domains[index] != domain &&
            g_iommu.domains[index].bus == bus) {
            bus_still_used = true;
            break;
        }
    }
    uint64_t *root = iommu_page_table(g_iommu.root_table_page);
    if (root != 0 && !bus_still_used) root[bus] = 0U;
    iommu_free_table(domain->root_page, 3U);
    domain->root_page = 0;
    domain->device = 0;
    domain->used = false;
    if (!bus_still_used) iommu_free_page(&g_iommu.context_pages[bus]);
    bool invalidated = iommu_global_invalidate();
    iommu_unlock();
    return invalidated ? K_OK : K_EIO;
}

kstatus_t iommu_map_pages(struct device *device, iova_t iova, page_t **pages,
                          uint32_t page_count, uint32_t access) {
    if (!g_iommu.enabled || device == 0 || pages == 0 || page_count == 0 ||
        (iova.value & (PAGE_SIZE - 1ULL)) != 0 ||
        iova.value >= VTD_MAX_IOVA ||
        page_count > (VTD_MAX_IOVA - iova.value) / PAGE_SIZE ||
        (access & (IOMMU_MAP_DEVICE_READ | IOMMU_MAP_DEVICE_WRITE)) == 0) {
        return g_iommu.enabled ? K_EINVAL : K_OK;
    }
    iommu_lock();
    iommu_domain_t *domain = iommu_find_domain(device);
    if (domain == 0) {
        iommu_unlock();
        return K_ENOENT;
    }
    uint32_t mapped = 0;
    kstatus_t status = K_OK;
    for (; mapped < page_count; ++mapped) {
        if (pages[mapped] == 0 || iommu_map_one(domain,
                                                iova.value + (uint64_t)mapped * PAGE_SIZE,
                                                page_to_phys(pages[mapped]), access) != K_OK) {
            status = K_EIO;
            break;
        }
    }
    if (status != K_OK) {
        for (uint32_t index = 0; index < mapped; ++index) {
            uint64_t *pte = iommu_pte_for(domain,
                                          iova.value + (uint64_t)index * PAGE_SIZE,
                                          false);
            if (pte != 0) *pte = 0;
        }
    }
    if (status == K_OK) status = iommu_global_invalidate() ? K_OK : K_EIO;
    iommu_unlock();
    return status;
}

kstatus_t iommu_unmap_pages(struct device *device, iova_t iova, uint64_t length) {
    if (!g_iommu.enabled) return K_OK;
    if (device == 0 || length == 0 || (iova.value & (PAGE_SIZE - 1ULL)) != 0 ||
        (length & (PAGE_SIZE - 1ULL)) != 0 || iova.value >= VTD_MAX_IOVA ||
        length > VTD_MAX_IOVA - iova.value) return K_EINVAL;
    iommu_lock();
    iommu_domain_t *domain = iommu_find_domain(device);
    if (domain == 0) {
        iommu_unlock();
        return K_ENOENT;
    }
    for (uint64_t offset = 0; offset < length; offset += PAGE_SIZE) {
        uint64_t *pte = iommu_pte_for(domain, iova.value + offset, false);
        if (pte != 0) *pte = 0;
    }
    bool invalidated = iommu_global_invalidate();
    iommu_unlock();
    return invalidated ? K_OK : K_EIO;
}

bool iommu_self_test(void) {
    if (!iommu_init()) return false;
    return !g_iommu_has_dmar ||
           (g_iommu.enabled && g_iommu.root_table_page != 0 &&
            g_iommu.registers != 0);
}
