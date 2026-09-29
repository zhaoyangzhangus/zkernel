#include "pmm.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t root;
    uint32_t capacity;
    uint32_t leaf_count;
    uint32_t middle_count;
    uint32_t reserved;
    uint64_t *middle;
    uint64_t *leaf;
    pmm_frame_t *slot;
} pmm_pool3_t;

typedef struct {
    uint64_t pte;
    uint64_t pde;
    uint64_t pages;
} frame_count_t;

static pmm_pool3_t *g_pte_pool;
static pmm_pool3_t *g_pde_pool;
static pmm_cpu_t *g_boot_cpu;

static inline uint64_t div_up64(uint64_t n, uint64_t d)
{
    return n / d + (n % d != 0);
}

static inline uint64_t align_up_page(uint64_t n)
{
    if (n > UINT64_MAX - (PMM_PAGE_4K - 1))
        return UINT64_MAX;
    return (n + PMM_PAGE_4K - 1) & ~(PMM_PAGE_4K - 1);
}

static inline unsigned scan1(uint64_t value)
{
    return (unsigned)__builtin_ctzll(value);
}

static inline uint64_t leaf_valid_mask(uint32_t capacity, uint32_t leaf_index)
{
    uint32_t first = leaf_index << 6;
    uint32_t left = capacity - first;

    if (left >= 64U)
        return UINT64_MAX;
    return left == 0 ? 0 : (UINT64_C(1) << left) - 1;
}

static uint64_t pool3_bytes(uint32_t capacity)
{
    uint64_t leaf_count = div_up64(capacity, 64U);
    uint64_t middle_count = div_up64(leaf_count, 64U);

    return sizeof(pmm_pool3_t) +
           middle_count * sizeof(uint64_t) +
           leaf_count * sizeof(uint64_t) +
           (uint64_t)capacity * sizeof(pmm_frame_t);
}

static uint64_t pool2_bytes(uint32_t capacity)
{
    uint64_t leaf_count = div_up64(capacity, 64U);

    return sizeof(pmm_cpu_t) +
           leaf_count * sizeof(uint64_t) +
           (uint64_t)capacity * sizeof(pmm_frame_t);
}

static uint64_t metadata_bytes(uint32_t pte_capacity,
                               uint32_t pde_capacity,
                               uint32_t cpu_capacity)
{
    uint64_t a = pool3_bytes(pte_capacity);
    uint64_t b = pool3_bytes(pde_capacity);
    uint64_t c = pool2_bytes(cpu_capacity);

    if (a > UINT64_MAX - b || a + b > UINT64_MAX - c)
        return UINT64_MAX;
    return a + b + c;
}

static void memory_zero(void *ptr, uint64_t bytes)
{
    uint8_t *p = ptr;
    while (bytes-- != 0)
        *p++ = 0;
}

/*
 * 把一个 Conventional range 分成：
 *   两端不满足 2M 对齐的 4K PTE frames
 *   中间完整 2M PDE frames
 */
static bool count_range(uint64_t start, uint64_t pages, frame_count_t *count)
{
    if (pages > UINT64_MAX / PMM_PAGE_4K)
        return false;

    uint64_t bytes = pages * PMM_PAGE_4K;
    if (start > UINT64_MAX - bytes)
        return false;

    uint64_t end = start + bytes;
    uint64_t first = (start + PMM_PAGE_2M - 1) & ~(PMM_PAGE_2M - 1);
    uint64_t last = end & ~(PMM_PAGE_2M - 1);

    if (first < start || first >= end || last <= first) {
        count->pte += pages;
    } else {
        count->pte += (first - start) / PMM_PAGE_4K;
        count->pde += (last - first) / PMM_PAGE_2M;
        count->pte += (end - last) / PMM_PAGE_4K;
    }

    count->pages += pages;
    return true;
}

/*
 * 模拟 bootstrap allocator 从第一个足够大的 Conventional descriptor
 * 低地址端拿 reserve_pages。这里只计算，不修改 UEFI map。
 */
