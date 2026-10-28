#pragma once

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/list.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

/* REFACTOR_P2_PHYS_INTERNAL: private contract for the physical MM Owners. */

#define PAGE_DB_MAX_SECTIONS 65536U
#define PAGE_DB_MAX_RESERVED 32U
#define PAGE_DB_MAX_PHYSICAL \
    (X86_64_DIRECT_MAP_END - X86_64_DIRECT_MAP_BASE + 1ULL)

/* 64 个等长槽位作为一个 Buddy 块驻留。 */
#define PAGE_POOL_GROUP_SHIFT       6U
#define PAGE_POOL_GROUP_SLOTS       64U
/* 仅缓存单页。多页请求直接交给 Buddy，避免空闲的高阶槽位使 RAM 滞留在部分占用的 CPU 池中。 */
#define PAGE_POOL_SMALL_MAX_ORDER   0U
#define PAGE_POOL_ORDER_COUNT       (PAGE_POOL_SMALL_MAX_ORDER + 1U)
#define PAGE_POOL_GROUP_NONE        UINT32_MAX
#define PAGE_POOL_GROUP_PARTIAL     1U

/* 缓存的 0 阶页面此前可能用作页表、文件页或 slab。所有权和 private_data 属于该页面原有生命周期，
 * 而不属于内存池。特别是 DMA 将 private_data 用作固定计数，初始值为零。 */
static inline void pool_prepare_allocated_page(page_t *page, uint8_t order) {
    page->flags = PAGE_COMPOUND_HEAD;
    page->owner = PAGE_OWNER_NONE;
    page->order = order;
    page->private_data = 0U;
    page->u.compound.head = page;
    page->u.compound.index = 0U;
    atomic_store_explicit(&page->mapcount, -1, memory_order_relaxed);
    atomic_store_explicit(&page->refs, 1U, memory_order_relaxed);
}

#if defined(__GNUC__) || defined(__clang__)
#define PAGE_HOT_NOINLINE __attribute__((noinline, hot, aligned(64)))
#define PAGE_NOINLINE     __attribute__((noinline))
#define PAGE_LIKELY(x)    __builtin_expect(!!(x), 1)
#define PAGE_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#else
#define PAGE_HOT_NOINLINE
#define PAGE_NOINLINE
#define PAGE_LIKELY(x)   (x)
#define PAGE_UNLIKELY(x) (x)
#endif

typedef struct {
    uint64_t start;
    uint64_t end;
} reserved_range_t;

typedef struct {
    uint64_t base_pfn;
    uint64_t free_mask;                 /* 页面驻留池中时仅由所属 CPU 访问 */
    atomic_uint_fast64_t remote_mask;   /* 跨 CPU 释放 */
    uint32_t partial_next;              /* page_pool_group_t 索引 */
    uint32_t remote_next;               /* page_pool_group_t 索引 */
    uint16_t owner_cpu;
    uint8_t pool_order;
    uint8_t zone;
    uint8_t flags;
    uint8_t reserved[3];
} page_pool_group_t;

_Static_assert(sizeof(page_pool_group_t) == 40U,
               "Step5 group metadata must remain compact");

typedef struct {
    uint64_t group_base_pfn;
    uint32_t group_first;
    uint32_t group_count;
} page_pool_section_t;

typedef struct {
    page_t *base;
    uint64_t free_mask;
    uint32_t partial_head;
    uint32_t contiguous_pages; /* 安装基址时缓存的连续元数据范围 */
} page_local_pool_t;

typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    page_local_pool_t normal;
    page_local_pool_t dma32;
} page_o0_cache_t;

_Static_assert(sizeof(page_o0_cache_t) == CACHELINE_SIZE,
               "order0 Step5 cache must fit one cache line");

typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    atomic_uint head[PAGE_ZONE_COUNT][PAGE_POOL_SMALL_MAX_ORDER + 1U];
} page_remote_heads_t;

_Static_assert(sizeof(page_remote_heads_t) == CACHELINE_SIZE,
               "remote Step5 inbox must be cache-line isolated");

