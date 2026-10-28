#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/debug_stage.h>
#include <arch/x86_64/cpu.h>

#define KMEM_MAGIC             0x4B4D454D48445231ULL
#define KMEM_SLAB_MAGIC        0x534C414250414731ULL
#define KMEM_COOKIE            0x9E3779B97F4A7C15ULL
#define KMEM_FRONT_SIZE        16U
#define KMEM_TRAILER_SIZE      8U
#define KMEM_PAYLOAD_OFFSET    48U
#define KMEM_CLASS_LARGE       0xFFFFU
#define KMEM_STATE_FREE        0x39U
#define KMEM_STATE_ALLOCATED   0xA7U
#define KMEM_POISON_FREE       0xA5U
#define KMEM_POISON_ALLOC      0xCCU
#define KMEM_REDZONE_BYTE      0xD3U
#define KMEM_SLAB_HEADER_SIZE  64U
#define KMEM_CLASS_COUNT       10U
#define KMEM_MAGAZINE_CAPACITY 16U
#define KMEM_MAGAZINE_FLUSH    8U
#define KMEM_STRESS_SLOT_COUNT 48U
#define KMEM_STRESS_OPERATIONS 2048U
#define KMEM_STRESS_MAX_SIZE    4096U
#define KMEM_CACHE_CHAIN_TEST_ALLOCATIONS 128U

typedef struct allocation_header {
    uint64_t magic;
    uintptr_t owner;
    uint32_t requested;
    uint16_t class_index;
    uint8_t state;
    uint8_t flags;
    uint64_t cookie;
} allocation_header_t;

typedef struct slab_page {
    uint64_t magic;
    struct slab_page *next;
    page_t *page;
    void *free_list;
    uint16_t slot_count;
    uint16_t free_count;
    uint16_t class_index;
    uint16_t reserved;
    struct slab_page **prev_link;
} slab_page_t;

typedef struct {
    spinlock_t lock;
    /* 仅包含至少有一个全局可用槽位的 slab。 */
    slab_page_t *slabs;
    uint16_t slot_size;
    uint16_t slot_count; /* 分配器初始化后不可变 */
} kmem_cache_t;

/* 每个 CPU 的小对象缓存；缓存对象已从全局 slab 链表摘除。 */
typedef struct {
    void *head;
    uint16_t count;
    uint16_t reserved;
} kmem_magazine_t;

/* 将由不同 CPU 独立写入的分配器状态放在不同缓存行。 */
typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    kmem_magazine_t classes[KMEM_CLASS_COUNT];
} kmem_cpu_magazines_t;

typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    atomic_uint_fast64_t fastpath_hits;
    atomic_uint_fast64_t fastpath_refills;
} kmem_cpu_stats_t;

static const uint16_t g_slot_sizes[KMEM_CLASS_COUNT] = {
    96U, 128U, 192U, 256U, 384U, 512U, 768U, 1024U, 1536U, 2048U,
};
static kmem_cache_t g_caches[KMEM_CLASS_COUNT];
static kmem_cpu_magazines_t g_magazines[MAX_CPUS];
static kmem_cpu_stats_t g_cpu_stats[MAX_CPUS];
static atomic_uint g_init_state;
static atomic_uint_fast64_t g_error_count;
static volatile uint32_t g_cache_owner[KMEM_CLASS_COUNT];
static volatile uint32_t g_cache_waiting[MAX_CPUS];
static volatile uint32_t g_cache_class[MAX_CPUS];
static volatile uint32_t g_kmem_progress[MAX_CPUS];

_Static_assert(sizeof(allocation_header_t) == 32U, "kmalloc header layout");
_Static_assert(sizeof(slab_page_t) <= KMEM_SLAB_HEADER_SIZE, "slab page header layout");

static void bytes_fill(void *memory, uint8_t value, size_t size) {
    /* 保留毒化/红区语义，但分配器禁用抢占期间不要逐字节填充大型分配。 */
    __builtin_memset(memory, value, size);
}

static bool bytes_equal(const void *memory, uint8_t value, size_t size) {
    const uint8_t *bytes = (const uint8_t *)memory;
    uint64_t repeated = UINT64_C(0x0101010101010101) * value;
    while (size >= sizeof(repeated)) {
        uint64_t word;
        /* 显式使用内建复制可避免对齐/别名假设；独立运行的 x86 构建会生成整数加载，而非 SIMD 指令。 */
        __builtin_memcpy(&word, bytes, sizeof(word));
        if (word != repeated) return false;
        bytes += sizeof(word);
        size -= sizeof(word);
    }
    while (size-- != 0) {
        if (*bytes++ != value) return false;
    }
    return true;
}

