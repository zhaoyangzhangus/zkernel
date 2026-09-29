#include <stdint.h>

#include "../boot/bootinfo.h"
#include "../lib/printf.h"
#include "../arch/x86_64/idt.h"
#include "../mm/page_fault.h"
#include "../mm/pmm.h"
#include "../mm/paging.h"
#include "../mm/vm.h"

static void halt(void)
{
    for (;;)
        __asm__ volatile("hlt");
}

static uint64_t usable_pages(const BOOT_INFO *bi)
{
    uint64_t pages = 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_addr;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)p;
        if (d->type == MEM_CONVENTIONAL ||
            d->type == MEM_LOADER_CODE ||
            d->type == MEM_LOADER_DATA ||
            d->type == MEM_BOOT_SERVICES_CODE ||
            d->type == MEM_BOOT_SERVICES_DATA)
            pages += d->number_of_pages;
    }
    return pages;
}

static const char *mem_type_name(uint32_t type)
{
    switch (type) {
    case MEM_RESERVED:              return "Reserved";
    case MEM_LOADER_CODE:           return "LoaderCode";
    case MEM_LOADER_DATA:           return "LoaderData";
    case MEM_BOOT_SERVICES_CODE:    return "BootServicesCode";
    case MEM_BOOT_SERVICES_DATA:    return "BootServicesData";
    case MEM_RUNTIME_SERVICES_CODE: return "RuntimeServicesCode";
    case MEM_RUNTIME_SERVICES_DATA: return "RuntimeServicesData";
    case MEM_CONVENTIONAL:          return "Conventional";
    case MEM_UNUSABLE:              return "Unusable";
    case MEM_ACPI_RECLAIM:          return "ACPIReclaim";
    case MEM_ACPI_NVS:              return "ACPINVS";
    case MEM_MMIO:                  return "MMIO";
    case MEM_MMIO_PORT_SPACE:       return "MMIOPort";
    case MEM_PAL_CODE:              return "PALCode";
    case MEM_PERSISTENT:            return "Persistent";
    default:                        return "Unknown";
    }
}

static void dump_uefi_memmap(const BOOT_INFO *bi)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_addr;

    printf("[uefi-mmap] count=%u desc_size=%lu map_bytes=%lu\n",
           bi->mmap_desc_count,
           (unsigned long)bi->mmap_desc_size,
           (unsigned long)bi->mmap_size);

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)p;

        uint64_t bytes = 0;
        uint64_t end = d->physical_start;

        if (d->number_of_pages <= UINT64_MAX / UINT64_C(0x1000)) {
            bytes = d->number_of_pages * UINT64_C(0x1000);
            if (d->physical_start <= UINT64_MAX - bytes)
                end = d->physical_start + bytes;
        }

        printf("[uefi-mmap] %03u type=%2u %-19s "
               "phys=%p..%p virt=%p pages=%lu bytes=%lu "
               "attr=0x%016llx\n",
               i,
               d->type,
               mem_type_name(d->type),
               (void *)(uintptr_t)d->physical_start,
               (void *)(uintptr_t)end,
               (void *)(uintptr_t)d->virtual_start,
               (unsigned long)d->number_of_pages,
               (unsigned long)bytes,
               (unsigned long long)d->attribute);
    }
}

