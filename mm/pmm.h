#ifndef __KERNEL_MM_PMM_H__
#define __KERNEL_MM_PMM_H__

#include <stdbool.h>
#include <stdint.h>

#include "../boot/bootinfo.h"

#define PMM_PAGE_4K      0x1000ULL
#define PMM_PAGE_2M      0x200000ULL
#define PMM_POOL3_MAX    (64U * 64U * 64U)
#define PMM_CPU_PTE_MAX  (64U * 64U)
#define PMM_BATCH        64U

typedef uint64_t pmm_frame_t;

/*
 * 每 CPU PTE cache 仍是两层 64 叉，但容量按实际 PTE 数动态决定，
 * 最大 4096 slots。leaf/slot 存储来自 PMM bootstrap metadata。
 */
typedef struct {
    uint64_t root;
    uint32_t capacity;
    uint32_t leaf_count;
    uint64_t *leaf;
    pmm_frame_t *slot;
} pmm_cpu_t;

int pmm_init(BOOT_INFO *bi);
pmm_cpu_t *pmm_boot_cpu(void);

/* pmm_init() 从 Conventional 中切出的 bootstrap metadata 物理范围。 */
uint64_t pmm_metadata_phys(void);
uint64_t pmm_metadata_size(void);

bool pmm_alloc4k(pmm_cpu_t *cpu, pmm_frame_t *out_frame);
bool pmm_free4k(pmm_cpu_t *cpu, pmm_frame_t frame);

bool pmm_alloc2m(pmm_frame_t *out_frame);
bool pmm_free2m(pmm_frame_t frame);

#endif
