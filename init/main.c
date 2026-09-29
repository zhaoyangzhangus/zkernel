#include <stdint.h>

#include "../boot/bootinfo.h"
#include "../graphics/text.h"
#include "../lib/printf.h"
#include "../mm/pmm.h"

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

    printf("[kernel] entry=%p image=%p+%lu\n",
           (void *)(uintptr_t)bi->kernel_entry,
           (void *)(uintptr_t)bi->kernel_base,
           (unsigned long)bi->kernel_size);
    printf("[kernel] mmap=%u usable=%lu MiB\n",
           bi->mmap_desc_count,
           (unsigned long)(usable_pages(bi) / 256));

    if (fb_text_supported(bi)) {
        fb_draw_text_a8(bi, 16, 16,
                        "zkernel 12x24 A8\n"
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ\n"
                        "abcdefghijklmnopqrstuvwxyz 0123456789",
                        0x00F2F5F7U, 0x00081018U);
    }

    int status = pmm_init(bi);
    if (status != 0) {
        printf("[kernel] pmm init failed: %d\n", status);
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