void kernel_main(BOOT_INFO *bi)
{
    if (bi == 0 || bi->magic != BOOTINFO_MAGIC) {
        printf("[kernel] bad bootinfo\n");
        halt();
    }

    /*
     * 这是 ExitBootServices 后内核收到的最终 UEFI memory map。
     * 必须在 paging_early_takeover()/boot_alloc_pages() 修改 descriptor 前打印。
     */
    dump_uefi_memmap(bi);

    /*
     * BOOT_INFO 基本校验后立即接管 CR3。
     * framebuffer/MMIO 尚未建立专用映射，接管后不能直接访问 fb_base。
     */
    paging_info_t paging;
    int status = paging_early_takeover(bi, &paging);
    if (status != 0) {
        printf("[kernel] early paging takeover failed: %d\n", status);
        halt();
    }

    printf("[kernel] entry=%p image=%p+%lu\n",
           (void *)(uintptr_t)bi->kernel_entry,
           (void *)(uintptr_t)bi->kernel_base,
           (unsigned long)bi->kernel_size);
    printf("[kernel] mmap=%u usable=%lu MiB\n",
           bi->mmap_desc_count,
           (unsigned long)(usable_pages(bi) / 256));
    printf("[kernel] paging cr3=%p direct=%lu MiB "
           "tables=%lu pages/%lu KiB 1G=%lu 2M=%lu 4K=%lu\n",
           (void *)(uintptr_t)paging.root_phys,
           (unsigned long)(paging.direct_span >> 20),
           (unsigned long)paging.table_pages,
           (unsigned long)(paging.table_pages * 4),
           (unsigned long)paging.leaf_1g,
           (unsigned long)paging.leaf_2m,
           (unsigned long)paging.leaf_4k);

    status = pmm_init(bi);
    if (status != 0) {
        printf("[kernel] pmm init failed: %d\n", status);
        halt();
    }

    static vm_space_t kernel_vm;
    if (!vm_space_init(&kernel_vm, pmm_boot_cpu(),
                       VM_KERNEL_BASE, VM_KERNEL_SIZE)) {
        printf("[kernel] vm init failed\n");
        halt();
    }

    /*
     * direct-map VA window 由 VM 统一登记。窗口中的物理 hole 仍然没有 PTE，
     * 但这些 VA 不再允许其它用途分配。
     */
    if (!vm_reserve(&kernel_vm,
                    VM_DIRECT_MAP_BASE,
                    paging.direct_span,
                    VM_REGION_DIRECT_MAP,
                    VM_ATTR_READ | VM_ATTR_WRITE |
                    VM_ATTR_PINNED | VM_ATTR_CACHE_WB)) {
        printf("[kernel] direct-map VM reserve failed\n");
        halt();
    }

    /*
     * PML4[511] 是 paging 的 recursive mapping window，不能交给普通
     * kernel VA allocator。
     */
    if (!vm_reserve(&kernel_vm,
                    PAGING_RECURSIVE_BASE,
                    PAGING_RECURSIVE_SIZE,
                    VM_REGION_RESERVED,
                    VM_ATTR_PINNED)) {
        printf("[kernel] recursive paging VM reserve failed\n");
        halt();
    }

    /*
     * 先只验证 demand paging 主链，避免 DMA/MMIO alloc/free 和显式 PMM
     * 自测干扰定位。
     */
    page_fault_bind_space(&kernel_vm);
    idt_init();

    vaddr_t lazy_va;
    if (!vm_alloc(&kernel_vm, VM_PAGE_SIZE, VM_PAGE_SIZE,
                  VM_REGION_HEAP,
                  VM_ATTR_READ | VM_ATTR_WRITE |
                  VM_ATTR_LAZY | VM_ATTR_CACHE_WB,
                  &lazy_va)) {
        printf("[kernel] lazy VM alloc failed\n");
        halt();
    }

    printf("[kernel] lazy va=%p touch\n",
           (void *)(uintptr_t)lazy_va);

    volatile uint64_t *lazy =
        (volatile uint64_t *)(uintptr_t)lazy_va;
    *lazy = UINT64_C(0x1122334455667788);

    printf("[kernel] page fault mapped va=%p value=0x%llx\n",
           (void *)(uintptr_t)lazy_va,
           (unsigned long long)*lazy);

    halt();
    }

    vm_region_info_t info;
    if (!vm_query(&kernel_vm, va2m + VM_PAGE_SIZE, &info)) {
        printf("[kernel] vm query failed\n");
        halt();
    }

    printf("[kernel] VA DMA=%p MMIO=%p type=%u attrs=%p free=%lu TiB\n",
           (void *)(uintptr_t)va4k,
           (void *)(uintptr_t)va2m,
           (unsigned)info.type,
           (void *)(uintptr_t)info.attrs,
           (unsigned long)(kernel_vm.free_bytes >> 40));

    if (!vm_free(&kernel_vm, va4k) ||
        !vm_free(&kernel_vm, va2m)) {
        printf("[kernel] vm free failed\n");
        halt();
    }

    pmm_frame_t pte;
    pmm_frame_t pde;

    if (!pmm_alloc4k(pmm_boot_cpu(), &pte)) {
        printf("[kernel] PTE pool empty\n");
        halt();
    }

    if (!pmm_alloc2m(&pde)) {
        printf("[kernel] PDE pool empty\n");
        halt();
    }

    printf("[kernel] PTE frame=%p PDE frame=%p\n",
           (void *)(uintptr_t)pte,
           (void *)(uintptr_t)pde);

    pmm_free4k(pmm_boot_cpu(), pte);
    pmm_free2m(pde);


    }

    volatile uint64_t *lazy =
        (volatile uint64_t *)(uintptr_t)lazy_va;
    *lazy = UINT64_C(0x1122334455667788);

    printf("[kernel] page fault mapped va=%p value=0x%llx\n",
           (void *)(uintptr_t)lazy_va,
           (unsigned long long)*lazy);

    halt();
}
