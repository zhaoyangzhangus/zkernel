#ifndef __KERNEL_MM_VM_H__
#define __KERNEL_MM_VM_H__

#include <stdbool.h>
#include <stdint.h>

#include "pmm.h"

#define VM_PAGE_SIZE 0x1000ULL

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

#define VM_ATTR_CACHE_WB  (UINT64_C(1) << 8)
#define VM_ATTR_CACHE_WC  (UINT64_C(1) << 9)
#define VM_ATTR_CACHE_UC  (UINT64_C(1) << 10)
#define VM_ATTR_CACHE_MASK     (VM_ATTR_CACHE_WB | VM_ATTR_CACHE_WC | VM_ATTR_CACHE_UC)

typedef struct {
    vaddr_t start;
    vaddr_t end;
    vm_region_type_t type;
    uint64_t attrs;
} vm_region_info_t;

struct vm_range;

typedef struct {
    vaddr_t base;
    vaddr_t end;

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

/* addr 必须是 region 的起始地址；大小由 used region metadata 保存。 */
bool vm_free(vm_space_t *space, vaddr_t addr);

/* addr 可以是 region 内任意地址。 */
bool vm_query(const vm_space_t *space, vaddr_t addr,
              vm_region_info_t *out_info);

#endif