/* 物理页状态仅在 page_db.c 中定义一次。 */
extern page_section_t g_sections[PAGE_DB_MAX_SECTIONS];
extern page_pool_section_t g_pool_sections[PAGE_DB_MAX_SECTIONS];
extern uint32_t g_section_count;
extern bool g_pool_sections_ordered;
extern list_head_t g_free_lists[PAGE_ZONE_COUNT][BUDDY_MAX_ORDER + 1U];
extern spinlock_t g_page_lock;
extern reserved_range_t g_reserved[PAGE_DB_MAX_RESERVED];
extern uint32_t g_reserved_count;
extern bool g_initialized;

/* 每 CPU 内存池状态仅在 percpu_page.c 中定义一次。 */
extern page_pool_group_t *g_pool_groups;
extern uint32_t g_pool_base_group_count;
extern uint32_t g_pool_group_count;
extern page_o0_cache_t g_o0_cache[MAX_CPUS]
    __attribute__((aligned(CACHELINE_SIZE)));
extern page_remote_heads_t g_remote_heads[MAX_CPUS]
    __attribute__((aligned(CACHELINE_SIZE)));

/* 小型原语保持内联，避免拆分文件后增加热路径调用。 */
static inline void page_memory_zero(void *address, size_t size) {
    /* 复用不触碰 SIMD 状态的批量清零；逐字节循环会拖慢大量匿名页缺页。 */
    memset(address, 0, size);
}

static inline uint64_t page_pool_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void page_pool_irq_restore(uint64_t flags) {
    if ((flags & (1ULL << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    } else {
        __asm__ volatile ("cli" : : : "memory");
    }
}

static inline uint64_t align_down_group(uint64_t pfn) {
    return pfn & ~(uint64_t)(PAGE_POOL_GROUP_SLOTS - 1U);
}

static inline uint64_t align_up_group(uint64_t pfn) {
    return (pfn + PAGE_POOL_GROUP_SLOTS - 1U) &
           ~(uint64_t)(PAGE_POOL_GROUP_SLOTS - 1U);
}

static inline void lock_pages(void) {
    /* 其他 CPU 可能正在等待，此时要让 Buddy 所有权固定在当前 CPU。 */
    sched_preempt_disable();
    spinlock_lock(&g_page_lock);
}

static inline void unlock_pages(void) {
    spinlock_unlock(&g_page_lock);
    sched_preempt_enable();
}

/* page_db.c：元数据和 Buddy 所有权。 */
page_t *page_for_pfn(pfn_t pfn);
/* 将 *count 限制为单一区段中的连续元数据范围；遇到空洞时返回 NULL。 */
page_t *page_span_for_pfn(pfn_t pfn, uint64_t *count);
page_t *page_for_address(uint64_t physical);
void mark_compound(page_t *head, uint8_t order, bool free_page);
void insert_block(uint64_t physical, uint8_t order);
page_t *take_from_zone_locked(uint8_t zone, uint8_t order);
void free_block_locked(page_t *head, uint8_t order);
void page_stats_note_alloc(uint8_t order);
void page_stats_note_free(uint8_t order);

/* percpu_page.c：池化分配和公开的 0 阶快速路径。 */
page_t *pool_alloc_small(uint8_t zone, uint8_t order, uint32_t cpu);
/* 调用方必须先屏蔽 IRQ 并固定当前 CPU，再获取内存池保护。持有 Buddy 保护时绝不能获取池保护，
 * 也不得同时持有两个池保护。 */
void pool_cpu_lock(uint32_t cpu);
void pool_cpu_unlock(uint32_t cpu);
void pool_drain_cpu(uint32_t cpu);
void pool_reclaim_all(void);
page_t *pool_alloc_remote(uint8_t order, page_alloc_flags_t flags, uint32_t requester);
bool pool_free_small(page_t *page, uint8_t order, uint32_t cpu);
void pool_init_cpu_state(void);

/* page_alloc.c：冷/慢速阶数分配和旧式 Buddy 回退路径。 */
page_t *page_alloc_slow(uint8_t order, page_alloc_flags_t flags);
void free_legacy_slow(page_t *head, uint8_t order);
void page_free_non_o0(page_t *head, uint8_t order);

