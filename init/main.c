#include <stdint.h>

#include "../boot/bootinfo.h"
#include "../drivers/serial.h"
#include "../graphics/text.h"
#include "../lib/printf.h"
#include "../mm/pmm.h"

static void halt(void)
{
    for (;;)
        __asm__ volatile("hlt");
}

static uint64_t conventional_pages(const BOOT_INFO *bi)
{
    uint64_t pages = 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_addr;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)p;
        if (d->type == MEM_CONVENTIONAL)
            pages += d->number_of_pages;
    }
    return pages;
}

void kernel_main(BOOT_INFO *bi)
{
    serial_init();

    if (bi == 0 || bi->magic != BOOTINFO_MAGIC) {
        printf("[kernel] bad bootinfo\n");
        halt();
    }

    printf("[kernel] entry=%p image=%p+%lu\n",
           (void *)(uintptr_t)bi->kernel_entry,
           (void *)(uintptr_t)bi->kernel_base,
           (unsigned long)bi->kernel_size);
    printf("[kernel] mmap=%u conventional=%lu MiB\n",
           bi->mmap_desc_count,
           (unsigned long)(conventional_pages(bi) / 256));

    if (fb_text_supported(bi)) {
        fb_draw_text_a8(bi, 16, 16,
                        "zkernel 12x24 A8\n"
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ\n"
                        "abcdefghijklmnopqrstuvwxyz 0123456789",
                        0x00F2F5F7U, 0x00081018U);
    }

    if (pmm_init(bi) != 0) {
        printf("[kernel] pmm init failed\n");
        halt();
    }

    uint64_t p = pmm_alloc(24577);
    if (p == PMM_ALLOC_FAILED) {
        printf("[kernel] pmm alloc failed\n");
        halt();
    }

    printf("[kernel] pmm 24577 bytes -> %p\n", (void *)(uintptr_t)p);
    halt();
}
