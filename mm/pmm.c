/*
 * pmm.c —— 直接复用 UEFI memory map 的最小物理内存分配器
 *
 * 约束：
 *   - 从 ExitBootServices 后可回收的 MEM_CONVENTIONAL /
 *     MEM_BOOT_SERVICES_CODE / MEM_BOOT_SERVICES_DATA 分配；
 *   - 调用方只传字节数 size；
 *   - size 自动向上对齐到 4 KiB，返回物理连续区域；
 *   - 不支持 free；
 *   - 不建立任何按页元数据。
 *
 * 分配器会原地修改 memory map。分配之后，MEM_CONVENTIONAL 描述符表示的
 * 是“尚未分配”的剩余范围，而不是固件最初返回的历史快照。
 */
#include "pmm.h"

#include <stddef.h>
#include <stdint.h>

static BOOT_INFO *g_boot_info;

static int pmm_type_usable(uint32_t type)
{
    /*
     * ExitBootServices 成功后，BootServicesCode/Data 已不再被固件使用，
     * 可以直接交给内核。LoaderCode/Data 暂时不能加入：kernel.elf、
     * BOOT_INFO 和最终 memory-map buffer 本身都可能还位于 LoaderData。
     */
    return type == MEM_CONVENTIONAL ||
           type == MEM_BOOT_SERVICES_CODE ||
           type == MEM_BOOT_SERVICES_DATA;
}

int pmm_init(BOOT_INFO *boot_info)
{
    if (boot_info == NULL ||
        boot_info->mmap_addr == 0 ||
        boot_info->mmap_desc_size < sizeof(BOOT_MEMORY_DESCRIPTOR) ||
        boot_info->mmap_size < boot_info->mmap_desc_size ||
        boot_info->mmap_desc_count == 0 ||
        (uint64_t)boot_info->mmap_desc_count >
            boot_info->mmap_size / boot_info->mmap_desc_size) {
        return -1;
    }

    g_boot_info = boot_info;
    return 0;
}

uint64_t pmm_alloc(uint64_t size)
{
    uint64_t offset;
    uint64_t bytes;
    uint64_t pages;

    if (g_boot_info == NULL || size == 0)
        return 0;

    /* size + 4095 不能溢出。 */
    if (size > UINT64_MAX - (PMM_PAGE_SIZE - 1))
        return 0;

    bytes = (size + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
    pages = bytes / PMM_PAGE_SIZE;

    /*
     * 不保存 current-index 游标：每次直接扫描 UEFI descriptors。
     * 这样分配状态只存在 descriptor 自身的 physical_start /
     * number_of_pages 中。早期启动阶段 descriptor 数量很少，O(n) 足够简单。
     */
    for (offset = 0;
         offset <= g_boot_info->mmap_size - g_boot_info->mmap_desc_size;
         offset += g_boot_info->mmap_desc_size) {
        BOOT_MEMORY_DESCRIPTOR *d =
            (BOOT_MEMORY_DESCRIPTOR *)((uint8_t *)(uintptr_t)g_boot_info->mmap_addr +
                                       offset);
        uint64_t base;

        if (!pmm_type_usable(d->type) ||
            d->number_of_pages < pages) {
            continue;
        }

        base = d->physical_start;

        /* UEFI 页必须 4 KiB 对齐；同时防止损坏的 map 导致地址溢出。 */
        if ((base & (PMM_PAGE_SIZE - 1)) != 0 ||
            base > UINT64_MAX - bytes) {
            continue;
        }

        /*
         * 从该 free range 的低地址端切走对齐后的 bytes。
         * descriptor 原地收缩，因此不需要额外 bitmap/free-list。
         */
        d->physical_start = base + bytes;
        if (d->virtual_start != 0)
            d->virtual_start += bytes;
        d->number_of_pages -= pages;

        return base;
    }

    return 0;
}
