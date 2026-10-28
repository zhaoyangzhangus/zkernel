#include <arch/x86_64/paging.h>
#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

#include "mmu_internal.h"

#define PAGE_TABLE_USER_GUARDS 64U
typedef struct {
    spinlock_t lock;
    atomic_uint owner;
} __attribute__((aligned(64))) page_table_guard_t;
/* 共享内核页表始终使用 guard 0；私有用户根页表使用稳定的分片锁。哈希冲突只会安全地
 * 串行化操作，不需要额外的生命周期钩子。 */
static page_table_guard_t g_page_table_guards[PAGE_TABLE_USER_GUARDS + 1U];
static atomic_uint g_page_table_cpu_guard[MAX_CPUS];
static atomic_uint g_page_table_waiter;
static atomic_uint_fast64_t g_page_table_wait_count;

static uint64_t *table_from_physical(uint64_t physical);

static uint32_t table_guard_index(paddr_t root, uint64_t address) {
    if (address > X86_64_USER_TOP) return 0U;
    uint64_t key = root.value >> PAGE_SHIFT;
    return 1U + (uint32_t)((key ^ (key >> 6) ^ (key >> 12)) &
                           (PAGE_TABLE_USER_GUARDS - 1U));
}

static void table_lock(paddr_t root, uint64_t address) {
    uint64_t saved_flags = 0;
    bool enabled_delivery = false;

    /*
    * 页表操作可能跨越内存分配、TLB shootdown 和最终 PTE 发布。等待时可以开启中断，
    * 以便确认远端 TLB IPI；但持有此 guard 期间不得切换当前线程。
     */
    sched_preempt_disable();
    uint32_t cpu_index = x86_current_cpu_index();
    uint32_t index = table_guard_index(root, address);
    page_table_guard_t *guard = &g_page_table_guards[index];
    atomic_store_explicit(&g_page_table_cpu_guard[cpu_index], index,
                          memory_order_relaxed);
    while (!spinlock_try_lock(&guard->lock)) {
        atomic_store_explicit(&g_page_table_waiter, cpu_index,
                              memory_order_release);
        atomic_fetch_add_explicit(&g_page_table_wait_count, 1U,
                                  memory_order_relaxed);
        if (!enabled_delivery) {
            __asm__ volatile ("pushfq; popq %0" : "=r"(saved_flags) : : "memory");
            x86_tlb_wait_begin();
            /* 页表锁持有者可能正在等待本 CPU 的 TLB 确认。 */
            __asm__ volatile ("sti" : : : "memory");
            enabled_delivery = true;
        }
        do {
            __asm__ volatile ("pause");
        } while (atomic_load_explicit(&guard->lock.state,
                                       memory_order_relaxed) != 0U);
    }
    atomic_store_explicit(&guard->owner, cpu_index,
                          memory_order_release);
    atomic_store_explicit(&g_page_table_waiter, UINT32_MAX,
                          memory_order_release);
    if (enabled_delivery) {
        if ((saved_flags & (1ULL << 9)) == 0) {
            __asm__ volatile ("cli" : : : "memory");
        }
        x86_tlb_wait_end();
    }
}

static void table_unlock(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    uint32_t index = atomic_load_explicit(&g_page_table_cpu_guard[cpu_index],
                                          memory_order_relaxed);
    page_table_guard_t *guard = &g_page_table_guards[index];
    atomic_store_explicit(&guard->owner, UINT32_MAX,
                          memory_order_release);
    spinlock_unlock(&guard->lock);
    sched_preempt_enable();
}

void x86_page_table_arch_init(void) {
    for (uint32_t index = 0U; index <= PAGE_TABLE_USER_GUARDS; ++index) {
        spinlock_init(&g_page_table_guards[index].lock);
        atomic_init(&g_page_table_guards[index].owner, UINT32_MAX);
    }
    for (uint32_t cpu = 0U; cpu < MAX_CPUS; ++cpu)
        atomic_init(&g_page_table_cpu_guard[cpu], 0U);
    atomic_store_explicit(&g_page_table_waiter, UINT32_MAX,
                          memory_order_relaxed);
    atomic_init(&g_page_table_wait_count, 0U);
    /* PAT 索引 0/1/2/3 分别固定为 WB/WC/UC-/UC。 */
    (void)0;
}

