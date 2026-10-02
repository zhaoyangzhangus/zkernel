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

static paging_info_t boot_paging;

__attribute__((noreturn))
void kernel_high_continue(BOOT_INFO *bi);

static uint64_t current_rsp(void)
{
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

static bool boot_direct_mapped_address(const BOOT_INFO *bi, uint64_t pa)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_addr;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)p;

        if (d->type != MEM_LOADER_CODE &&
            d->type != MEM_LOADER_DATA &&
            d->type != MEM_BOOT_SERVICES_CODE &&
            d->type != MEM_BOOT_SERVICES_DATA &&
            d->type != MEM_CONVENTIONAL)
            continue;

        if (d->number_of_pages == 0 ||
            d->number_of_pages > UINT64_MAX / VM_PAGE_SIZE)
            continue;

        uint64_t bytes = d->number_of_pages * VM_PAGE_SIZE;
        if (d->physical_start > UINT64_MAX - bytes)
            continue;

        if (pa >= d->physical_start &&
            pa < d->physical_start + bytes)
            return true;
    }

    return false;
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
    case VM_REGION_RECURSIVE:   return "RECURSIVE";
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
    /* debugcon: 已经真正执行到高半区 kernel_main。 */
    __asm__ volatile("outb %0, $0xe9" :: "a"((uint8_t)'K'));

    if (bi == 0 ||
        bi->magic != BOOTINFO_MAGIC ||
        bi->version != BOOTINFO_VERSION) {
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
    int status = paging_early_takeover(bi, &boot_paging);
    if (status != 0) {
        printf("[kernel] early paging takeover failed: %d\n", status);
        halt();
    }

    /*
     * final CR3 暂时同时存在 identity/direct alias。
     * BOOT_INFO、mmap 和当前 loader stack 必须都在 direct map 覆盖的 RAM 中。
     */
    uint64_t bi_phys = (uint64_t)(uintptr_t)bi;
    uint64_t mmap_phys = bi->mmap_addr;
    uint64_t rsp_phys = current_rsp();

    if (!boot_direct_mapped_address(bi, bi_phys) ||
        !boot_direct_mapped_address(bi, mmap_phys) ||
        !boot_direct_mapped_address(bi, rsp_phys)) {
        printf("[kernel] boot pointers are outside direct map\n");
        halt();
    }

    BOOT_INFO *high_bi =
        (BOOT_INFO *)(uintptr_t)(VM_DIRECT_MAP_BASE + bi_phys);

    /* BOOT_INFO/mmap 不复制，只切到同一物理页的 direct-map alias。 */
    high_bi->mmap_addr = VM_DIRECT_MAP_BASE + mmap_phys;

    if (high_bi->magic != BOOTINFO_MAGIC ||
        high_bi->version != BOOTINFO_VERSION) {
        printf("[kernel] direct-map bootinfo validation failed\n");
        halt();
    }

    /*
     * 当前 kernel_main 的 C frame 仍建立在低地址 stack alias 上。
     * 直接在这里把 RSP 改成同一物理栈的 direct-map alias，然后丢弃
     * 旧 frame chain。之后绝不能再返回本函数，因此直接跳到新的
     * C continuation。
     *
     * jmp 前把 RSP 调整成 SysV 函数入口要求的 16n+8，continuation
     * 是 noreturn，不需要真实 return address。
     */
    __asm__ volatile(
        "addq %[base], %%rsp\n\t"
        "andq $-16, %%rsp\n\t"
        "subq $8, %%rsp\n\t"
        "xorl %%ebp, %%ebp\n\t"
        "jmp *%%rax"
        :
        : [base] "r"(VM_DIRECT_MAP_BASE),
          "D"(high_bi),
          "a"(kernel_high_continue)
        : "memory");

    __builtin_unreachable();
}