static void cache_lock(kmem_cache_t *cache) {
    /* 补充缓存时可能分配 slab，因此持锁时间可能长到跨越一次定时器节拍。缓存状态发布前必须让锁所有者
     * 留在当前 CPU；否则如果所有者之后未再次获选，其他 CPU 可能永久自旋。 */
    sched_preempt_disable();
    uint32_t cpu_index = x86_current_cpu_index();
    uint32_t class_index = cache >= g_caches &&
                           cache < g_caches + KMEM_CLASS_COUNT ?
                           (uint32_t)(cache - g_caches) : UINT32_MAX;
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_cache_class[cpu_index], class_index,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&g_cache_waiting[cpu_index], 1U,
                         __ATOMIC_RELEASE);
    }
    spinlock_lock(&cache->lock);
    if (class_index < KMEM_CLASS_COUNT) {
        __atomic_store_n(&g_cache_owner[class_index], cpu_index,
                         __ATOMIC_RELEASE);
    }
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_cache_waiting[cpu_index], 0U,
                         __ATOMIC_RELEASE);
    }
}

static void cache_unlock(kmem_cache_t *cache) {
    uint32_t class_index = cache >= g_caches &&
                           cache < g_caches + KMEM_CLASS_COUNT ?
                           (uint32_t)(cache - g_caches) : UINT32_MAX;
    /* 释放所有权前清除诊断信息：后继者可能在解锁后立即发布自己的 CPU，不能再被当前线程清除。 */
    if (class_index < KMEM_CLASS_COUNT) {
        __atomic_store_n(&g_cache_owner[class_index], UINT32_MAX,
                         __ATOMIC_RELEASE);
    }
    spinlock_unlock(&cache->lock);
    sched_preempt_enable();
}

uint32_t kmem_debug_cache_waiting(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_cache_waiting[cpu_index], __ATOMIC_ACQUIRE);
}

uint32_t kmem_debug_progress(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_kmem_progress[cpu_index], __ATOMIC_ACQUIRE);
}

uint32_t kmem_debug_cache_class(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return UINT32_MAX;
    return __atomic_load_n(&g_cache_class[cpu_index], __ATOMIC_ACQUIRE);
}

uint32_t kmem_debug_cache_owner(uint32_t class_index) {
    if (class_index >= KMEM_CLASS_COUNT) return UINT32_MAX;
    return __atomic_load_n(&g_cache_owner[class_index], __ATOMIC_ACQUIRE);
}

uint32_t kmem_debug_cache_state(uint32_t class_index) {
    if (class_index >= KMEM_CLASS_COUNT) return 0U;
    return atomic_load_explicit(&g_caches[class_index].lock.state,
                                memory_order_acquire);
}

static void record_error(void) {
    atomic_fetch_add_explicit(&g_error_count, 1U, memory_order_relaxed);
}