static bool count_after_reserve(const BOOT_INFO *bi,
                                uint64_t reserve_pages,
                                frame_count_t *count)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_addr;
    bool reserved = reserve_pages == 0;

    *count = (frame_count_t){0};

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)p;

        if (d->type != MEM_CONVENTIONAL || d->number_of_pages == 0)
            continue;

        uint64_t start = d->physical_start;
        uint64_t pages = d->number_of_pages;

        if (!reserved && pages >= reserve_pages) {
            start += reserve_pages * PMM_PAGE_4K;
            pages -= reserve_pages;
            reserved = true;
        }

        if (pages != 0 && !count_range(start, pages, count))
            return false;
    }

    return reserved;
}

/*
 * 真正的 bootstrap allocator。它只在 pmm_init() 内使用一次，
 * 直接收缩 UEFI MEM_CONVENTIONAL descriptor；初始化完成后生命周期结束。
 */
static bool boot_alloc_pages(BOOT_INFO *bi, uint64_t pages, uint64_t *out_phys)
{
    uint8_t *p = (uint8_t *)(uintptr_t)bi->mmap_addr;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        BOOT_MEMORY_DESCRIPTOR *d = (BOOT_MEMORY_DESCRIPTOR *)(void *)p;

        if (d->type != MEM_CONVENTIONAL || d->number_of_pages < pages)
            continue;

        uint64_t bytes = pages * PMM_PAGE_4K;
        uint64_t base = d->physical_start;

        if (base > UINT64_MAX - bytes)
            return false;

        d->physical_start = base + bytes;
        d->number_of_pages -= pages;
        if (d->virtual_start != 0)
            d->virtual_start += bytes;

        *out_phys = base;
        return true;
    }

    return false;
}

static uint8_t *pool3_layout(uint8_t *p, pmm_pool3_t **out,
                             uint32_t capacity)
{
    pmm_pool3_t *pool = (pmm_pool3_t *)(void *)p;
    p += sizeof(*pool);

    pool->root = 0;
    pool->capacity = capacity;
    pool->leaf_count = (uint32_t)div_up64(capacity, 64U);
    pool->middle_count = (uint32_t)div_up64(pool->leaf_count, 64U);
    pool->reserved = 0;

    pool->middle = (uint64_t *)(void *)p;
    p += (uint64_t)pool->middle_count * sizeof(uint64_t);

    pool->leaf = (uint64_t *)(void *)p;
    p += (uint64_t)pool->leaf_count * sizeof(uint64_t);

    pool->slot = (pmm_frame_t *)(void *)p;
    p += (uint64_t)capacity * sizeof(pmm_frame_t);

    *out = pool;
    return p;
}

static uint8_t *pool2_layout(uint8_t *p, pmm_cpu_t **out,
                             uint32_t capacity)
{
    pmm_cpu_t *pool = (pmm_cpu_t *)(void *)p;
    p += sizeof(*pool);

    pool->root = 0;
    pool->capacity = capacity;
    pool->leaf_count = (uint32_t)div_up64(capacity, 64U);

    pool->leaf = (uint64_t *)(void *)p;
    p += (uint64_t)pool->leaf_count * sizeof(uint64_t);

    pool->slot = (pmm_frame_t *)(void *)p;
    p += (uint64_t)capacity * sizeof(pmm_frame_t);

    *out = pool;
    return p;
}

static bool pool2_pop(pmm_cpu_t *pool, pmm_frame_t *out)
{
    uint64_t root = pool->root;
    if (root == 0)
        return false;

    unsigned li = scan1(root);
    uint64_t leaf = pool->leaf[li];
    unsigned bi = scan1(leaf);
    uint64_t bit = UINT64_C(1) << bi;

    *out = pool->slot[(li << 6) | bi];

    leaf &= ~bit;
    pool->leaf[li] = leaf;
    if (leaf == 0)
        pool->root = root & ~(UINT64_C(1) << li);

    return true;
}

static bool pool2_push(pmm_cpu_t *pool, pmm_frame_t frame)
{
    for (uint32_t li = 0; li < pool->leaf_count; ++li) {
        uint64_t leaf = pool->leaf[li];
        uint64_t empty = ~leaf & leaf_valid_mask(pool->capacity, li);

        if (empty == 0)
            continue;

        unsigned bi = scan1(empty);
        uint64_t bit = UINT64_C(1) << bi;

        pool->slot[(li << 6) | bi] = frame;
        pool->leaf[li] = leaf | bit;
        if (leaf == 0)
            pool->root |= UINT64_C(1) << li;

        return true;
    }

    return false;
}

