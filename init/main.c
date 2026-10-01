#include <stdint.h>

#include "../boot/bootinfo.h"
#include "../lib/printf.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/pat.h"
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

static const char *vm_type_name(vm_region_type_t type)
{
    switch (type) {
    case VM_REGION_GENERIC:     return "GENERIC";
    case VM_REGION_KERNEL:      return "KERNEL";
    case VM_REGION_HEAP:        return "HEAP";
    case VM_REGION_STACK:       return "STACK";
    case VM_REGION_DMA:         return "DMA";
    case VM_REGION_MMIO:        return "MMIO";
    case VM_REGION_FRAMEBUFFER: return "FRAMEBUFFER";
    case VM_REGION_ACPI:        return "ACPI";
    case VM_REGION_DIRECT_MAP:  return "DIRECT_MAP";
    case VM_REGION_RESERVED:    return "RESERVED";
    case VM_REGION_USER:        return "USER";
    default:                    return "UNKNOWN";
    }
}

static bool dump_vm_region(const vm_region_info_t *region, void *context)
{
    (void)context;

    uint64_t last = region->start + region->size - 1;

    printf("[vm] %-11s %p..%p size=%lu KiB attrs=0x%llx\n",
           vm_type_name(region->type),
           (void *)(uintptr_t)region->start,
           (void *)(uintptr_t)last,
           (unsigned long)(region->size >> 10),
           (unsigned long long)region->attrs);
    return true;
}

static void dump_vm_layout(const vm_space_t *space)
{
    printf("[vm] used=%lu KiB free=%lu MiB regions=%lu\n",
           (unsigned long)(space->used_bytes >> 10),
           (unsigned long)(space->free_bytes >> 20),
           (unsigned long)space->region_count);

    if (!vm_for_each_region(space, dump_vm_region, NULL))
        printf("[vm] layout walk failed\n");
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

    if (!pat_init_cpu()) {
        printf("[kernel] PAT init failed\n");
        halt();
    }

    printf("[kernel] PAT=0x%016llx\n",
           (unsigned long long)pat_read());

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
     * 只登记当前页表中真正存在的 direct-map extent。
     * 没有物理 RAM / 没有 PTE 的 hole 保持为普通可分配 VA。
     */
    if (!paging_register_direct_map(&kernel_vm, paging.direct_span)) {
        printf("[kernel] direct-map VM register failed\n");
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
     * GOP framebuffer 不属于普通 RAM direct map。
     * 给它单独分配 kernel VA，并以 WC 4K PTE 显式映射。
     * 保留 fb_phys_base；fb_base 从这里开始表示可直接解引用的 kernel VA。
     */
    if (bi->fb_phys_base != 0 && bi->fb_size != 0) {
        uint64_t fb_page = bi->fb_phys_base & ~(VM_PAGE_SIZE - 1);
        uint64_t fb_offset = bi->fb_phys_base - fb_page;

        if (bi->fb_size > UINT64_MAX - fb_offset) {
            printf("[kernel] framebuffer range overflow\n");
            halt();
        }

        uint64_t fb_map_size = fb_offset + bi->fb_size;
        if (fb_map_size > UINT64_MAX - (VM_PAGE_SIZE - 1)) {
            printf("[kernel] framebuffer map size overflow\n");
            halt();
        }
        fb_map_size =
            (fb_map_size + VM_PAGE_SIZE - 1) & ~(VM_PAGE_SIZE - 1);

        vaddr_t fb_va;
        uint64_t fb_attrs =
            VM_ATTR_READ | VM_ATTR_WRITE |
            VM_ATTR_PINNED | VM_ATTR_CACHE_WC;

        if (!vm_alloc(&kernel_vm, fb_map_size, VM_PAGE_SIZE,
                      VM_REGION_FRAMEBUFFER, fb_attrs, &fb_va)) {
            printf("[kernel] framebuffer VA alloc failed\n");
            halt();
        }

        if (!paging_map_range_current(pmm_boot_cpu(), fb_va,
                                      fb_page, fb_map_size,
                                      fb_attrs)) {
            printf("[kernel] framebuffer map failed\n");
            halt();
        }

        bi->fb_base = fb_va + fb_offset;

        printf("[kernel] framebuffer pa=%p va=%p size=%lu KiB\n",
               (void *)(uintptr_t)bi->fb_phys_base,
               (void *)(uintptr_t)bi->fb_base,
               (unsigned long)(bi->fb_size >> 10));
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

    /*
     * 把 VMM 当前已占用的高半区虚拟地址完整打印出来，方便直接检查
     * direct-map / recursive / framebuffer / lazy region 的实际布局。
     */
    dump_vm_layout(&kernel_vm);

    volatile uint64_t *lazy =
        (volatile uint64_t *)(uintptr_t)lazy_va;
    *lazy = UINT64_C(0x1122334455667788);

    printf("[kernel] page fault mapped va=%p value=0x%llx\n",
           (void *)(uintptr_t)lazy_va,
           (unsigned long long)*lazy);

    halt();
}