static void initialize_allocator(void) {
    if (atomic_load_explicit(&g_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    sched_preempt_disable();
    if (atomic_compare_exchange_strong_explicit(&g_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_error_count, 0U);
        for (uint16_t i = 0; i < KMEM_CLASS_COUNT; ++i) {
            atomic_init(&g_caches[i].lock.state, 0U);
            __atomic_store_n(&g_cache_owner[i], UINT32_MAX, __ATOMIC_RELAXED);
            g_caches[i].slabs = 0;
            g_caches[i].slot_size = g_slot_sizes[i];
            g_caches[i].slot_count = (uint16_t)((PAGE_SIZE -
                KMEM_SLAB_HEADER_SIZE) / g_slot_sizes[i]);
        }
        for (uint32_t cpu = 0; cpu < MAX_CPUS; ++cpu) {
            for (uint16_t i = 0; i < KMEM_CLASS_COUNT; ++i) {
                g_magazines[cpu].classes[i].head = 0;
                g_magazines[cpu].classes[i].count = 0;
                g_magazines[cpu].classes[i].reserved = 0;
            }
            atomic_init(&g_cpu_stats[cpu].fastpath_hits, 0U);
            atomic_init(&g_cpu_stats[cpu].fastpath_refills, 0U);
        }
        atomic_store_explicit(&g_init_state, 2U, memory_order_release);
        sched_preempt_enable();
        return;
    }
    sched_preempt_enable();
    while (atomic_load_explicit(&g_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static uint16_t class_index_for_size(size_t size) {
    if (size <= (size_t)g_slot_sizes[0] - KMEM_PAYLOAD_OFFSET - KMEM_TRAILER_SIZE)
        return 0U;
    if (size > (size_t)g_slot_sizes[KMEM_CLASS_COUNT - 1U] -
                   KMEM_PAYLOAD_OFFSET - KMEM_TRAILER_SIZE)
        return KMEM_CLASS_COUNT;
    /* 槽位大小依次为 3/4 次幂和完整的 2 次幂：96、128、……、1536、2048。
     * 上方的边界保证加法安全，且 clz 的操作数非零。 */
    uint32_t slot = (uint32_t)size + KMEM_PAYLOAD_OFFSET + KMEM_TRAILER_SIZE;
    unsigned exponent = 32U - (unsigned)__builtin_clz(slot - 1U);
    uint32_t three_quarters = 3U << (exponent - 2U);
    return (uint16_t)(2U * (exponent - 7U) + (slot > three_quarters));
}

static size_t class_capacity(uint16_t class_index) {
    return (size_t)g_slot_sizes[class_index] - KMEM_PAYLOAD_OFFSET - KMEM_TRAILER_SIZE;
}

static void *header_payload(allocation_header_t *header) {
    return (uint8_t *)header + KMEM_PAYLOAD_OFFSET;
}

static allocation_header_t *payload_header(void *payload) {
    return (allocation_header_t *)((uint8_t *)payload - KMEM_PAYLOAD_OFFSET);
}

static kmem_magazine_t *current_magazine(uint16_t class_index, uint32_t cpu) {
    if (class_index >= KMEM_CLASS_COUNT) return 0;
    if (cpu >= MAX_CPUS) return 0;
    return &g_magazines[cpu].classes[class_index];
}

static void record_fastpath_hit(uint32_t cpu) {
    if (cpu < MAX_CPUS) {
        atomic_fetch_add_explicit(&g_cpu_stats[cpu].fastpath_hits, 1U,
                                  memory_order_relaxed);
    }
}

static void record_fastpath_refill(void) {
    uint32_t cpu = x86_current_cpu_index();
    if (cpu < MAX_CPUS) {
        atomic_fetch_add_explicit(&g_cpu_stats[cpu].fastpath_refills, 1U,
                                  memory_order_relaxed);
    }
}

static uint64_t allocation_cookie(const allocation_header_t *header, const void *payload) {
    return KMEM_COOKIE ^ (uint64_t)(uintptr_t)payload ^ header->requested;
}

static void write_trailer(void *payload, uint32_t requested, uint64_t cookie) {
    _Static_assert(KMEM_TRAILER_SIZE == sizeof(cookie), "allocation trailer width");
    _Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "allocation trailer encoding");
    /* 尾部数据紧跟任意字节长度的请求，因此使用内建复制，而非对齐的 uint64_t 指针解引用。 */
    __builtin_memcpy((uint8_t *)payload + requested, &cookie, sizeof(cookie));
}

static bool trailer_valid(const void *payload, uint32_t requested, uint64_t cookie) {
    uint64_t stored;
    __builtin_memcpy(&stored, (const uint8_t *)payload + requested, sizeof(stored));
    return stored == cookie;
}

static bool header_redzones_valid(const allocation_header_t *header, const void *payload) {
    const uint8_t *front = (const uint8_t *)header + sizeof(*header);
    return bytes_equal(front, KMEM_REDZONE_BYTE, KMEM_FRONT_SIZE) &&
           header->cookie == allocation_cookie(header, payload) &&
           trailer_valid(payload, header->requested, header->cookie);
}

static slab_page_t *create_slab(uint16_t class_index) {
    /* 下方会初始化头部、保护区和负载毒化值。持有缓存锁期间避免重复清零整页。 */
    page_t *page = page_alloc(0, 0);
    if (page == 0) return 0;
    page->owner = PAGE_OWNER_SLAB;
    uint8_t *base = (uint8_t *)phys_to_direct(page_to_phys(page));
    if (base == 0) {
        page_free(page);
        return 0;
    }
    slab_page_t *slab = (slab_page_t *)base;
    slab->magic = KMEM_SLAB_MAGIC;
    slab->next = 0;
    slab->prev_link = 0;
    slab->page = page;
    slab->free_list = 0;
    slab->class_index = class_index;
    slab->reserved = 0;
    slab->slot_count = g_caches[class_index].slot_count;
    slab->free_count = slab->slot_count;

    size_t capacity = class_capacity(class_index);
    for (uint16_t i = 0; i < slab->slot_count; ++i) {
        allocation_header_t *header = (allocation_header_t *)(base + KMEM_SLAB_HEADER_SIZE +
                                      (size_t)i * g_slot_sizes[class_index]);
        header->magic = KMEM_MAGIC;
        header->owner = (uintptr_t)slab;
        header->requested = 0;
        header->class_index = class_index;
        header->state = KMEM_STATE_FREE;
        header->flags = 0;
        header->cookie = 0;
        bytes_fill((uint8_t *)header + sizeof(*header), KMEM_REDZONE_BYTE,
                   KMEM_FRONT_SIZE);
        void *payload = header_payload(header);
        /* 空闲链表链接占用首字；激活时只检查其后的毒化值。不要填充随后会立即被覆盖的字节。 */
        bytes_fill((uint8_t *)payload + sizeof(void *), KMEM_POISON_FREE,
                   capacity - sizeof(void *));
        *(void **)payload = slab->free_list;
        slab->free_list = payload;
    }
    return slab;
}

/* 所有部分 slab 链表操作都必须持有 cache->lock。 */
static slab_page_t *cache_partial_head(kmem_cache_t *cache) {
    while (cache->slabs != 0 && cache->slabs->free_list == 0) {
        slab_page_t *empty = cache->slabs;
        cache->slabs = empty->next;
        if (cache->slabs != 0) cache->slabs->prev_link = &cache->slabs;
        empty->next = 0;
        empty->prev_link = 0;
        record_error();
    }
    return cache->slabs;
}

static void cache_partial_insert(kmem_cache_t *cache, slab_page_t *slab) {
    if (slab == 0 || slab->free_list == 0) {
        record_error();
        return;
    }
    slab->next = cache->slabs;
    slab->prev_link = &cache->slabs;
    if (slab->next != 0) slab->next->prev_link = &slab->next;
    cache->slabs = slab;
}

static void cache_partial_remove_head(kmem_cache_t *cache,
                                      slab_page_t *slab) {
    if (slab == 0 || cache->slabs != slab || slab->free_list != 0) {
        record_error();
        return;
    }
    cache->slabs = slab->next;
    if (cache->slabs != 0) cache->slabs->prev_link = &cache->slabs;
    slab->next = 0;
    slab->prev_link = 0;
}

/* 所有槽位均为全局空闲，意味着它们不再位于 CPU 缓存，也不属于任何活动分配。释放 slab 前先移除
 * 部分 slab 链表链接。 */
static void cache_release_empty(kmem_cache_t *cache, slab_page_t *slab) {
    if (slab == 0 || slab->free_count != slab->slot_count) return;
    (void)cache;
    slab_page_t **link = slab->prev_link;
    if (link == 0 || *link != slab) {
        record_error();
        return;
    }
    *link = slab->next;
    if (slab->next != 0) slab->next->prev_link = link;
    slab->next = 0;
    slab->prev_link = 0;
    page_t *page = slab->page;
    slab->magic = 0U;
    page_free(page);
}

/* 持有 cache->lock 时返回已验证的空闲负载。此操作后调用方不得再访问该 slab，因为释放最后一个槽位
 * 可能会释放整个 slab。 */
static void cache_put_payload_locked(kmem_cache_t *cache, slab_page_t *slab,
                                     void *payload) {
    bool was_empty = slab->free_list == 0;
    *(void **)payload = slab->free_list;
    slab->free_list = payload;
    ++slab->free_count;
    if (was_empty) cache_partial_insert(cache, slab);
    cache_release_empty(cache, slab);
}

/* 调用方持有 cache->lock；两条分配路径共用槽位所有权。 */
static void *cache_take_payload_locked(kmem_cache_t *cache, uint16_t class_index) {
    slab_page_t *slab = cache_partial_head(cache);
    if (slab == 0) {
        slab = create_slab(class_index);
        if (slab == 0) return 0;
        cache_partial_insert(cache, slab);
    }
    void *payload = slab->free_list;
    slab->free_list = *(void **)payload;
    --slab->free_count;
    if (slab->free_list == 0) cache_partial_remove_head(cache, slab);
    return payload;
}

static bool magazine_refill(uint16_t class_index, kmem_magazine_t *magazine) {
    kmem_cache_t *cache;
    uint16_t moved = 0;

    if (magazine == 0 || class_index >= KMEM_CLASS_COUNT) return false;
    cache = &g_caches[class_index];
    cache_lock(cache);
    while (moved < KMEM_MAGAZINE_CAPACITY) {
        void *payload = cache_take_payload_locked(cache, class_index);
        if (payload == 0) break;
        *(void **)payload = magazine->head;
        magazine->head = payload;
        ++magazine->count;
        ++moved;
    }
    cache_unlock(cache);
    if (moved != 0) {
        record_fastpath_refill();
    }
    return moved != 0;
}

static void magazine_flush(uint16_t class_index, kmem_magazine_t *magazine) {
    kmem_cache_t *cache;

    if (magazine == 0 || class_index >= KMEM_CLASS_COUNT ||
        magazine->count <= KMEM_MAGAZINE_CAPACITY) return;
    cache = &g_caches[class_index];
    cache_lock(cache);
    while (magazine->count > KMEM_MAGAZINE_FLUSH) {
        void *payload = magazine->head;
        allocation_header_t *header;
        slab_page_t *slab;

        if (payload == 0) break;
        magazine->head = *(void **)payload;
        --magazine->count;
        header = payload_header(payload);
        slab = (slab_page_t *)(uintptr_t)header->owner;
        if (header->magic != KMEM_MAGIC || header->class_index != class_index ||
            header->state != KMEM_STATE_FREE || slab == 0 ||
            slab->magic != KMEM_SLAB_MAGIC || slab->class_index != class_index) {
            record_error();
            continue;
        }
        cache_put_payload_locked(cache, slab, payload);
    }
    cache_unlock(cache);
}

static void *activate_payload(uint16_t class_index, void *payload,
                              size_t requested, uint32_t flags, bool zero) {
    allocation_header_t *header;
    size_t capacity;

    if (payload == 0 || class_index >= KMEM_CLASS_COUNT) return 0;
    header = payload_header(payload);
    capacity = class_capacity(class_index);
    if (header->magic != KMEM_MAGIC || header->state != KMEM_STATE_FREE ||
        !bytes_equal((uint8_t *)payload + sizeof(void *), KMEM_POISON_FREE,
                     capacity - sizeof(void *))) {
        record_error();
    }
    header->requested = (uint32_t)requested;
    header->state = KMEM_STATE_ALLOCATED;
    header->flags = (uint8_t)flags;
    if (zero) {
        bytes_fill(payload, 0, requested);
        bytes_fill((uint8_t *)payload + requested, KMEM_POISON_ALLOC,
                   capacity - requested);
    } else {
        bytes_fill(payload, KMEM_POISON_ALLOC, capacity);
    }
    bytes_fill((uint8_t *)header + sizeof(*header), KMEM_REDZONE_BYTE, KMEM_FRONT_SIZE);
    header->cookie = allocation_cookie(header, payload);
    write_trailer(payload, header->requested, header->cookie);
    return payload;
}

static void *allocate_from_cache(uint16_t class_index, size_t requested,
                                 uint32_t flags, bool zero) {
    kmem_cache_t *cache = &g_caches[class_index];
    cache_lock(cache);
    void *payload = cache_take_payload_locked(cache, class_index);
    cache_unlock(cache);
    if (payload == 0) return 0;
    return activate_payload(class_index, payload, requested, flags, zero);
}

static void *allocate_large(size_t requested, uint32_t flags, bool zero) {
    if (requested > UINT32_MAX || requested > UINT64_MAX -
        (KMEM_PAYLOAD_OFFSET + KMEM_TRAILER_SIZE)) return 0;
    uint64_t needed = requested + KMEM_PAYLOAD_OFFSET + KMEM_TRAILER_SIZE;
    /* ceil(log2(needed)) 可得到覆盖需求的最小二次幂。单页情况也可避免 clz(0)；请求有界，因此阶数
     * 始终有效。 */
    uint8_t order = needed <= PAGE_SIZE ? 0U :
        (uint8_t)(64U - (unsigned)__builtin_clzll(needed - 1U) - PAGE_SHIFT);
    if (order > BUDDY_MAX_ORDER) return 0;
    page_t *page = page_alloc(order, 0);
    if (page == 0) return 0;
    page->owner = PAGE_OWNER_SLAB;
    allocation_header_t *header = (allocation_header_t *)phys_to_direct(page_to_phys(page));
    if (header == 0) {
        page_free(page);
        return 0;
    }
    header->magic = KMEM_MAGIC;
    header->owner = (uintptr_t)page;
    header->requested = (uint32_t)requested;
    header->class_index = KMEM_CLASS_LARGE;
    header->state = KMEM_STATE_ALLOCATED;
    header->flags = (uint8_t)flags;
    bytes_fill((uint8_t *)header + sizeof(*header), KMEM_REDZONE_BYTE, KMEM_FRONT_SIZE);
    void *payload = header_payload(header);
    bytes_fill(payload, zero ? 0 : KMEM_POISON_ALLOC, requested);
    header->cookie = allocation_cookie(header, payload);
    write_trailer(payload, header->requested, header->cookie);
    return payload;
}

/* 激活前先选择初始化方式：kzalloc 不应先毒化再覆盖相同的请求字节。保留未使用空间的毒化值和所有保护区。 */
static void *kmalloc_internal(size_t size, uint32_t flags, bool zero) {
    void *result = 0;
    bool large = false;
    if (size == 0) return 0;
    /* 固定到 CPU 后只读取一次 CPU 编号；此次分配期间，缓存、命中统计和诊断都使用该值。 */
    sched_preempt_disable();
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_kmem_progress[cpu_index], 1U,
                         __ATOMIC_RELEASE);
    }
    initialize_allocator();
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_kmem_progress[cpu_index], 2U,
                         __ATOMIC_RELEASE);
    }
    uint16_t i = class_index_for_size(size);
    if (i < KMEM_CLASS_COUNT) {
        kmem_magazine_t *magazine = current_magazine(i, cpu_index);
        if (magazine != 0) {
            if (cpu_index < MAX_CPUS) {
                __atomic_store_n(&g_kmem_progress[cpu_index], 20U + i,
                                 __ATOMIC_RELEASE);
            }
            if (magazine->head == 0) (void)magazine_refill(i, magazine);
            if (cpu_index < MAX_CPUS) {
                __atomic_store_n(&g_kmem_progress[cpu_index], 30U + i,
                                 __ATOMIC_RELEASE);
            }
            if (magazine->head != 0) {
                void *payload = magazine->head;
                magazine->head = *(void **)payload;
                --magazine->count;
                record_fastpath_hit(cpu_index);
                result = activate_payload(i, payload, size, flags, zero);
                goto done;
            }
        }
        if (cpu_index < MAX_CPUS) {
            __atomic_store_n(&g_kmem_progress[cpu_index], 40U + i,
                             __ATOMIC_RELEASE);
        }
        result = allocate_from_cache(i, size, flags, zero);
        goto done;
    }
    /* 大型分配独占页面，不使用 CPU 本地缓存。page_alloc 会管理自身保护区；填充/清零数 MB 负载时，
     * 不要继续固定当前 CPU。先完成 CPU 本地诊断。 */
    large = true;
done:
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_kmem_progress[cpu_index], 90U,
                         __ATOMIC_RELEASE);
    }
    sched_preempt_enable();
    return large ? allocate_large(size, flags, zero) : result;
}