static bool pool3_pop(pmm_pool3_t *pool, pmm_frame_t *out)
{
    uint64_t root = pool->root;
    if (root == 0)
        return false;

    unsigned ri = scan1(root);
    uint64_t middle = pool->middle[ri];
    unsigned mi = scan1(middle);
    uint32_t li = (ri << 6) | mi;
    uint64_t leaf = pool->leaf[li];
    unsigned bi = scan1(leaf);
    uint64_t bit = UINT64_C(1) << bi;

    *out = pool->slot[(li << 6) | bi];

    leaf &= ~bit;
    pool->leaf[li] = leaf;

    if (leaf == 0) {
        middle &= ~(UINT64_C(1) << mi);
        pool->middle[ri] = middle;
        if (middle == 0)
            pool->root = root & ~(UINT64_C(1) << ri);
    }

    return true;
}

static bool pool3_push(pmm_pool3_t *pool, pmm_frame_t frame)
{
    for (uint32_t li = 0; li < pool->leaf_count; ++li) {
        uint64_t leaf = pool->leaf[li];
        uint64_t empty = ~leaf & leaf_valid_mask(pool->capacity, li);

        if (empty == 0)
            continue;

        unsigned bi = scan1(empty);
        uint64_t bit = UINT64_C(1) << bi;
        uint32_t ri = li >> 6;
        uint32_t mi = li & 63U;

        pool->slot[(li << 6) | bi] = frame;
        pool->leaf[li] = leaf | bit;

        if (leaf == 0) {
            uint64_t middle = pool->middle[ri];
            pool->middle[ri] = middle | (UINT64_C(1) << mi);
            if (middle == 0)
                pool->root |= UINT64_C(1) << ri;
        }

        return true;
    }

    return false;
}

static uint32_t pool3_free_slots(const pmm_pool3_t *pool, uint32_t stop_at)
{
    uint32_t free_slots = 0;

    for (uint32_t li = 0; li < pool->leaf_count; ++li) {
        uint64_t valid = leaf_valid_mask(pool->capacity, li);
        free_slots += (uint32_t)__builtin_popcountll((~pool->leaf[li]) & valid);
        if (free_slots >= stop_at)
            break;
    }

    return free_slots;
}

static bool refill_pte(pmm_cpu_t *cpu)
{
    uint32_t batch = cpu->capacity < PMM_BATCH ? cpu->capacity : PMM_BATCH;
    pmm_frame_t frame;
    uint32_t moved = 0;

    while (moved < batch && pool3_pop(g_pte_pool, &frame)) {
        if (!pool2_push(cpu, frame))
            break;
        ++moved;
    }

    return moved != 0;
}

static bool drain_pte(pmm_cpu_t *cpu)
{
    uint32_t batch = cpu->capacity < PMM_BATCH ? cpu->capacity : PMM_BATCH;

    if (batch == 0 || pool3_free_slots(g_pte_pool, batch) < batch)
        return false;

    pmm_frame_t frames[PMM_BATCH];

    for (uint32_t i = 0; i < batch; ++i)
        if (!pool2_pop(cpu, &frames[i]))
            return false;

    for (uint32_t i = 0; i < batch; ++i)
        if (!pool3_push(g_pte_pool, frames[i]))
            return false;

    return true;
}

bool pmm_alloc4k(pmm_cpu_t *cpu, pmm_frame_t *out_frame)
{
    if (cpu == NULL || out_frame == NULL)
        return false;

    if (pool2_pop(cpu, out_frame))
        return true;

    return refill_pte(cpu) && pool2_pop(cpu, out_frame);
}

bool pmm_free4k(pmm_cpu_t *cpu, pmm_frame_t frame)
{
    if (cpu == NULL)
        return false;

    if (pool2_push(cpu, frame))
        return true;

    if (drain_pte(cpu))
        return pool2_push(cpu, frame);

    return pool3_push(g_pte_pool, frame);
}

bool pmm_alloc2m(pmm_frame_t *out_frame)
{
    return out_frame != NULL && pool3_pop(g_pde_pool, out_frame);
}

bool pmm_free2m(pmm_frame_t frame)
{
    return pool3_push(g_pde_pool, frame);
}

