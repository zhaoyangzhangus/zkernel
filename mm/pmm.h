#ifndef __KERNEL_MM_PMM_H__
#define __KERNEL_MM_PMM_H__

#include <stdbool.h>
#include <stdint.h>

#include "../boot/bootinfo.h"

#define PMM_PAGE_4K       0x1000ULL
#define PMM_PAGE_2M       0x200000ULL
#define PMM_POOL2_SLOTS   (64U * 64U)
#define PMM_POOL3_SLOTS   (64U * 64U * 64U)
#define PMM_REFILL_BATCH  64U

typedef uint64_t pmm_frame_t;

/*
 * 每逻辑 CPU 的 PTE pool：
 * root -> leaf -> slot，64 x 64 = 4096 个 4K frame。
 *
 * bit=1: 对应 slot/subtree 还有可分配 frame
 * bit=0: 对应 slot 空 / subtree 已耗尽
 */
typedef struct {
    uint64_t root;
    uint64_t leaf[64];
    pmm_frame_t slot[PMM_POOL2_SLOTS];
} pmm_cpu_t;

int pmm_init(const BOOT_INFO *bi);
void pmm_cpu_init(pmm_cpu_t *cpu);
pmm_cpu_t *pmm_boot_cpu(void);

bool pmm_alloc4k(pmm_cpu_t *cpu, pmm_frame_t *out_frame);
bool pmm_free4k(pmm_cpu_t *cpu, pmm_frame_t frame);

bool pmm_alloc2m(pmm_frame_t *out_frame);
bool pmm_free2m(pmm_frame_t frame);

#endif