void x86_page_table_debug_state(uint32_t *lock_state, uint32_t *owner_cpu,
                                uint32_t *waiter_cpu,
                                uint64_t *wait_count) {
    /* 诊断信息描述最近等待者使用的分片，或此 CPU 最近使用的分片。独立用户根页表没有
     * 单一的全局所有者。 */
    uint32_t waiter = atomic_load_explicit(&g_page_table_waiter,
                                           memory_order_acquire);
    uint32_t cpu = waiter < MAX_CPUS ? waiter : x86_current_cpu_index();
    uint32_t index = atomic_load_explicit(&g_page_table_cpu_guard[cpu],
                                          memory_order_relaxed);
    page_table_guard_t *guard = &g_page_table_guards[index];
    if (lock_state != 0) {
        *lock_state = atomic_load_explicit(&guard->lock.state,
                                           memory_order_acquire);
    }
    if (owner_cpu != 0) {
        *owner_cpu = atomic_load_explicit(&guard->owner,
                                          memory_order_acquire);
    }
    if (waiter_cpu != 0) {
        *waiter_cpu = atomic_load_explicit(&g_page_table_waiter,
                                           memory_order_acquire);
    }
    if (wait_count != 0) {
        *wait_count = atomic_load_explicit(&g_page_table_wait_count,
                                           memory_order_relaxed);
    }
}


void x86_sync_kernel_half(paddr_t target, paddr_t kernel_root) {
    if (target.value == kernel_root.value) return;
    uint64_t *target_pml4 = table_from_physical(target.value);
    uint64_t *kernel_pml4 = table_from_physical(kernel_root.value);
    if (target_pml4 == 0 || kernel_pml4 == 0) return;
    table_lock(kernel_root, UINT64_MAX);
    for (uint32_t index = 256U; index < 512U; ++index) {
        target_pml4[index] = kernel_pml4[index];
    }
    table_unlock();
}

static uint64_t *table_from_physical(uint64_t physical) {
    return (uint64_t *)phys_to_direct(paddr_make(physical & PTE_ADDRESS_MASK));
}

static kstatus_t next_table(uint64_t *entry, bool user, uint64_t **next) {
    if ((*entry & PTE_PRESENT) != 0) {
        if ((*entry & PTE_LARGE) != 0) return K_EBUSY;
        if (user) *entry |= PTE_USER;
        *next = table_from_physical(*entry);
        if (*next == 0) {
            (void)kernel_serial_printf_serial_only(
                "OS_PT_NEXT_TABLE_FAIL entry=%llx physical=%llx\r\n",
                (unsigned long long)*entry,
                (unsigned long long)(*entry & PTE_ADDRESS_MASK));
            return K_EIO;
        }
        return K_OK;
    }
    page_t *page = page_alloc(0, PAGE_ALLOC_ZERO);
    if (page == 0) return K_ENOMEM;
    page->owner = PAGE_OWNER_PAGETABLE;
    paddr_t physical = page_to_phys(page);
    if (physical.value == UINT64_MAX) {
        page_free(page);
        return K_EIO;
    }
    *entry = physical.value | PTE_PRESENT | PTE_WRITE | (user ? PTE_USER : 0U);
    *next = table_from_physical(physical.value);
    if (*next == 0) {
        (void)kernel_serial_printf_serial_only(
            "OS_PT_ALLOC_TABLE_FAIL physical=%llx\r\n",
            (unsigned long long)physical.value);
        *entry = 0;
        page_free(page);
        return K_EIO;
    }
    return K_OK;
}

static bool table_empty(const uint64_t *table) {
    for (uint32_t i = 0; i < 512U; ++i) {
        if ((table[i] & PTE_PRESENT) != 0) return false;
    }
    return true;
}

static void release_dynamic_table(uint64_t *parent_entry, uint64_t *table) {
    if (!table_empty(table)) return;
    page_t *page = phys_to_page(paddr_make(*parent_entry & PTE_ADDRESS_MASK));
    if (page == 0 || page->owner != PAGE_OWNER_PAGETABLE) return;
    *parent_entry = 0;
    page_free(page);
}