void *kmalloc(size_t size, uint32_t flags) {
    return kmalloc_internal(size, flags, false);
}

void *kmalloc_try_cached(size_t size, uint32_t flags) {
    if (size == 0U || atomic_load_explicit(&g_init_state,
                                          memory_order_acquire) != 2U)
        return 0;
    uint16_t index = class_index_for_size(size);
    if (index >= KMEM_CLASS_COUNT) return 0;
    kmem_cache_t *cache = &g_caches[index];
    /* 硬中断可能打断了此锁的持有者。不要自旋或调用 cache_lock()：尝试获取失败即视为分配失败。 */
    if (!spinlock_try_lock(&cache->lock)) return 0;
    slab_page_t *slab = cache_partial_head(cache);
    void *payload = 0;
    if (slab != 0) {
        payload = slab->free_list;
        slab->free_list = *(void **)payload;
        --slab->free_count;
        if (slab->free_list == 0) cache_partial_remove_head(cache, slab);
    }
    spinlock_unlock(&cache->lock);
    /* 已分离槽位会使 slab 保持有效。激活过程不会访问本地缓存或分配内存，因此可在缓存临界区之外安全执行。 */
    return activate_payload(index, payload, size, flags, false);
}

void *kzalloc(size_t size, uint32_t flags) {
    return kmalloc_internal(size, flags, true);
}

