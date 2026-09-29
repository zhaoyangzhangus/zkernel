#include <stdint.h>

#include "../boot/bootinfo.h"
#include "../lib/printf.h"
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

void kernel_main(BOOT_INFO *bi)
{
    if (bi == 0 || bi->magic != BOOTINFO_MAGIC) {
        printf("[kernel] bad bootinfo\n");
        halt();
    }

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
    printf("[kernel] paging cr3=%p direct=%lu MiB tables=%lu "
           "1G=%lu 2M=%lu 4K=%lu\n",
           (void *)(uintptr_t)paging.root_phys,
           (unsigned long)(paging.direct_span >> 20),
           (unsigned long)paging.table_pages,
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

    vaddr_t va4k;
    vaddr_t va2m;

    if (!vm_alloc(&kernel_vm, 3 * VM_PAGE_SIZE, VM_PAGE_SIZE,
                  VM_REGION_DMA,
                  VM_ATTR_READ | VM_ATTR_WRITE | VM_ATTR_PINNED |
                  VM_ATTR_CACHE_WB,
                  &va4k) ||
        !vm_alloc(&kernel_vm, 2 * 1024 * 1024ULL,
                  2 * 1024 * 1024ULL,
                  VM_REGION_MMIO,
                  VM_ATTR_READ | VM_ATTR_WRITE | VM_ATTR_CACHE_UC,
                  &va2m)) {
        printf("[kernel] vm alloc failed\n");
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

    halt();
}