kstatus_t x86_map_page(paddr_t root, vaddr_t virtual_address, paddr_t physical_address,
                       uint32_t flags, enum x86_cache_mode cache_mode) {
    uint64_t va = (uint64_t)virtual_address;
    if (!x86_is_canonical(va) || (va & (PAGE_SIZE - 1ULL)) != 0 ||
        (physical_address.value & (PAGE_SIZE - 1ULL)) != 0) return K_EINVAL;
    uint64_t *pml4 = table_from_physical(root.value);
    if (pml4 == 0) return K_EINVAL;
    uint32_t i4 = (uint32_t)((va >> 39) & 0x1FFU);
    uint32_t i3 = (uint32_t)((va >> 30) & 0x1FFU);
    uint32_t i2 = (uint32_t)((va >> 21) & 0x1FFU);
    uint32_t i1 = (uint32_t)((va >> 12) & 0x1FFU);
    bool user = (flags & X86_PAGE_USER) != 0;

    table_lock(root, va);
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;
    kstatus_t status = next_table(&pml4[i4], user, &pdpt);
    if (status == K_OK) status = next_table(&pdpt[i3], user, &pd);
    if (status == K_OK) status = next_table(&pd[i2], user, &pt);
    if (status == K_OK && (pt[i1] & PTE_PRESENT) != 0) status = K_EBUSY;
    if (status == K_OK) {
        uint64_t entry = physical_address.value | PTE_PRESENT |
                         x86_pte_cache_bits(cache_mode, false);
        if ((flags & X86_PAGE_WRITE) != 0) entry |= PTE_WRITE;
        if (user) entry |= PTE_USER;
        if ((flags & X86_PAGE_GLOBAL) != 0) entry |= PTE_GLOBAL;
        if ((flags & X86_PAGE_EXEC) == 0) entry |= PTE_NO_EXECUTE;
        pt[i1] = entry;
        page_t *page = phys_to_page(physical_address);
        if (page != 0) {
            atomic_fetch_add_explicit(&page->mapcount, 1, memory_order_relaxed);
        }

        /*
         * PTE 从 NOT-PRESENT -> PRESENT 是新映射，没有旧 translation 需要
         * 失效。这里做全 CPU shootdown 不但没有收益，还会让 vmalloc()
         * 每映射一个新页都依赖所有 CPU 的 IPI ACK。
         *
         * VA 的复用安全性由 unmap 保证：只有旧映射成功 shootdown 后，
         * 对应虚拟区间才允许被释放/再次分配。
         */
    }
    table_unlock();
    return status;
}

kstatus_t x86_translate_page(paddr_t root, vaddr_t virtual_address,
                             paddr_t *physical_address, uint64_t *entry_flags) {
    uint64_t va = (uint64_t)virtual_address;
    if (!x86_is_canonical(va) || physical_address == 0) return K_EINVAL;
    /* 页表释放也受同一把锁保护，避免遍历到刚被 unmap 回收的中间页表。 */
    table_lock(root, va);
    uint64_t *table = table_from_physical(root.value);
    if (table == 0) {
        table_unlock();
        return K_EINVAL;
    }
    const uint8_t shifts[4] = {39U, 30U, 21U, 12U};
    for (uint32_t level = 0; level < 4U; ++level) {
        uint64_t entry = table[(va >> shifts[level]) & 0x1FFU];
        if ((entry & PTE_PRESENT) == 0) {
            table_unlock();
            return K_ENOENT;
        }
        if (level < 3U && (entry & PTE_LARGE) != 0) {
            uint64_t offset_mask = (1ULL << shifts[level]) - 1ULL;
            physical_address->value = (entry & ~offset_mask & PTE_ADDRESS_MASK) |
                                      (va & offset_mask);
            if (entry_flags != 0) *entry_flags = entry;
            table_unlock();
            return K_OK;
        }
        if (level == 3U) {
            physical_address->value = (entry & PTE_ADDRESS_MASK) | (va & (PAGE_SIZE - 1ULL));
            if (entry_flags != 0) *entry_flags = entry;
            table_unlock();
            return K_OK;
        }
        table = table_from_physical(entry);
        if (table == 0) {
            (void)kernel_serial_printf_serial_only(
                "OS_PT_TRANSLATE_WALK_FAIL level=%u root=%llx va=%llx "
                "entry=%llx physical=%llx\r\n",
                level, (unsigned long long)root.value,
                (unsigned long long)va, (unsigned long long)entry,
                (unsigned long long)(entry & PTE_ADDRESS_MASK));
            table_unlock();
            return K_EIO;
        }
    }
    table_unlock();
    return K_ENOENT;
}

bool x86_page_entry_writable(uint64_t entry_flags) {
    return (entry_flags & PTE_WRITE) != 0U;
}

