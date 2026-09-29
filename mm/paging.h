#ifndef __KERNEL_MM_PAGING_H__
#define __KERNEL_MM_PAGING_H__

#include <stdint.h>

#include "../boot/bootinfo.h"
#include "pmm.h"
#include "vm.h"

typedef struct {
    pmm_frame_t root_phys;
    uint64_t direct_span;
    uint64_t leaf_1g;
    uint64_t leaf_2m;
    uint64_t leaf_4k;
    uint32_t has_1g_pages;
} paging_info_t;

/*
 * 建立并切换到内核自己的 4-level page tables。
 *
 * 第一阶段同时保留：
 *   1. bootstrap identity map：让当前低地址 kernel/UEFI stack/PMM metadata
 *      在 mov cr3 后继续工作；
 *   2. kernel direct map：VA = VM_DIRECT_MAP_BASE + PA。
 *
 * direct map 只映射 Loader/BootServices/Conventional RAM 和 PMM bootstrap
 * metadata，不把 MMIO/framebuffer 混进普通 WB RAM direct map。
 */
int paging_takeover(BOOT_INFO *bi, vm_space_t *space,
                    paging_info_t *out_info);

#endif