bool kmalloc_try_grow(void *pointer, size_t size) {
    if (pointer == 0 || size == 0U || size > UINT32_MAX) return false;
    allocation_header_t *header = payload_header(pointer);
    if (header->magic != KMEM_MAGIC ||
        header->class_index != KMEM_CLASS_LARGE ||
        header->state != KMEM_STATE_ALLOCATED) return false;
    page_t *page = (page_t *)header->owner;
    if (page == 0 || page->owner != PAGE_OWNER_SLAB ||
        page->order > BUDDY_MAX_ORDER) return false;
    size_t capacity = ((size_t)PAGE_SIZE << page->order) -
        KMEM_PAYLOAD_OFFSET - KMEM_TRAILER_SIZE;
    if (header->requested > capacity || size < header->requested ||
        size > capacity || !header_redzones_valid(header, pointer)) return false;
    /* 此分配不在本地缓存或共享空闲链表中。其所有者会串行化访问，因此无需固定 CPU、获取分配器锁或
     * 执行 TLB 操作。 */
    bytes_fill((uint8_t *)pointer + header->requested, KMEM_POISON_ALLOC,
               size - header->requested);
    header->requested = (uint32_t)size;
    header->cookie = allocation_cookie(header, pointer);
    write_trailer(pointer, header->requested, header->cookie);
    return true;
}