kstatus_t x86_unmap_page(paddr_t root, vaddr_t virtual_address, paddr_t *old_physical) {
    uint64_t va = (uint64_t)virtual_address;
    if (!x86_is_canonical(va) || (va & (PAGE_SIZE - 1ULL)) != 0) return K_EINVAL;
    uint64_t *pml4 = table_from_physical(root.value);
    if (pml4 == 0) return K_EINVAL;
    uint32_t i4 = (uint32_t)((va >> 39) & 0x1FFU);
    uint32_t i3 = (uint32_t)((va >> 30) & 0x1FFU);
    uint32_t i2 = (uint32_t)((va >> 21) & 0x1FFU);
    uint32_t i1 = (uint32_t)((va >> 12) & 0x1FFU);

    table_lock(root, va);
    if ((pml4[i4] & (PTE_PRESENT | PTE_LARGE)) != PTE_PRESENT) {
        table_unlock();
        return K_ENOENT;
    }
    uint64_t *pdpt = table_from_physical(pml4[i4]);
    if (pdpt == 0 || (pdpt[i3] & (PTE_PRESENT | PTE_LARGE)) != PTE_PRESENT) {
        table_unlock();
        return K_ENOENT;
    }
    uint64_t *pd = table_from_physical(pdpt[i3]);
    if (pd == 0 || (pd[i2] & (PTE_PRESENT | PTE_LARGE)) != PTE_PRESENT) {
        table_unlock();
        return K_ENOENT;
    }
    uint64_t *pt = table_from_physical(pd[i2]);
    if (pt == 0 || (pt[i1] & PTE_PRESENT) == 0) {
        table_unlock();
        return K_ENOENT;
    }
    uint64_t old_entry = pt[i1];
    pt[i1] = 0;
    if (!x86_tlb_shootdown_page(root, (vaddr_t)va)) {
        /* 未收到全部确认前不能释放映射或页表页。 */
        pt[i1] = old_entry;
        __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
        table_unlock();
        return K_EIO;
    }
    paddr_t physical = paddr_make(old_entry & PTE_ADDRESS_MASK);
    page_t *mapped_page = phys_to_page(physical);
    if (mapped_page != 0) {
        atomic_fetch_sub_explicit(&mapped_page->mapcount, 1, memory_order_relaxed);
    }
    if (i4 < 256U) {
        release_dynamic_table(&pd[i2], pt);
        release_dynamic_table(&pdpt[i3], pd);
        release_dynamic_table(&pml4[i4], pdpt);
    }
    table_unlock();
    if (old_physical != 0) *old_physical = physical;
    return K_OK;
}

