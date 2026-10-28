#include <arch/x86_64/paging.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/bitmap.h>

#define VMALLOC_MANAGED_SIZE  (1ULL << 30)
#define VMALLOC_PAGE_COUNT    (VMALLOC_MANAGED_SIZE / PAGE_SIZE)
#define VMALLOC_BITMAP_WORDS  (VMALLOC_PAGE_COUNT / 64U)
#define VMALLOC_MAX_AREAS     1024U

typedef struct {
    bool used;
    bool owns_pages;
    bool cleanup_pending;
    uint32_t bitmap_start;
    uint32_t total_pages;
    uint32_t data_pages;
    vaddr_t address;
    paddr_t root_table;
} vmalloc_area_t;

_Static_assert(sizeof(vmalloc_area_t) == 32U, "compact vmalloc area record");

static uint64_t g_vmalloc_bitmap[VMALLOC_BITMAP_WORDS];
static vmalloc_area_t g_vmalloc_areas[VMALLOC_MAX_AREAS];
static spinlock_t g_vmalloc_lock;
static atomic_uint g_vmalloc_init_state;
static volatile uint32_t g_vmalloc_failure;
static uint32_t g_vmalloc_pending_cleanup;
static uint32_t g_vmalloc_next_free_area;
static uint32_t g_vmalloc_area_limit; /* 最后一个已用记录之后的位置 */

static uint64_t vmalloc_lock_irqsave(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    sched_preempt_disable();
    bool enabled_delivery = false;
    unsigned expected = 0U;
    for (;;) {
        if (atomic_compare_exchange_strong_explicit(&g_vmalloc_lock.state,
                &expected, 1U, memory_order_acquire, memory_order_relaxed)) {
            if (enabled_delivery) {
                if ((flags & (1ULL << 9)) == 0) {
                    __asm__ volatile ("cli" : : : "memory");
                }
                x86_tlb_wait_end();
            }
            return flags;
        }
        if (!enabled_delivery) {
            x86_tlb_wait_begin();
            /* 另一个 CPU 可能持有 vmalloc 锁并等待本 CPU 的 TLB 确认。 */
            __asm__ volatile ("sti" : : : "memory");
            enabled_delivery = true;
        }
        /* 禁止本地调度但保持 IF，确保等待期间仍可接收 TLB IPI。 */
        do {
            __asm__ volatile ("pause");
        } while (atomic_load_explicit(&g_vmalloc_lock.state,
                                      memory_order_relaxed) != 0U);
        expected = 0U;
    }
}

