#ifndef __KERNEL_MM_VM_H__
#define __KERNEL_MM_VM_H__

#include <stdbool.h>
#include <stdint.h>

#include "pmm.h"

#define VM_PAGE_SIZE   0x1000ULL
#define VM_KERNEL_BASE     UINT64_C(0xFFFF800000000000)
#define VM_KERNEL_SIZE     UINT64_C(0x0000800000000000) /* full 128 TiB high half */
#define VM_DIRECT_MAP_BASE VM_KERNEL_BASE

typedef uint64_t vaddr_t;

typedef enum {
    VM_REGION_GENERIC = 0,
    VM_REGION_KERNEL,
    VM_REGION_HEAP,
    VM_REGION_STACK,
    VM_REGION_DMA,
    VM_REGION_MMIO,
    VM_REGION_FRAMEBUFFER,
    VM_REGION_ACPI,
    VM_REGION_DIRECT_MAP,
    VM_REGION_RECURSIVE,
    VM_REGION_RESERVED,
    VM_REGION_USER
} vm_region_type_t;

/*
 * 这些只是 VM metadata。当前 VM 不写页表；
 * 后续 mapping 层可根据这些属性生成 PTE/PAT 等硬件属性。
 */
#define VM_ATTR_READ      (UINT64_C(1) << 0)
#define VM_ATTR_WRITE     (UINT64_C(1) << 1)
#define VM_ATTR_EXEC      (UINT64_C(1) << 2)
#define VM_ATTR_USER      (UINT64_C(1) << 3)
#define VM_ATTR_GUARD     (UINT64_C(1) << 4)
#define VM_ATTR_PINNED    (UINT64_C(1) << 5)
#define VM_ATTR_LAZY      (UINT64_C(1) << 6)
/* VM 拥有该 region 的普通 RAM frame；vm_free() 会归还 PMM。 */
#define VM_ATTR_PMM_OWNED (UINT64_C(1) << 7)

#define VM_ATTR_CACHE_WB       (UINT64_C(1) << 8)
#define VM_ATTR_CACHE_WC       (UINT64_C(1) << 9)
#define VM_ATTR_CACHE_UC       (UINT64_C(1) << 10)
#define VM_ATTR_CACHE_WT       (UINT64_C(1) << 11)
#define VM_ATTR_CACHE_WP       (UINT64_C(1) << 12)
#define VM_ATTR_CACHE_UC_MINUS (UINT64_C(1) << 13)
#define VM_ATTR_CACHE_MASK \
    (VM_ATTR_CACHE_WB | VM_ATTR_CACHE_WC | VM_ATTR_CACHE_UC | \
     VM_ATTR_CACHE_WT | VM_ATTR_CACHE_WP | VM_ATTR_CACHE_UC_MINUS)

typedef struct {
    vaddr_t start;
    uint64_t size;
    vm_region_type_t type;
    uint64_t attrs;
} vm_region_info_t;

struct vm_range;

typedef struct {
    vaddr_t base;
    uint64_t size;

    uint64_t free_bytes;
    uint64_t used_bytes;
    uint64_t region_count;

    /*
     * free_root:
     *   Linux vmalloc 风格 augmented RB-tree，subtree_max 用于找空洞。
     *
     * free_head/free_tail:
     *   地址有序双链表，用于 O(1) 访问相邻 free range。
     *
     * used_root:
     *   已占用 region RB-tree；保存 type/attrs，并支持按任意 VA 查询。
     *
     * tree 内部 key/range 都使用相对 base 的 offset，这样完整高半区
     * [0xFFFF800000000000, 2^64) 也能用 uint64_t 的 [0, size) 表示。
     */
    struct vm_range *free_root;
    struct vm_range *free_head;
    struct vm_range *free_tail;
    struct vm_range *used_root;

    pmm_cpu_t *cpu;
} vm_space_t;

bool vm_space_init(vm_space_t *space, pmm_cpu_t *cpu,
                   vaddr_t base, uint64_t size);

bool vm_alloc(vm_space_t *space, uint64_t size, uint64_t align,
              vm_region_type_t type, uint64_t attrs,
              vaddr_t *out_addr);

bool vm_reserve(vm_space_t *space, vaddr_t addr, uint64_t size,
                vm_region_type_t type, uint64_t attrs);

/*
 * addr 必须是 region 的起始地址；大小由 used region metadata 保存。
 * PINNED region 拒绝释放。PRESENT 4K PTE 会被解除映射；
 * 若带 VM_ATTR_PMM_OWNED，对应 physical frame 会归还 PMM。
 * 当前不拆 2M/1G large leaf，也暂不回收空页表页。
 */
bool vm_free(vm_space_t *space, vaddr_t addr);

/* addr 可以是 region 内任意地址。 */
bool vm_query(const vm_space_t *space, vaddr_t addr,
              vm_region_info_t *out_info);

typedef bool (*vm_region_visit_t)(const vm_region_info_t *region,
                                  void *context);

/*
 * 按虚拟地址从低到高遍历当前所有 used region。
 * visitor 返回 false 时提前停止。
 */
bool vm_for_each_region(const vm_space_t *space,
                        vm_region_visit_t visitor,
                        void *context);

#endif