kstatus_t x86_unmap_user_page_batch(paddr_t root, const vaddr_t *addresses,
                                   size_t count, size_t *unmapped) {
    uint64_t *entries[X86_USER_UNMAP_BATCH_MAX];
    uint64_t saved[X86_USER_UNMAP_BATCH_MAX];
    size_t resident_count = 0U;
    if (unmapped == 0) return K_EINVAL;
    *unmapped = 0U;
    if (root.value == 0U || addresses == 0 || count == 0U ||
        count > X86_USER_UNMAP_BATCH_MAX) return K_EINVAL;
    uint64_t *pml4 = table_from_physical(root.value);
    if (pml4 == 0) return K_EINVAL;
    for (size_t index = 0U; index < count; ++index) {
        uint64_t va = addresses[index];
        if (va > X86_64_USER_TOP || (va & (PAGE_SIZE - 1U)) != 0U ||
            (index != 0U && va <= addresses[index - 1U])) return K_EINVAL;
    }
    table_lock(root, 0U);
    /* 修改任何 PTE 前先验证整个批次。缺少叶项无害；大页、特权级映射和全局映射不能
     * 批量处理。 */
    for (size_t index = 0U; index < count; ++index) {
        uint64_t va = addresses[index];
        uint64_t *table = pml4;
        for (uint32_t shift = 39U; shift > 12U; shift -= 9U) {
            uint64_t entry = table[(va >> shift) & 511U];
            if ((entry & PTE_PRESENT) == 0U) { table = 0; break; }
            if ((entry & PTE_LARGE) != 0U) {
                table_unlock(); return K_EINVAL;
            }
            table = table_from_physical(entry);
            if (table == 0) { table_unlock(); return K_EIO; }
        }
        if (table == 0) continue;
        uint64_t *entry = &table[(va >> 12) & 511U];
        if ((*entry & PTE_PRESENT) == 0U) continue;
        if ((*entry & (PTE_USER | PTE_GLOBAL)) != PTE_USER) {
            table_unlock(); return K_EINVAL;
        }
        entries[resident_count] = entry;
        saved[resident_count++] = *entry;
    }
    if (resident_count == 0U) { table_unlock(); return K_OK; }
    for (size_t index = 0U; index < resident_count; ++index) *entries[index] = 0U;
    if (!x86_tlb_shootdown_root(root)) {
        /* 所有目标 CPU 均确认前，不释放任何资源所有权。 */
        for (size_t index = 0U; index < resident_count; ++index)
            *entries[index] = saved[index];
        table_unlock(); return K_EIO;
    }
    for (size_t index = 0U; index < resident_count; ++index) {
        page_t *page = phys_to_page(paddr_make(saved[index] & PTE_ADDRESS_MASK));
        if (page != 0)
            atomic_fetch_sub_explicit(&page->mapcount, 1, memory_order_relaxed);
    }
    /* 排序后的地址可能共享 PT/PD/PDPT。仅在每组最后一个地址处检查各级页表是否为空，
     * 不必为每个已移除叶项都扫描一次。回收前重新遍历，因为已保存的父指针可能已经失效。
     * 即使一组末尾是空洞，也仍需触发祖先页表回收。 */
    for (size_t index = 0U; index < count; ++index) {
        uint64_t va = addresses[index];
        if (index + 1U < count && (addresses[index + 1U] >> 21) == (va >> 21))
            continue;
        uint32_t i4 = (uint32_t)((va >> 39) & 511U);
        uint32_t i3 = (uint32_t)((va >> 30) & 511U);
        uint32_t i2 = (uint32_t)((va >> 21) & 511U);
        if ((pml4[i4] & PTE_PRESENT) == 0U) continue;
        uint64_t *pdpt = table_from_physical(pml4[i4]);
        if ((pdpt[i3] & PTE_PRESENT) != 0U) {
            uint64_t *pd = table_from_physical(pdpt[i3]);
            if ((pd[i2] & PTE_PRESENT) != 0U) {
                uint64_t *pt = table_from_physical(pd[i2]);
                release_dynamic_table(&pd[i2], pt);
            }
            if (index + 1U == count || (addresses[index + 1U] >> 30) != (va >> 30))
                release_dynamic_table(&pdpt[i3], pd);
        }
        if (index + 1U == count || (addresses[index + 1U] >> 39) != (va >> 39))
            release_dynamic_table(&pml4[i4], pdpt);
    }
    *unmapped = resident_count;
    table_unlock();
    return K_OK;
}

/*
 * 仅遍历覆盖目标范围的页表项。Linux MAP_NORESERVE 预留会创建 VMA，但通常不会创建用户
 * 页表；探测顶层页表项可使此类范围上的 mprotect/munmap 复杂度取决于已填充的页表分支数，
 * 而非虚拟页数。页表损坏时向调用方保守地报告“存在”，以继续沿用现有的逐页错误处理。
 */
static bool page_table_range_has_present_locked(const uint64_t *table,
                                                uint32_t level,
                                                uint64_t table_base,
                                                uint64_t start,
                                                uint64_t end) {
    const uint32_t shift = 12U + (level - 1U) * 9U;
    const uint64_t span = 1ULL << shift;
    uint64_t first;
    uint64_t last;

    if (table == 0 || level == 0U || start >= end) return true;

    first = start <= table_base ? 0U : (start - table_base) / span;
    last = (end - 1U) <= table_base ? 0U :
           ((end - 1U - table_base) / span);
    if (first >= 512U) return false;
    if (last >= 512U) last = 511U;

    for (uint64_t index = first; index <= last; ++index) {
        uint64_t entry = table[index];
        uint64_t entry_base = table_base + index * span;
        uint64_t entry_end = entry_base + span;

        if ((entry & PTE_PRESENT) == 0U) continue;
        if (level == 1U || (entry & PTE_LARGE) != 0U) return true;

        uint64_t *child = table_from_physical(entry);
        if (child == 0) return true;
        uint64_t child_start = start > entry_base ? start : entry_base;
        uint64_t child_end = end < entry_end ? end : entry_end;
        if (page_table_range_has_present_locked(
                child, level - 1U, entry_base, child_start, child_end)) {
            return true;
        }
    }
    return false;
}