__attribute__((noreturn))
void kernel_high_continue(BOOT_INFO *bi)
{
    /*
     * 先把 GDTR/IDTR 切到 kernel 自己的高地址表。
     * 这样删除低半区后，即使随后发生异常也不会再依赖 firmware IDT/GDT。
     */
    idt_init();

    /*
     * RIP/RSP/BOOT_INFO/mmap/GDT/IDT 都已经在高半区。
     * 删除 PML4[0..255]，彻底去掉 final kernel CR3 的 identity map。
     */
    paging_drop_low_half_current();

    printf("[kernel] entry=%p image pa=%p va=%p size=%lu\n",
           (void *)(uintptr_t)bi->kernel_entry,
           (void *)(uintptr_t)bi->kernel_base,
           (void *)(uintptr_t)bi->kernel_virt_base,
           (unsigned long)bi->kernel_size);
    printf("[kernel] mmap=%u usable=%lu MiB\n",
           bi->mmap_desc_count,
           (unsigned long)(usable_pages(bi) / 256));
    printf("[kernel] paging cr3=%p direct=%lu MiB "
           "tables=%lu pages/%lu KiB 1G=%lu 2M=%lu 4K=%lu\n",
           (void *)(uintptr_t)boot_paging.root_phys,
           (unsigned long)(boot_paging.direct_span >> 20),
           (unsigned long)boot_paging.table_pages,
           (unsigned long)(boot_paging.table_pages * 4),
           (unsigned long)boot_paging.leaf_1g,
           (unsigned long)boot_paging.leaf_2m,
           (unsigned long)boot_paging.leaf_4k);

    int status;

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
    if (!paging_register_direct_map(&kernel_vm, boot_paging.direct_span)) {
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
                    VM_REGION_RECURSIVE,
                    VM_ATTR_PINNED)) {
        printf("[kernel] recursive paging VM reserve failed\n");
        halt();
    }

    /*
     * 内核本体已经由 paging_early_takeover() 映射到高半区；
     * VMM 也必须登记这段地址，避免后续 vm_alloc() 再次分配。
     */
    if (bi->kernel_size > UINT64_MAX - (VM_PAGE_SIZE - 1)) {
        printf("[kernel] kernel image size overflow\n");
        halt();
    }

    uint64_t kernel_vm_size =
        (bi->kernel_size + VM_PAGE_SIZE - 1) & ~(VM_PAGE_SIZE - 1);

    if (!vm_reserve(&kernel_vm,
                    bi->kernel_virt_base,
                    kernel_vm_size,
                    VM_REGION_KERNEL,
                    VM_ATTR_READ | VM_ATTR_WRITE | VM_ATTR_EXEC |
                    VM_ATTR_PINNED | VM_ATTR_CACHE_WB)) {
        printf("[kernel] kernel image VM reserve failed\n");
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

    vaddr_t demand_va;
    if (!vm_alloc(&kernel_vm, VM_PAGE_SIZE, VM_PAGE_SIZE,
                  VM_REGION_HEAP,
                  VM_ATTR_READ | VM_ATTR_WRITE |
                  VM_ATTR_PMM_OWNED | VM_ATTR_CACHE_WB,
                  &demand_va)) {
        printf("[kernel] demand VM alloc failed\n");
        halt();
    }

    printf("[kernel] demand va=%p touch\n",
           (void *)(uintptr_t)demand_va);

    /*
     * 把 VMM 当前已占用的高半区虚拟地址完整打印出来，方便直接检查
     * direct-map / recursive / framebuffer / demand region 的实际布局。
     */
    dump_vm_layout(&kernel_vm);

    volatile uint64_t *demand =
        (volatile uint64_t *)(uintptr_t)demand_va;
    *demand = UINT64_C(0x1122334455667788);

    printf("[kernel] page fault mapped va=%p value=0x%llx\n",
           (void *)(uintptr_t)demand_va,
           (unsigned long long)*demand);

    /*
     * 验证完整释放链：
     * PTE -> physical frame -> PMM -> free VA region。
     */
    if (!vm_free(&kernel_vm, demand_va)) {
        printf("[kernel] vm_free failed\n");
        halt();
    }

    printf("[kernel] vm_free reclaimed demand region\n");
    dump_vm_layout(&kernel_vm);

    halt();
}