static void kfree_internal(void *pointer) {
    if (pointer == 0) return;
    allocation_header_t *header = payload_header(pointer);
    if (header->magic != KMEM_MAGIC) {
        record_error();
        return;
    }
    if (header->class_index == KMEM_CLASS_LARGE) {
        if (header->state != KMEM_STATE_ALLOCATED) {
            record_error();
            return;
        }
        if (!header_redzones_valid(header, pointer)) record_error();
        page_t *page = (page_t *)header->owner;
        bytes_fill(pointer, KMEM_POISON_FREE, header->requested);
        header->state = KMEM_STATE_FREE;
        if (page == 0 || page->owner != PAGE_OWNER_SLAB) {
            record_error();
            return;
        }
        page_free(page);
        return;
    }
    if (header->class_index >= KMEM_CLASS_COUNT) {
        record_error();
        return;
    }
    kmem_cache_t *cache = &g_caches[header->class_index];
    slab_page_t *slab = (slab_page_t *)header->owner;
    if (header->state != KMEM_STATE_ALLOCATED || slab == 0 ||
        slab->magic != KMEM_SLAB_MAGIC || slab->class_index != header->class_index) {
        record_error();
        return;
    }
    if (!header_redzones_valid(header, pointer)) record_error();
    size_t capacity = class_capacity(header->class_index);
    /* 两条释放路径都会用空闲链表链接覆盖首字；激活时仅验证剩余的毒化字节。 */
    bytes_fill((uint8_t *)pointer + sizeof(void *), KMEM_POISON_FREE,
               capacity - sizeof(void *));
    header->requested = 0;
    header->state = KMEM_STATE_FREE;
    header->cookie = 0;

    kmem_magazine_t *magazine = current_magazine(header->class_index,
                                               x86_current_cpu_index());
    if (magazine != 0) {
        *(void **)pointer = magazine->head;
        magazine->head = pointer;
        ++magazine->count;
        magazine_flush(header->class_index, magazine);
        return;
    }

    cache_lock(cache);
    cache_put_payload_locked(cache, slab, pointer);
    cache_unlock(cache);
}