bool x86_page_range_has_present_pages(paddr_t root, vaddr_t start,
                                      uint64_t end) {
    uint64_t begin = (uint64_t)start;
    uint64_t *pml4;
    bool present;

    /* VM 层传入规范的用户地址范围。遇到意外输入时采取保守处理，避免此辅助函数掩盖
     * 已有错误。 */
    if (begin >= end || !x86_is_canonical(begin) ||
        end > X86_64_USER_TOP + 1ULL) {
        return true;
    }

    table_lock(root, begin);
    pml4 = table_from_physical(root.value);
    present = pml4 == 0 ? true :
              page_table_range_has_present_locked(
                  pml4, 4U, 0U, begin, end);
    table_unlock();
    return present;
}

static kstatus_t page_table_next_present_locked(const uint64_t *table,
                                                uint32_t level,
                                                uint64_t table_base,
                                                uint64_t start, uint64_t end,
                                                vaddr_t *address) {
    if (table == 0 || level == 0U || start >= end) return K_EIO;
    const uint64_t span = 1ULL << (12U + (level - 1U) * 9U);
    uint64_t first = start <= table_base ? 0U : (start - table_base) / span;
    uint64_t last = (end - 1U - table_base) / span;
    if (first >= 512U) return K_ENOENT;
    if (last >= 512U) last = 511U;
    for (uint64_t index = first; index <= last; ++index) {
        const uint64_t entry = table[index];
        if ((entry & PTE_PRESENT) == 0U) continue;
        const uint64_t base = table_base + index * span;
        const uint64_t begin = start > base ? start : base;
        const uint64_t limit = end < base + span ? end : base + span;
        if (level == 1U || (entry & PTE_LARGE) != 0U) {
            *address = (vaddr_t)begin;
            return K_OK;
        }
        uint64_t *child = table_from_physical(entry);
        if (child == 0) return K_EIO;
        kstatus_t status = page_table_next_present_locked(
            child, level - 1U, base, begin, limit, address);
        if (status != K_ENOENT) return status;
    }
    return K_ENOENT;
}

kstatus_t x86_next_present_page(paddr_t root, vaddr_t start, uint64_t end,
                                vaddr_t *address) {
    if (address == 0 || (uint64_t)start >= end ||
        ((uint64_t)start & (PAGE_SIZE - 1ULL)) != 0U ||
        (end & (PAGE_SIZE - 1ULL)) != 0U ||
        !x86_is_canonical((uint64_t)start) || end > X86_64_USER_TOP + 1ULL) {
        return K_EINVAL;
    }
    table_lock(root, start);
    uint64_t *pml4 = table_from_physical(root.value);
    kstatus_t status = pml4 == 0 ? K_EIO :
        page_table_next_present_locked(pml4, 4U, 0U, start, end, address);
    table_unlock();
    return status;
}

/* 仅延迟用户态、非全局映射的权限变更；调用方不会用此路径替换物理页或释放映射。
 * 刷新根页表即可完成整个批次。 */
kstatus_t x86_protect_user_page_deferred(paddr_t root, vaddr_t address,
    uint32_t flags, enum x86_cache_mode cache_mode, bool *changed) {
    uint64_t va = (uint64_t)address;
    if (changed == 0) return K_EINVAL;
    *changed = false;
    if (root.value == 0 || va > X86_64_USER_TOP ||
        (va & (PAGE_SIZE - 1ULL)) != 0 ||
        (flags & (X86_PAGE_USER | X86_PAGE_GLOBAL)) != X86_PAGE_USER) {
        return K_EINVAL;
    }
    const uint32_t indices[4] = {
        (uint32_t)((va >> 39) & 511U), (uint32_t)((va >> 30) & 511U),
        (uint32_t)((va >> 21) & 511U), (uint32_t)((va >> 12) & 511U)};
    table_lock(root, va);
    uint64_t *table = table_from_physical(root.value);
    if (table == 0) { table_unlock(); return K_EINVAL; }
    for (uint32_t level = 0; level < 3U; ++level) {
        uint64_t entry = table[indices[level]];
        if ((entry & (PTE_PRESENT | PTE_LARGE)) != PTE_PRESENT) {
            table_unlock(); return K_ENOENT;
        }
        table = table_from_physical(entry);
        if (table == 0) { table_unlock(); return K_EIO; }
    }
    uint64_t old = table[indices[3]];
    if ((old & PTE_PRESENT) == 0) { table_unlock(); return K_ENOENT; }
    if ((old & (PTE_USER | PTE_GLOBAL)) != PTE_USER) {
        table_unlock(); return K_EINVAL;
    }
    uint64_t entry = (old & (PTE_ADDRESS_MASK | PTE_ACCESSED | PTE_DIRTY)) |
        PTE_PRESENT | PTE_USER | x86_pte_cache_bits(cache_mode, false);
    if ((flags & X86_PAGE_WRITE) != 0) entry |= PTE_WRITE;
    if ((flags & X86_PAGE_EXEC) == 0) entry |= PTE_NO_EXECUTE;
    if (entry != old) {
        table[indices[3]] = entry;
        *changed = true;
    }
    table_unlock();
    return K_OK;
}

