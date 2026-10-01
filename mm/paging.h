#ifndef __KERNEL_MM_PAGING_H__
#define __KERNEL_MM_PAGING_H__

#include <stdbool.h>
#include <stdint.h>

#include "../boot/bootinfo.h"
#include "vm.h"

#define PAGING_RECURSIVE_BASE UINT64_C(0xFFFFFF8000000000)
#define PAGING_RECURSIVE_SIZE UINT64_C(0x0000008000000000) /* 512 GiB, PML4[511] */

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
 * 页表建立过程中每缺一个 4K table page 就直接调用
 * boot_alloc_pages(bi, 1, ...)。boot allocator 从最高 Conventional
 * 物理地址向下分配；这些页永久归 paging 使用，不再回收到 PMM。
 * 在 PMM 初始化之前建立并切换到自己的 4-level page tables。
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

/*
 * 在当前 CR3 上安装一个 4K 映射。缺失的中间页表从 PMM 4K pool
 * 动态分配，并通过 PML4[511] recursive mapping 修改。
 *
 * 当前生成 PRESENT/WRITE/USER，并按 VM_ATTR_CACHE_* 编码 PAT。
 * NX 后续补。
 */
bool paging_map_4k_current(pmm_cpu_t *cpu, vaddr_t va,
                           pmm_frame_t frame, uint64_t attrs);

/*
 * 映射一段已经存在的物理地址。va/pa 必须 4K 对齐，size 会向上取整。
 * 当前按 4K PTE 建图；适合 framebuffer/MMIO 等设备物理地址。
 */
bool paging_map_range_current(pmm_cpu_t *cpu, vaddr_t va,
                              uint64_t pa, uint64_t size,
                              uint64_t attrs);

/*
 * 解除当前 CR3 中一个 4K leaf 映射。
 * 中间项或 PTE 不存在时返回 true 且 *out_mapped=false；
 * 遇到 1G/2M large leaf 返回 false。当前不回收空 PT/PD/PDPT。
 */
bool paging_unmap_4k_current(vaddr_t va,
                             pmm_frame_t *out_frame,
                             bool *out_mapped);

/*
 * 扫描当前页表中的 direct-map 窗口，只把真正 PRESENT 的连续 VA
 * extent 登记到 kernel VM。物理 hole 不占用 VMM 地址空间。
 */
bool paging_register_direct_map(vm_space_t *space, uint64_t direct_span);

#endif
