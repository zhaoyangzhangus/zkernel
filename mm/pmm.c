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

static bool usable_type(uint32_t type)
{
    return type == MEM_CONVENTIONAL ||
           type == MEM_LOADER_CODE ||
           type == MEM_LOADER_DATA ||
           type == MEM_BOOT_SERVICES_CODE ||
           type == MEM_BOOT_SERVICES_DATA;
}

/*
 * 把一个 usable range 分成天然的 4K 边缘和 2M 对齐块。
 * 这里只统计布局，不决定最终 1:7 配额。
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

static bool seed_range(uint64_t start, uint64_t pages,
                       uint64_t *split_blocks)
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
        if (*split_blocks != 0) {
            for (unsigned i = 0; i < 512U; ++i) {
                if (!pool3_push(g_pte_pool,
                                start + (uint64_t)i * PMM_PAGE_4K))
                    return false;
            }
            --*split_blocks;
        } else {
            if (!pool3_push(g_pde_pool, start))
                return false;
        }
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
     * 第一步统计最终归内核所有的 RAM：
     * LoaderCode/Data、BootServicesCode/Data、Conventional。
     *
     * 当前只做容量规划。真正 seed pool 暂时仍只使用 Conventional；
     * Loader/BootServices 等切换自有 stack/page tables 后再回收。
     *
     * 目标按物理容量：4K(PTE) : 2M(PDE) = 1 : 7。
     * 天然不能组成 2M 的边缘先算进 PTE；若仍不足 1/8，
     * 再把若干完整 2M block 规划为 512 个 4K frame。
     */
    frame_count_t count = {0};
    const uint8_t *scan = (const uint8_t *)(uintptr_t)bi->mmap_addr;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, scan += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)scan;

        if (usable_type(d->type) &&
            d->number_of_pages != 0 &&
            !count_range(d->physical_start, d->number_of_pages, &count))
            return -2;
    }

    if (count.pages == 0)
        return -3;

    uint64_t target_pte_pages = div_up64(count.pages, 8U);
    uint64_t split_blocks = 0;

    if (count.pte < target_pte_pages)
        split_blocks = div_up64(target_pte_pages - count.pte, 512U);
    if (split_blocks > count.pde)
        split_blocks = count.pde;

    uint64_t planned_pte = count.pte + split_blocks * 512U;
    uint64_t planned_pde = count.pde - split_blocks;

    /* boot_alloc 最多会额外制造 511 个 4K 前缀页。 */
    if (planned_pte > UINT64_MAX - 511U)
        return -4;

    uint64_t pte_capacity64 = planned_pte + 511U;

    if (pte_capacity64 > PMM_POOL3_MAX || planned_pde > PMM_POOL3_MAX)
        return -5;

    uint32_t pte_capacity = (uint32_t)pte_capacity64;
    uint32_t pde_capacity = (uint32_t)planned_pde;
    uint32_t cpu_capacity = pte_capacity < PMM_CPU_PTE_MAX ?
                            pte_capacity : PMM_CPU_PTE_MAX;

    uint64_t used_bytes =
        metadata_bytes(pte_capacity, pde_capacity, cpu_capacity);
    if (used_bytes == UINT64_MAX)
        return -6;

    uint64_t reserved_bytes = align_up_page(used_bytes);
    if (reserved_bytes == UINT64_MAX)
        return -6;

    uint64_t reserve_pages = reserved_bytes / PMM_PAGE_4K;
    uint64_t metadata_phys;
    if (!boot_alloc_pages(bi, reserve_pages, &metadata_phys))
        return -7;

    uint8_t *metadata = (uint8_t *)(uintptr_t)metadata_phys;
    memory_zero(metadata, reserved_bytes);

    uint8_t *cursor = metadata;
    cursor = pool3_layout(cursor, &g_pte_pool, pte_capacity);
    cursor = pool3_layout(cursor, &g_pde_pool, pde_capacity);
    cursor = pool2_layout(cursor, &g_boot_cpu, cpu_capacity);

    if ((uint64_t)(cursor - metadata) > reserved_bytes)
        return -8;

    /*
     * boot_alloc 已经原地收缩 UEFI map。当前阶段只 seed
     * MEM_CONVENTIONAL；Loader/BootServices 只参与容量规划，等自有
     * stack/page tables 建立后再回收。
     */
    const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_addr;
    uint64_t split_remaining = split_blocks;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)p;

        if (d->type == MEM_CONVENTIONAL &&
            d->number_of_pages != 0 &&
            !seed_range(d->physical_start, d->number_of_pages,
                        &split_remaining))
            return -9;
    }

    return (g_pte_pool->root != 0 || g_pde_pool->root != 0) ? 0 : -10;
}