kstatus_t x86_protect_page(paddr_t root, vaddr_t virtual_address, uint32_t flags,
                           enum x86_cache_mode cache_mode) {
    uint64_t va = (uint64_t)virtual_address;
    if (!x86_is_canonical(va) || (va & (PAGE_SIZE - 1ULL)) != 0) return K_EINVAL;
    uint64_t *pml4 = table_from_physical(root.value);
    if (pml4 == 0) return K_EINVAL;
    uint32_t indices[4] = {
        (uint32_t)((va >> 39) & 0x1FFU),
        (uint32_t)((va >> 30) & 0x1FFU),
        (uint32_t)((va >> 21) & 0x1FFU),
        (uint32_t)((va >> 12) & 0x1FFU),
    };
    table_lock(root, va);
    uint64_t *table = pml4;
    for (uint32_t level = 0; level < 3U; ++level) {
        uint64_t entry = table[indices[level]];
        if ((entry & (PTE_PRESENT | PTE_LARGE)) != PTE_PRESENT) {
            table_unlock();
            return K_ENOENT;
        }
        table = table_from_physical(entry);
        if (table == 0) {
            (void)kernel_serial_printf_serial_only(
                "OS_PT_PROTECT_WALK_FAIL level=%u root=%llx va=%llx "
                "entry=%llx physical=%llx\r\n",
                level, (unsigned long long)root.value,
                (unsigned long long)va, (unsigned long long)entry,
                (unsigned long long)(entry & PTE_ADDRESS_MASK));
            table_unlock();
            return K_EIO;
        }
    }
    uint64_t old = table[indices[3]];
    if ((old & PTE_PRESENT) == 0) {
        table_unlock();
        return K_ENOENT;
    }
    uint64_t entry = (old & PTE_ADDRESS_MASK) | PTE_PRESENT |
                     x86_pte_cache_bits(cache_mode, false);
    if ((flags & X86_PAGE_WRITE) != 0) entry |= PTE_WRITE;
    if ((flags & X86_PAGE_USER) != 0) entry |= PTE_USER;
    if ((flags & X86_PAGE_GLOBAL) != 0) entry |= PTE_GLOBAL;
    if ((flags & X86_PAGE_EXEC) == 0) entry |= PTE_NO_EXECUTE;
    /* 重复的 COW fork 通常会遇到已经只读的 PTE。若映射及其权限/缓存策略均未变化，则
     * 保留硬件记账位并避免远端 shootdown。 */
    entry |= old & (PTE_ACCESSED | PTE_DIRTY);
    if (entry == old) {
        table_unlock();
        return K_OK;
    }
    table[indices[3]] = entry;
    /* 提升用户页写权限不会暴露其他物理页，也不会撤销访问权。远端只读转换仍然安全：
     * 后续写入会触发缺页，VM 缺页处理程序会使该本地项失效。替换映射、收紧权限和全局
     * 映射仍使用同步 shootdown。 */
    if (va <= X86_64_USER_TOP &&
        (old & (PTE_USER | PTE_GLOBAL | PTE_WRITE)) == PTE_USER &&
        entry == (old | PTE_WRITE)) {
        __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
        table_unlock();
        return K_OK;
    }
    if (!x86_tlb_shootdown_page(root, (vaddr_t)va)) {
        table[indices[3]] = old;
        __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
        table_unlock();
        return K_EIO;
    }
    table_unlock();
    return K_OK;
}