pmm_cpu_t *pmm_boot_cpu(void)
{
    return g_boot_cpu;
}

static bool seed_range(uint64_t start, uint64_t pages)
{
    if (pages > UINT64_MAX / PMM_PAGE_4K)
        return false;

    uint64_t end = start + pages * PMM_PAGE_4K;
    uint64_t first = (start + PMM_PAGE_2M - 1) & ~(PMM_PAGE_2M - 1);
    uint64_t last = end & ~(PMM_PAGE_2M - 1);

    if (first < start || first >= end || last <= first) {
        while (start < end) {
            if (!pool3_push(g_pte_pool, start))
                return false;
            start += PMM_PAGE_4K;
        }
        return true;
    }

    while (start < first) {
        if (!pool3_push(g_pte_pool, start))
            return false;
        start += PMM_PAGE_4K;
    }

    while (start < last) {
        if (!pool3_push(g_pde_pool, start))
            return false;
        start += PMM_PAGE_2M;
    }

    while (start < end) {
        if (!pool3_push(g_pte_pool, start))
            return false;
        start += PMM_PAGE_4K;
    }

    return true;
}

int pmm_init(BOOT_INFO *bi)
{
    if (bi == NULL || bi->mmap_addr == 0 ||
        bi->mmap_desc_size < sizeof(BOOT_MEMORY_DESCRIPTOR) ||
        bi->mmap_desc_count == 0 ||
        (uint64_t)bi->mmap_desc_count > bi->mmap_size / bi->mmap_desc_size)
        return -1;

    /*
     * 自举定容：
     * 先假设预留 1 页 metadata；模拟收缩 memory map 后统计真正的
     * PTE/PDE frame 数，再计算实际 metadata 页数。只向上增加预留，
     * 直到当前预留已经足够，因此不会出现“metadata 自己改变 pool 大小”
     * 导致空间不足的问题。
     */
    uint64_t reserve_pages = 1;
    frame_count_t count = {0};
    uint64_t used_bytes = 0;
    uint32_t pte_capacity = 0;
    uint32_t pde_capacity = 0;
    uint32_t cpu_capacity = 0;

    for (unsigned pass = 0; pass < 32U; ++pass) {
        if (!count_after_reserve(bi, reserve_pages, &count))
            return -2;

        if (count.pte > PMM_POOL3_MAX || count.pde > PMM_POOL3_MAX)
            return -3;

        pte_capacity = (uint32_t)count.pte;
        pde_capacity = (uint32_t)count.pde;
        cpu_capacity = pte_capacity < PMM_CPU_PTE_MAX ?
                       pte_capacity : PMM_CPU_PTE_MAX;

        used_bytes = metadata_bytes(pte_capacity, pde_capacity, cpu_capacity);
        if (used_bytes == UINT64_MAX)
            return -4;

        uint64_t need = align_up_page(used_bytes);
        if (need == UINT64_MAX)
            return -4;
        need /= PMM_PAGE_4K;

        if (need <= reserve_pages)
            break;

        reserve_pages = need;

        if (pass == 31U)
            return -5;
    }

    uint64_t metadata_phys;
    if (!boot_alloc_pages(bi, reserve_pages, &metadata_phys))
        return -6;

    uint64_t reserved_bytes = reserve_pages * PMM_PAGE_4K;
    uint8_t *metadata = (uint8_t *)(uintptr_t)metadata_phys;
    memory_zero(metadata, reserved_bytes);

    uint8_t *cursor = metadata;
    cursor = pool3_layout(cursor, &g_pte_pool, pte_capacity);
    cursor = pool3_layout(cursor, &g_pde_pool, pde_capacity);
    cursor = pool2_layout(cursor, &g_boot_cpu, cpu_capacity);

    if ((uint64_t)(cursor - metadata) > reserved_bytes)
        return -7;

    /*
     * 现在 UEFI map 已经被 bootstrap allocator 原地收缩。
     * 再扫一次，填入的正好是 metadata 之外真正剩余的 free frames。
     */
    const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_addr;
    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)p;

        if (d->type == MEM_CONVENTIONAL &&
            d->number_of_pages != 0 &&
            !seed_range(d->physical_start, d->number_of_pages))
            return -8;
    }

    return (g_pte_pool->root != 0 || g_pde_pool->root != 0) ? 0 : -9;
}