void kfree(void *pointer) {
    if (pointer == 0) return;
    /* 大对象路径不会访问 CPU 本地空闲链表。可能耗时的毒化填充无需额外的抢占保护；page_free 会提供
     * 自身保护。绝不能放宽调用方已有的保护。 */
    allocation_header_t *header = payload_header(pointer);
    if (header->magic == KMEM_MAGIC && header->class_index == KMEM_CLASS_LARGE) {
        kfree_internal(pointer);
        return;
    }
    sched_preempt_disable();
    kfree_internal(pointer);
    sched_preempt_enable();
}

uint64_t kmem_error_count(void) {
    return atomic_load_explicit(&g_error_count, memory_order_relaxed);
}

uint64_t kmem_fastpath_hits(void) {
    uint64_t total = 0U;
    for (uint32_t cpu = 0; cpu < MAX_CPUS; ++cpu) {
        total += atomic_load_explicit(&g_cpu_stats[cpu].fastpath_hits,
                                      memory_order_relaxed);
    }
    return total;
}

uint64_t kmem_fastpath_refills(void) {
    uint64_t total = 0U;
    for (uint32_t cpu = 0; cpu < MAX_CPUS; ++cpu) {
        total += atomic_load_explicit(&g_cpu_stats[cpu].fastpath_refills,
                                      memory_order_relaxed);
    }
    return total;
}

static bool cache_partial_list_valid_locked(kmem_cache_t *cache,
                                            uint16_t class_index) {
    uint32_t visited = 0U;
    for (slab_page_t *slab = cache->slabs; slab != 0; slab = slab->next) {
        if (slab->magic != KMEM_SLAB_MAGIC ||
            slab->class_index != class_index || slab->free_list == 0 ||
            slab->free_count == 0U || slab->free_count > slab->slot_count ||
            ++visited > 1048576U) return false;
    }
    return true;
}

static bool kmem_cache_partial_list_self_test(void) {
    void *allocations[KMEM_CACHE_CHAIN_TEST_ALLOCATIONS] = {0};
    uint32_t allocated = 0U;
    bool success = true;

    for (; allocated < KMEM_CACHE_CHAIN_TEST_ALLOCATIONS; ++allocated) {
        allocations[allocated] = kmalloc(32U, 0U);
        if (allocations[allocated] == 0) {
            success = false;
            break;
        }
    }

    kmem_cache_t *cache = &g_caches[0];
    cache_lock(cache);
    if (success && !cache_partial_list_valid_locked(cache, 0U)) {
        success = false;
    }
    cache_unlock(cache);

    while (allocated != 0U) kfree(allocations[--allocated]);

    cache_lock(cache);
    if (!cache_partial_list_valid_locked(cache, 0U)) success = false;
    cache_unlock(cache);
    return success;
}

