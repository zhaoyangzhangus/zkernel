#pragma once
#pragma once
#include <kernel/base.h>

#define X86_64_VA_BITS 48u
#define X86_64_USER_TOP 0x00007fffffffffffULL

#define X86_64_DIRECT_MAP_BASE 0xffff800000000000ULL
#define X86_64_DIRECT_MAP_END  0xffffbfffffffffffULL
#define X86_64_VMALLOC_BASE    0xffffc00000000000ULL
#define X86_64_VMALLOC_END     0xffffdfffffffffffULL
#define X86_64_MMIO_BASE       0xffffe00000000000ULL
#define X86_64_MMIO_END        0xffffefffffffffffULL
#define X86_64_KERNEL_AUX_BASE 0xfffff00000000000ULL
#define X86_64_KERNEL_IMAGE_BASE 0xffffffff80000000ULL

enum x86_cache_mode {
    X86_CACHE_WB = 0,
    X86_CACHE_WC,
    X86_CACHE_UC,
};

uint64_t x86_pte_cache_bits(enum x86_cache_mode mode, bool large_page);
bool x86_is_canonical(uint64_t va);

enum x86_page_map_flags {
    X86_PAGE_WRITE  = 1u << 0,
    X86_PAGE_USER   = 1u << 1,
    X86_PAGE_EXEC   = 1u << 2,
    X86_PAGE_GLOBAL = 1u << 3,
};

#define X86_TLB_IPI_VECTOR 0xF1U

/* 四级页表的 4 KiB 映射接口；root 是页表根的物理地址。 */
void x86_paging_arch_init(void);
void x86_paging_arch_init_secondary(void);
paddr_t x86_current_root_table(void);
void x86_activate_root_table(paddr_t root);
void x86_activate_root_table_pcid(paddr_t root, uint16_t pcid);
uint64_t x86_cr3_value(paddr_t root, uint16_t pcid);
void x86_sync_kernel_half(paddr_t target, paddr_t kernel_root);
kstatus_t x86_map_page(paddr_t root, vaddr_t virtual_address, paddr_t physical_address,
                       uint32_t flags, enum x86_cache_mode cache_mode);
kstatus_t x86_unmap_page(paddr_t root, vaddr_t virtual_address, paddr_t *old_physical);
#define X86_USER_UNMAP_BATCH_MAX 64U
/* 用户地址必须有序且互不重复；先同步失效根页表，再修改映射计数并回收页表。
 * 批处理失败时会恢复 PTE。 */
kstatus_t x86_unmap_user_page_batch(paddr_t root, const vaddr_t *addresses,
                                   size_t count, size_t *unmapped);
kstatus_t x86_protect_page(paddr_t root, vaddr_t virtual_address, uint32_t flags,
                           enum x86_cache_mode cache_mode);
/* 仅供 fork 更新用户 PTE。必须持续持有 VM 锁；发布子进程前，
 * 对已修改的 PTE 必须调用 x86_tlb_shootdown_root。 */
kstatus_t x86_protect_user_page_deferred(paddr_t root, vaddr_t address,
    uint32_t flags, enum x86_cache_mode cache_mode, bool *changed);
bool x86_tlb_shootdown_root(paddr_t root);
/* 判断是否存在 PTE/大页映射与 [start, end) 相交。
 * 稀疏 VM 预留区可能宽达数 TB，因此调用方不能为了确认没有常驻页，
 * 就扫描每个 4 KiB 虚拟页。 */
bool x86_page_range_has_present_pages(paddr_t root, vaddr_t start,
                                      uint64_t end);
/* 在按页对齐的规范用户地址范围内查找第一个有效映射。
 * K_ENOENT 表示没有常驻映射；遇到缺失分支时会直接跳过。 */
kstatus_t x86_next_present_page(paddr_t root, vaddr_t start, uint64_t end,
                                vaddr_t *address);
kstatus_t x86_translate_page(paddr_t root, vaddr_t virtual_address,
                             paddr_t *physical_address, uint64_t *entry_flags);
bool x86_page_entry_writable(uint64_t entry_flags);
bool x86_tlb_shootdown_page(paddr_t root, vaddr_t virtual_address);
bool x86_tlb_shootdown_self_test(void);
void x86_tlb_ipi_interrupt(void);
bool x86_tlb_shootdown_active(void);
bool x86_tlb_cpu_waiting(uint32_t cpu_index);
void x86_tlb_wait_begin(void);
void x86_tlb_wait_end(void);
void x86_page_table_debug_state(uint32_t *lock_state, uint32_t *owner_cpu,
                                uint32_t *waiter_cpu, uint64_t *wait_count);
void x86_tlb_debug_state(uint32_t *lock_state, uint32_t *owner_cpu,
                         uint32_t *waiting_mask, uint32_t *ack_mask,
                         uint64_t *generation);
