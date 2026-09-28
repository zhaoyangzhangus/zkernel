
/*
 * init/main.c —— 内核入口
 *
 * boot/boot.c 在 ExitBootServices 之后直接跳到 KERNEL_LOAD_ADDR，
 * 也就是这里的 kernel_main（链接脚本把它放在最前面，见 kernel.lds）。
 *
 * 进入时的状态：
 *   - 已处于 64 位长模式；UEFI 的恒等映射页表仍然有效，
 *     所以只能访问恒等映射的物理地址
 *   - 中断是关的（引导程序执行了 cli），GDT/IDT 还是固件留下的
 *   - 栈还是固件的栈，之后要自己换成内核栈
 *   - 所有 Boot Services 已失效，Runtime Services 仍可用
 *   - 除了传参用的 rdi，CPU 寄存器都是未定义的，不要依赖它们
 *   - .data 已由引导程序从 ELF 文件拷到位，.bss 已按 p_memsz 清零
 *     （内核不再需要自己清 .bss；下面有专门的自检来验证这两件事）
 *
 * 目前只做最小验证：初始化串口，用 printf 打印启动信息，然后停机。
 */
#include <stdint.h>

#include "../boot/bootinfo.h"
#include "../drivers/serial.h"
#include "../graphics/text.h"
#include "../lib/printf.h"
#include "../mm/pmm.h"

/* ------------------------------------------------------------------ */
/* ELF 装载自检                                                        */
/* ------------------------------------------------------------------ */
/*
 * 这两个变量专门用来验证引导程序有没有按 ELF 的规则装载映像：
 *   data_probe 有初值 -> 落在 .data，必须由引导程序从文件里拷进来
 *   bss_probe  无初值 -> 落在 .bss，必须由引导程序按 p_memsz 清零
 * 加 volatile 是防止 -O2 把读取折叠成常量，让自检失去意义。
 */
#define DATA_PROBE_INIT 0xC0FFEEu

static volatile uint32_t data_probe = DATA_PROBE_INIT;
static volatile uint32_t bss_probe;

/* .bss 的边界由链接脚本给出 */
extern char __bss_start[];
extern char __bss_end[];

static void verify_elf_load(void)
{
    const volatile char *p;
    unsigned long bss_bytes = 0;
    int bss_all_zero = 1;

    for (p = __bss_start; p < __bss_end; p++) {
        bss_bytes++;
        if (*p != 0) {
            bss_all_zero = 0;
            break;
        }
    }

    printf("[kernel] ELF load check: .data probe = %#x (%s), "
           ".bss = %lu bytes (%s)\n",
           data_probe, (data_probe == DATA_PROBE_INIT) ? "ok" : "WRONG",
           bss_bytes,
           (bss_all_zero && bss_probe == 0) ? "all zero" : "NOT ZERO");
}

/* ------------------------------------------------------------------ */
/* 内存映射                                                            */
/* ------------------------------------------------------------------ */
static const char *const mem_type_name[] = {
    "reserved",  "loader-code", "loader-data", "bs-code",
    "bs-data",   "rt-code",     "rt-data",     "free",
    "unusable",  "acpi-reclaim", "acpi-nvs",   "mmio",
    "mmio-port", "pal",         "persistent",
};

static const char *mem_type_str(uint32_t type)
{
    if (type < sizeof(mem_type_name) / sizeof(mem_type_name[0]))
        return mem_type_name[type];
    return "unknown";
}

