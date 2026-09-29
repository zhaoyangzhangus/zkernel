#ifndef __KERNEL_MM_PAGING_H__
#define __KERNEL_MM_PAGING_H__

#include <stdint.h>

#include "../boot/bootinfo.h"
#include "vm.h"

typedef struct {
    uint64_t root_phys;
    uint64_t direct_span;
    uint64_t table_pages;
    uint64_t leaf_1g;
    uint64_t leaf_2m;
    uint64_t leaf_4k;
    uint32_t has_1g_pages;
} paging_info_t;

/*
 * kernel_main() 的第一阶段。
 *
 * 直接从一个 Conventional descriptor 的高端切出页表页，在 PMM 初始化
 * 之前建立并切换到自己的 4-level page tables。
 *
 * 所有最终可归内核使用的 RAM：
 *   LoaderCode/Data
 *   BootServicesCode/Data
 *   Conventional
 *
 * 都同时建立：
 *   bootstrap identity: VA = PA
 *   kernel direct map:  VA = VM_DIRECT_MAP_BASE + PA
 *
 * 每段优先使用 1G -> 2M -> 4K。
 */
int paging_early_takeover(BOOT_INFO *bi, paging_info_t *out_info);

#endif