bool kmem_self_test(void) {
    uint64_t before = kmem_error_count();
    uint8_t *small = (uint8_t *)kmalloc(63U, 0);
    uint8_t *zeroed = (uint8_t *)kzalloc(127U, 0);
    uint8_t *large = (uint8_t *)kmalloc(5000U, 0);
    if (small == 0 || zeroed == 0 || large == 0 ||
        ((uintptr_t)small & 15U) != 0 || ((uintptr_t)zeroed & 15U) != 0) return false;
    for (uint32_t i = 0; i < 127U; ++i) {
        if (zeroed[i] != 0) return false;
    }
    small[0] = 1U;
    large[0] = 2U;
    large[4999] = 3U;
    kfree(zeroed);
    kfree(large);
    kfree(small);
    kfree(small); /* 双重释放必须被检测且不能破坏空闲链。 */

    uint8_t *poison = (uint8_t *)kmalloc(32U, 0);
    if (poison == 0) return false;
    kfree(poison);
    poison[12] = 0U; /* 模拟释放后写入。 */
    poison = (uint8_t *)kmalloc(32U, 0);
    if (poison == 0) return false;
    kfree(poison);

    if (!kmem_cache_partial_list_self_test()) return false;

    uint8_t *redzone = (uint8_t *)kmalloc(17U, 0);
    if (redzone == 0) return false;
    redzone[17] ^= 0xFFU; /* 修改尾部金丝雀。 */
    kfree(redzone);
    return kmem_error_count() == before + 3U;
}

typedef struct kmem_stress_slot {
    uint8_t *memory;
    uint32_t size;
    uint8_t tag;
} kmem_stress_slot_t;

static uint32_t kmem_stress_random(uint32_t *state) {
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static void kmem_stress_fill(kmem_stress_slot_t *slot) {
    if (slot == 0 || slot->memory == 0) return;
    for (uint32_t i = 0; i < slot->size; ++i) {
        slot->memory[i] = (uint8_t)(slot->tag ^ (uint8_t)(i * 29U));
    }
}

static bool kmem_stress_validate(const kmem_stress_slot_t *slot) {
    if (slot == 0 || slot->memory == 0 || slot->size == 0U) return false;
    for (uint32_t i = 0; i < slot->size; ++i) {
        if (slot->memory[i] != (uint8_t)(slot->tag ^ (uint8_t)(i * 29U))) {
            return false;
        }
    }
    return true;
}

static void kmem_stress_release(kmem_stress_slot_t *slot) {
    if (slot == 0 || slot->memory == 0) return;
    kfree(slot->memory);
    slot->memory = 0;
    slot->size = 0U;
    slot->tag = 0U;
}

bool kmem_stress_self_test(void) {
    kmem_stress_slot_t slots[KMEM_STRESS_SLOT_COUNT] = {0};
    uint32_t random_state = 0x51A3C0DEU;
    uint64_t errors_before = kmem_error_count();
    bool success = true;

    kernel_debug_stage(OS_DEBUG_PHASE_MEMORY,
                       OS_DEBUG_STEP_STRESS, 0U);
    for (uint32_t operation = 0U;
         operation < KMEM_STRESS_OPERATIONS && success;
         ++operation) {
        uint32_t index = kmem_stress_random(&random_state) %
                         KMEM_STRESS_SLOT_COUNT;
        kmem_stress_slot_t *slot = &slots[index];

        if (slot->memory != 0) {
            success = kmem_stress_validate(slot);
            kmem_stress_release(slot);
        } else {
            slot->size = 1U + kmem_stress_random(&random_state) %
                         KMEM_STRESS_MAX_SIZE;
            slot->tag = (uint8_t)kmem_stress_random(&random_state);
            slot->memory = (uint8_t *)kmalloc(slot->size, 0U);
            if (slot->memory == 0) {
                success = false;
            } else {
                kmem_stress_fill(slot);
            }
        }
    }

    for (uint32_t index = 0U; index < KMEM_STRESS_SLOT_COUNT; ++index) {
        if (slots[index].memory != 0) {
            if (!kmem_stress_validate(&slots[index])) success = false;
            kmem_stress_release(&slots[index]);
        }
    }
    if (kmem_error_count() != errors_before) success = false;

    if (!success) {
        kernel_debug_stage_fail(OS_DEBUG_PHASE_MEMORY,
                                OS_DEBUG_STEP_STRESS, K_EIO);
        return false;
    }
    kernel_debug_stage(OS_DEBUG_PHASE_MEMORY,
                       OS_DEBUG_STEP_STRESS,
                       KMEM_STRESS_OPERATIONS);
    return true;
}
