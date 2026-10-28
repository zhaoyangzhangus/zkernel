#include "internal.h"

/* REFACTOR_P2_PAGE_ALLOC_OWNER: cold order allocation and Buddy fallback. */

PAGE_NOINLINE void free_legacy_slow(page_t *head, uint8_t order) {
    lock_pages();
    free_block_locked(head, order);
    unlock_pages();
}

PAGE_NOINLINE page_t *page_alloc_slow(uint8_t order, page_alloc_flags_t flags) {
    if (order > BUDDY_MAX_ORDER) return 0;

    page_t *page = 0;
    if (order <= PAGE_POOL_SMALL_MAX_ORDER) {
        uint64_t irq_flags = page_pool_irq_save();
        x86_preempt_disable_fast();
        uint32_t cpu = x86_current_cpu_index_fast();
        for (unsigned attempt = 0; attempt < 3U; ++attempt) {
            pool_cpu_lock(cpu);
            if ((flags & PAGE_ALLOC_DMA32) != 0) {
                page = pool_alloc_small(PAGE_ZONE_DMA32, order, cpu);
            } else {
                page = pool_alloc_small(PAGE_ZONE_NORMAL, order, cpu);
                if (page == 0) page = pool_alloc_small(PAGE_ZONE_DMA32, order, cpu);
            }
            pool_cpu_unlock(cpu);
            if (page != 0 || attempt == 2U) break;
            if (attempt == 0U) pool_drain_cpu(cpu);
            else {
                /* 复用已有远端槽位比每借用一个页面就扫描所有者的完整内存池列表更节省开销。 */
                page = pool_alloc_remote(order, flags, cpu);
                if (page != 0) break;
                pool_reclaim_all();
            }
        }
        if (page == 0) page = pool_alloc_remote(order, flags, cpu);
        x86_preempt_enable_fast();
        page_pool_irq_restore(irq_flags);
    } else {
        for (unsigned attempt = 0; attempt < 3U; ++attempt) {
            lock_pages();
            if ((flags & PAGE_ALLOC_DMA32) != 0) {
                page = take_from_zone_locked(PAGE_ZONE_DMA32, order);
            } else {
                page = take_from_zone_locked(PAGE_ZONE_NORMAL, order);
                if (page == 0) page = take_from_zone_locked(PAGE_ZONE_DMA32, order);
            }
            unlock_pages();
            if (page != 0 || attempt == 2U) break;
            if (attempt == 0U) {
                x86_preempt_disable_fast();
                uint32_t cpu = x86_current_cpu_index_fast();
                pool_drain_cpu(cpu);
                x86_preempt_enable_fast();
            } else pool_reclaim_all();
        }
    }

    if (page != 0) page_stats_note_alloc(order);
    if (page != 0 && (flags & PAGE_ALLOC_ZERO) != 0) {
        void *memory = phys_to_direct(page_to_phys(page));
        if (memory == 0) {
            page_free(page);
            return 0;
        }
        page_memory_zero(memory, (size_t)(1ULL << order) * PAGE_SIZE);
    }
    return page;
}

PAGE_NOINLINE void page_free_non_o0(page_t *head, uint8_t order) {
    if (order <= PAGE_POOL_SMALL_MAX_ORDER) {
        uint64_t irq_flags = page_pool_irq_save();
        x86_preempt_disable_fast();
        uint32_t cpu = x86_current_cpu_index_fast();
        pool_cpu_lock(cpu);
        bool pooled = pool_free_small(head, order, cpu);
        pool_cpu_unlock(cpu);
        x86_preempt_enable_fast();
        page_pool_irq_restore(irq_flags);
        if (pooled) return;
    }
    free_legacy_slow(head, order);
}

