#ifndef __KERNEL_MM_PMM_H__
#define __KERNEL_MM_PMM_H__

#include <stdint.h>
#include "../boot/bootinfo.h"

#define PMM_PAGE_SIZE    4096ULL
#define PMM_ALLOC_FAILED UINT64_MAX

int pmm_init(BOOT_INFO *boot_info);
uint64_t pmm_alloc(uint64_t size);

#endif