static void vmalloc_unlock_irqrestore(uint64_t flags) {
    atomic_store_explicit(&g_vmalloc_lock.state, 0U, memory_order_release);
    sched_preempt_enable();
    if ((flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    } else {
        __asm__ volatile ("cli" : : : "memory");
    }
}

static void vmalloc_initialize(void) {
    if (atomic_load_explicit(&g_vmalloc_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_vmalloc_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_vmalloc_lock.state, 0U);
        g_vmalloc_next_free_area = 0U;
        g_vmalloc_area_limit = 0U;
        for (uint32_t i = 0; i < VMALLOC_BITMAP_WORDS; ++i) g_vmalloc_bitmap[i] = 0;
        for (uint32_t i = 0; i < VMALLOC_MAX_AREAS; ++i) g_vmalloc_areas[i].used = false;
        atomic_store_explicit(&g_vmalloc_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_vmalloc_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static bool find_virtual_run(uint32_t count, uint32_t *start_out) {
    if (count == 0U || count > VMALLOC_PAGE_COUNT) return false;
    uint32_t run_start = 0;
    uint32_t run_length = 0;
    for (uint32_t word = 0; word < VMALLOC_BITMAP_WORDS; ++word) {
        uint64_t used = g_vmalloc_bitmap[word];
        uint32_t base = word * 64U;
        if (used == UINT64_MAX) {
            run_length = 0;
            continue;
        }
        if (used == 0U) {
            if (run_length == 0U) run_start = base;
            run_length += 64U;
            if (run_length >= count) {
                *start_out = run_start;
                return true;
            }
            continue;
        }
        /* 跳过整个占用/空闲区间。绝不移位 64 位，也不对零调用 ctz；
         * 保留空闲后缀，以支持跨越字边界的连续区间。 */
        for (uint32_t bit = 0; bit < 64U;) {
            uint64_t suffix = used >> bit;
            uint32_t remaining = 64U - bit;
            if ((suffix & 1U) != 0U) {
                bit += ~suffix == 0U ? remaining :
                    (uint32_t)__builtin_ctzll(~suffix);
                run_length = 0;
            } else {
                uint32_t free_pages = suffix == 0U ? remaining :
                    (uint32_t)__builtin_ctzll(suffix);
                if (run_length == 0U) run_start = base + bit;
                run_length += free_pages;
                if (run_length >= count) {
                    *start_out = run_start;
                    return true;
                }
                bit += free_pages;
            }
        }
    }
    return false;
}

/* 调用方持有 g_vmalloc_lock。在所有 PTE 均已处理且 TLB 失效得到确认前，保留该区间。重试时已回收
 * 页面会返回 ENOENT，因此所属物理页不会被重复释放。 */
static bool vmalloc_release_area_locked(vmalloc_area_t *area) {
    uint32_t remaining = 0U;
    for (uint32_t i = 0; i < area->data_pages; ++i) {
        paddr_t physical;
        kstatus_t status = x86_unmap_page(area->root_table,
            area->address + (vaddr_t)i * PAGE_SIZE, &physical);
        if (status == K_OK) {
            /* 借用映射只移除 PTE，不转移物理页所有权。 */
            if (area->owns_pages) {
                page_t *page = phys_to_page(physical);
                if (page != 0) page_free(page);
            }
        } else if (status != K_ENOENT) {
            remaining = i + 1U;
            g_vmalloc_failure = 0x400U | (uint32_t)(-status);
        }
    }
    if (remaining != 0U) {
        /* 只有失败前缀仍可能包含有效 PTE。保留完整位图预留，但不要重试已回收的后缀。 */
        area->data_pages = remaining;
        if (!area->cleanup_pending) ++g_vmalloc_pending_cleanup;
        area->cleanup_pending = true;
        return false;
    }
    if (area->cleanup_pending) --g_vmalloc_pending_cleanup;
    bitmap_set_range(g_vmalloc_bitmap, area->bitmap_start, area->total_pages, false);
    area->used = false;
    uint32_t area_index = (uint32_t)(area - g_vmalloc_areas);
    if (area_index < g_vmalloc_next_free_area) g_vmalloc_next_free_area = area_index;
    if (area_index + 1U == g_vmalloc_area_limit) {
        while (g_vmalloc_area_limit != 0U &&
               !g_vmalloc_areas[g_vmalloc_area_limit - 1U].used)
            --g_vmalloc_area_limit;
    }
    area->owns_pages = false;
    area->cleanup_pending = false;
    area->root_table = paddr_make(0);
    return true;
}

static vmalloc_area_t *vmalloc_reserve_area_locked(uint32_t pages,
                                                  bool owns) {
    if (g_vmalloc_pending_cleanup != 0U) {
        for (uint32_t i = 0; i < g_vmalloc_area_limit; ++i) {
            if (g_vmalloc_areas[i].cleanup_pending) {
                (void)vmalloc_release_area_locked(&g_vmalloc_areas[i]);
                if (g_vmalloc_pending_cleanup == 0U) break;
            }
        }
    }
    vmalloc_area_t *area = 0;
    for (uint32_t i = g_vmalloc_next_free_area; i < VMALLOC_MAX_AREAS; ++i) {
        if (!g_vmalloc_areas[i].used) {
            area = &g_vmalloc_areas[i];
            g_vmalloc_next_free_area = i;
            break;
        }
    }
    uint32_t start, total = pages + 2U;
    if (area == 0 || !find_virtual_run(total, &start)) {
        g_vmalloc_failure = 1U;
        return 0;
    }
    bitmap_set_range(g_vmalloc_bitmap, start, total, true);
    area->used = true;
    g_vmalloc_next_free_area = (uint32_t)(area - g_vmalloc_areas) + 1U;
    if (g_vmalloc_next_free_area > g_vmalloc_area_limit)
        g_vmalloc_area_limit = g_vmalloc_next_free_area;
    area->owns_pages = owns;
    area->cleanup_pending = false;
    area->bitmap_start = start;
    area->total_pages = total;
    area->data_pages = pages;
    area->address = (vaddr_t)(X86_64_VMALLOC_BASE + ((uint64_t)start + 1ULL) * PAGE_SIZE);
    area->root_table = x86_current_root_table();
    return area;
}

/* 两种映射共用提交和回滚事务。借用页不转移所有权；仅新分配且尚未映射的页
 * 在本层释放，已映射的前缀统一交给区间清理处理。调用方持有 vmalloc 锁。 */
static kstatus_t vmalloc_populate_area_locked(vmalloc_area_t *area,
                                              page_t *const *pages,
                                              enum x86_cache_mode cache_mode) {
    uint32_t data_pages = area->data_pages;
    uint32_t mapped = 0U;
    kstatus_t status = K_OK;
    while (mapped < data_pages) {
        page_t *page = area->owns_pages ? page_alloc(0, PAGE_ALLOC_ZERO) :
                                          pages[mapped];
        if (page == 0) {
            g_vmalloc_failure = 2U;
            status = K_ENOMEM;
            break;
        }
        paddr_t physical = page_to_phys(page);
        status = x86_map_page(area->root_table,
            area->address + (vaddr_t)mapped * PAGE_SIZE,
            physical, X86_PAGE_WRITE | X86_PAGE_GLOBAL, cache_mode);
        if (status != K_OK) {
            if (area->owns_pages) {
                g_vmalloc_failure = 0x300U | (uint32_t)(-status);
                page_free(page);
            }
            break;
        }
        ++mapped;
    }
    if (mapped != data_pages) {
        area->data_pages = mapped;
        (void)vmalloc_release_area_locked(area);
    }
    return status;
}

void *vmalloc(size_t size) {
    if (size == 0 || size > VMALLOC_MANAGED_SIZE - 2U * PAGE_SIZE) return 0;
    if (size > UINT64_MAX - (PAGE_SIZE - 1ULL)) return 0;
    uint32_t data_pages = (uint32_t)((size + PAGE_SIZE - 1ULL) >> PAGE_SHIFT);
    vmalloc_initialize();
    uint64_t irq_flags = vmalloc_lock_irqsave();
    vmalloc_area_t *area = vmalloc_reserve_area_locked(data_pages, true);
    void *result = 0;
    if (area != 0 && vmalloc_populate_area_locked(area, 0, X86_CACHE_WB) == K_OK)
        result = (void *)(uintptr_t)area->address;
    vmalloc_unlock_irqrestore(irq_flags);
    return result;
}

uint32_t vmalloc_last_failure(void) {
    return g_vmalloc_failure;
}

void vfree(void *pointer) {
    if (pointer == 0) return;
    vmalloc_initialize();
    uint64_t irq_flags = vmalloc_lock_irqsave();
    vmalloc_area_t *area = 0;
    for (uint32_t i = 0; i < g_vmalloc_area_limit; ++i) {
        if (g_vmalloc_areas[i].used &&
            g_vmalloc_areas[i].address == (vaddr_t)(uintptr_t)pointer) {
            area = &g_vmalloc_areas[i];
            break;
        }
    }
    if (area == 0) {
        vmalloc_unlock_irqrestore(irq_flags);
        return;
    }
    (void)vmalloc_release_area_locked(area);
    vmalloc_unlock_irqrestore(irq_flags);
}

kstatus_t vmalloc_map_pages(page_t *const *pages, uint32_t page_count,
                            uint32_t cache_mode, void **out) {
    if (pages == 0 || page_count == 0U || out == 0 ||
        cache_mode > X86_CACHE_UC || page_count > VMALLOC_PAGE_COUNT - 2U) {
        return K_EINVAL;
    }
    for (uint32_t index = 0U; index < page_count; ++index) {
        if (pages[index] == 0 || (pages[index]->flags & PAGE_FREE) != 0U ||
            page_to_phys(pages[index]).value == UINT64_MAX) {
            return K_EINVAL;
        }
    }

    vmalloc_initialize();
    uint64_t irq_flags = vmalloc_lock_irqsave();
    vmalloc_area_t *area = vmalloc_reserve_area_locked(page_count, false);
    if (area == 0) {
        vmalloc_unlock_irqrestore(irq_flags);
        return K_ENOMEM;
    }

    kstatus_t status = vmalloc_populate_area_locked(
        area, pages, (enum x86_cache_mode)cache_mode);
    if (status == K_OK) *out = (void *)(uintptr_t)area->address;
    vmalloc_unlock_irqrestore(irq_flags);
    return status;
}

bool vmalloc_self_test(void) {
    size_t size = PAGE_SIZE * 3U + 17U;
    uint8_t *memory = (uint8_t *)vmalloc(size);
    if (memory == 0 || ((uintptr_t)memory & (PAGE_SIZE - 1ULL)) != 0) return false;
    memory[0] = 0x11U;
    memory[PAGE_SIZE] = 0x22U;
    memory[PAGE_SIZE * 3U] = 0x33U;
    memory[size - 1U] = 0x44U;
    paddr_t root = x86_current_root_table();
    paddr_t physical;
    if (x86_translate_page(root, (vaddr_t)(uintptr_t)memory, &physical, 0) != K_OK ||
        x86_translate_page(root, (vaddr_t)(uintptr_t)memory - PAGE_SIZE, &physical, 0) != K_ENOENT ||
        x86_translate_page(root, (vaddr_t)(uintptr_t)memory + 4U * PAGE_SIZE,
                           &physical, 0) != K_ENOENT) {
        vfree(memory);
        return false;
    }
    vaddr_t old_address = (vaddr_t)(uintptr_t)memory;
    vfree(memory);
    return x86_translate_page(root, old_address, &physical, 0) == K_ENOENT;
}

/*
 * 验证 TLB 失效与物理页复用的顺序：撤销旧映射并释放旧页后，选择一个不同的
 * 物理页映射回同一虚拟地址。若远端 CPU 仍保留旧 TLB，读取会观察到旧内容。
 */
bool vmalloc_tlb_reuse_self_test(void) {
    paddr_t root = x86_current_root_table();
    for (uint32_t round = 0; round < 32U; ++round) {
        uint8_t *memory = (uint8_t *)vmalloc(PAGE_SIZE);
        if (memory == 0 || ((uintptr_t)memory & (PAGE_SIZE - 1ULL)) != 0) return false;
        vaddr_t address = (vaddr_t)(uintptr_t)memory;
        paddr_t old_physical;
        if (x86_translate_page(root, address, &old_physical, 0) != K_OK) {
            vfree(memory);
            return false;
        }
        *memory = (uint8_t)(0x30U + (round & 0x1FU));
        vfree(memory);
        if (x86_translate_page(root, address, &old_physical, 0) != K_ENOENT) return false;

        page_t *held[16] = {0};
        uint32_t held_count = 0;
        page_t *replacement = 0;
        for (; held_count < 16U; ++held_count) {
            page_t *candidate = page_alloc(0, PAGE_ALLOC_ZERO);
            if (candidate == 0) break;
            held[held_count] = candidate;
            if (page_to_phys(candidate).value != old_physical.value) {
                replacement = candidate;
                ++held_count;
                break;
            }
        }
        if (replacement == 0) {
            for (uint32_t i = 0; i < held_count; ++i) page_free(held[i]);
            return false;
        }
        for (uint32_t i = 0; i < held_count; ++i) {
            if (held[i] != replacement) page_free(held[i]);
        }

        paddr_t replacement_physical = page_to_phys(replacement);
        uint8_t *replacement_memory = (uint8_t *)phys_to_direct(replacement_physical);
        if (replacement_memory == 0 ||
            x86_map_page(root, address, replacement_physical,
                         X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_WB) != K_OK) {
            page_free(replacement);
            return false;
        }
        replacement_memory[0] = (uint8_t)(0xA0U + (round & 0x1FU));
        volatile uint8_t *probe = (volatile uint8_t *)(uintptr_t)address;
        paddr_t translated;
        bool valid = *probe == replacement_memory[0] &&
                     x86_translate_page(root, address, &translated, 0) == K_OK &&
                     translated.value == replacement_physical.value;
        paddr_t unmapped;
        if (x86_unmap_page(root, address, &unmapped) != K_OK ||
            unmapped.value != replacement_physical.value) valid = false;
        page_free(replacement);
        if (!valid) return false;
    }
    return true;
}