static void dump_memory_map(const BOOT_INFO *bi)
{
    uint64_t usable_pages = 0;
    uint64_t total_pages  = 0;
    uint64_t per_type[16] = { 0 };
    uint32_t i, t;

    if (bi->mmap_addr == 0 || bi->mmap_desc_size == 0 || bi->mmap_desc_count == 0) {
        printf("[kernel] no memory map available\n");
        return;
    }

    for (i = 0; i < bi->mmap_desc_count; i++) {
        /* 描述符必须按 mmap_desc_size 步进，不能当结构体数组直接索引 */
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)((const uint8_t *)(uintptr_t)bi->mmap_addr +
                                             (uint64_t)i * bi->mmap_desc_size);

        total_pages += d->number_of_pages;
        if (d->type < sizeof(per_type) / sizeof(per_type[0]))
            per_type[d->type] += d->number_of_pages;

        switch (d->type) {
        case MEM_CONVENTIONAL:
        case MEM_LOADER_CODE:
        case MEM_LOADER_DATA:
        case MEM_BOOT_SERVICES_CODE:
        case MEM_BOOT_SERVICES_DATA:
            usable_pages += d->number_of_pages;
            break;
        default:
            break;
        }
    }

    printf("[kernel] memory map: %u descriptors, %lu of %lu MiB usable\n",
           bi->mmap_desc_count, (unsigned long)(usable_pages / 256),   /* 页 -> MiB */
           (unsigned long)(total_pages / 256));

    printf("         %-14s %8s %8s\n", "type", "pages", "MiB");
    for (t = 0; t < sizeof(per_type) / sizeof(per_type[0]); t++) {
        if (per_type[t] == 0)
            continue;
        printf("         %-14s %8lu %8lu\n", mem_type_str(t),
               (unsigned long)per_type[t], (unsigned long)(per_type[t] / 256));
    }
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */
void kernel_main(BOOT_INFO *bi);

void kernel_main(BOOT_INFO *bi)
{
    serial_init();
    verify_elf_load();

    printf("\n");
    printf("====================================================\n");
    printf("[kernel] kernel_main() entered, running on UEFI's stack\n");

    if (bi == 0 || bi->magic != BOOTINFO_MAGIC) {
        printf("[kernel] ERROR: bad BOOT_INFO, magic=%lx (want %lx)\n",
               bi ? (unsigned long)bi->magic : 0UL, (unsigned long)BOOTINFO_MAGIC);
        for (;;)
            __asm__ volatile("hlt");
    }

    printf("[kernel] bootinfo v%u, image at %p (%lu bytes), entry %p\n",
           bi->version, (void *)(uintptr_t)bi->kernel_base,
           (unsigned long)bi->kernel_size, (void *)(uintptr_t)bi->kernel_entry);

    if (bi->fb_base != 0)
        printf("[kernel] framebuffer: %ux%u, pitch=%u, format=%u, base=%p\n",
               bi->fb_width, bi->fb_height, bi->fb_pixels_per_scanline,
               bi->fb_pixel_format, (void *)(uintptr_t)bi->fb_base);
    else
        printf("[kernel] framebuffer: none\n");

    if (fb_text_supported(bi)) {
        fb_draw_text_a8(bi, 16, 16,
                        "zkernel 12x24 A8\n"
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ\n"
                        "abcdefghijklmnopqrstuvwxyz 0123456789",
                        0x00F2F5F7U, 0x00081018U);
        printf("[kernel] framebuffer A8 font: rendered 12x24 demo\n");
    } else {
        printf("[kernel] framebuffer A8 font: unsupported GOP mode\n");
    }

    dump_memory_map(bi);

    /*
     * 第一版物理页分配器直接把 UEFI memory map 当作 free-range 元数据。
     * 这里只从 MEM_CONVENTIONAL 取页，因此不会碰到仍承载 bootinfo、
     * memory map 缓冲区和内核映像的 LoaderData/LoaderCode。
     */
    if (pmm_init(bi) != 0) {
        printf("[kernel] ERROR: pmm_init failed\n");
        for (;;)
            __asm__ volatile("hlt");
    }

    {
        uint64_t one_byte = pmm_alloc(1);
        uint64_t seven_pages = pmm_alloc(6 * PMM_PAGE_SIZE + 1);

        if (one_byte == PMM_ALLOC_FAILED ||
            seven_pages == PMM_ALLOC_FAILED) {
            printf("[kernel] ERROR: physical memory allocation failed\n");
            for (;;)
                __asm__ volatile("hlt");
        }

        printf("[kernel] pmm: 1 byte -> 1 page at %p, "
               "24577 bytes -> 7 contiguous pages at %p\n",
               (void *)(uintptr_t)one_byte,
               (void *)(uintptr_t)seven_pages);
    }

    /* printf 自检：对照下面这行的实际输出，可以快速确认格式化是否正确 */
    printf("[kernel] printf: %d %u %#x %c [%5s][%-5s] %p %.3s\n",
           -42, 42u, 0xbeefu, '!', "right", "left", (void *)bi, "abcdef");

    printf("[kernel] Boot Services are gone, we own the machine now\n");
    printf("====================================================\n");

    for (;;)
        __asm__ volatile("hlt");
}
