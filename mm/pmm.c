#include "pmm.h"

#include <stddef.h>
#include <stdint.h>

#define POOL3_MIDDLE_COUNT 64U
#define POOL3_LEAF_COUNT   (64U * 64U)

typedef struct {
    uint64_t root;
    uint64_t middle[POOL3_MIDDLE_COUNT];
    uint64_t leaf[POOL3_LEAF_COUNT];
    pmm_frame_t slot[PMM_POOL3_SLOTS];
} pmm_pool3_t;

/* global PTE/PDE free pools：两者统一三层 64 叉。 */
static pmm_pool3_t g_pte_pool;
static pmm_pool3_t g_pde_pool;

/* 现在只有 BSP；SMP 后每个逻辑 CPU 各自持有一份 pmm_cpu_t。 */
static pmm_cpu_t g_boot_cpu;

static inline unsigned scan1(uint64_t value)
{
    return (unsigned)__builtin_ctzll(value);
}

static void pool3_reset(pmm_pool3_t *p)
{
    p->root = 0;
    for (unsigned i = 0; i < POOL3_MIDDLE_COUNT; ++i)
        p->middle[i] = 0;
    for (unsigned i = 0; i < POOL3_LEAF_COUNT; ++i)
        p->leaf[i] = 0;
}

void pmm_cpu_init(pmm_cpu_t *cpu)
{
    cpu->root = 0;
    for (unsigned i = 0; i < 64U; ++i)
        cpu->leaf[i] = 0;
}

pmm_cpu_t *pmm_boot_cpu(void)
{
    return &g_boot_cpu;
}

/* root -> leaf -> slot */
static bool pool2_pop(pmm_cpu_t *p, pmm_frame_t *out)
{
    uint64_t root = p->root;
    if (root == 0)
        return false;

    unsigned li = scan1(root);
    uint64_t leaf = p->leaf[li];
    if (leaf == 0)
        return false;

    unsigned bit_index = scan1(leaf);
    uint64_t bit = UINT64_C(1) << bit_index;

    *out = p->slot[(li << 6) | bit_index];

    leaf &= ~bit;
    p->leaf[li] = leaf;
    if (leaf == 0)
        p->root = root & ~(UINT64_C(1) << li);

    return true;
}

/*
 * free 不做 frame->slot 反查。
 * 扫任意 bit=0 的 slot，frame 放进去后恢复 availability bit。
 */
static bool pool2_push(pmm_cpu_t *p, pmm_frame_t frame)
{
    for (unsigned li = 0; li < 64U; ++li) {
        uint64_t leaf = p->leaf[li];
        if (leaf == UINT64_MAX)
            continue;

        unsigned bit_index = scan1(~leaf);
        uint64_t bit = UINT64_C(1) << bit_index;

        p->slot[(li << 6) | bit_index] = frame;
        p->leaf[li] = leaf | bit;
        if (leaf == 0)
            p->root |= UINT64_C(1) << li;

        return true;
    }

    return false;
}

/* root -> middle -> leaf -> slot */
static bool pool3_pop(pmm_pool3_t *p, pmm_frame_t *out)
{
    uint64_t root = p->root;
    if (root == 0)
        return false;

    unsigned ri = scan1(root);
    uint64_t middle = p->middle[ri];
    if (middle == 0)
        return false;

    unsigned mi = scan1(middle);
    unsigned li = (ri << 6) | mi;
    uint64_t leaf = p->leaf[li];
    if (leaf == 0)
        return false;

    unsigned bi = scan1(leaf);
    uint64_t bit = UINT64_C(1) << bi;

    *out = p->slot[(li << 6) | bi];

    leaf &= ~bit;
    p->leaf[li] = leaf;

    if (leaf == 0) {
        middle &= ~(UINT64_C(1) << mi);
        p->middle[ri] = middle;
        if (middle == 0)
            p->root = root & ~(UINT64_C(1) << ri);
    }

    return true;
}

static bool pool3_push(pmm_pool3_t *p, pmm_frame_t frame)
{
    for (unsigned li = 0; li < POOL3_LEAF_COUNT; ++li) {
        uint64_t leaf = p->leaf[li];
        if (leaf == UINT64_MAX)
            continue;

        unsigned bi = scan1(~leaf);
        uint64_t bit = UINT64_C(1) << bi;
        unsigned ri = li >> 6;
        unsigned mi = li & 63U;

        p->slot[(li << 6) | bi] = frame;
        p->leaf[li] = leaf | bit;

        if (leaf == 0) {
            uint64_t middle = p->middle[ri];
            p->middle[ri] = middle | (UINT64_C(1) << mi);
            if (middle == 0)
                p->root |= UINT64_C(1) << ri;
        }

        return true;
    }

    return false;
}

static bool pool3_has_empty(const pmm_pool3_t *p, unsigned need)
{
    unsigned free_slots = 0;

    for (unsigned i = 0; i < POOL3_LEAF_COUNT; ++i) {
        free_slots += 64U - (unsigned)__builtin_popcountll(p->leaf[i]);
        if (free_slots >= need)
            return true;
    }

    return false;
}

/* CPU PTE pool 为空时，从 global PTE pool 搬 64 个。 */
static bool refill_pte(pmm_cpu_t *cpu)
{
    pmm_frame_t frame;
    unsigned moved = 0;

    while (moved < PMM_REFILL_BATCH &&
           pool3_pop(&g_pte_pool, &frame)) {
        if (!pool2_push(cpu, frame))
            return false;
        ++moved;
    }

    return moved != 0;
}

/* CPU PTE pool 满时，一次 drain 64 个回 global PTE pool。 */
static bool drain_pte(pmm_cpu_t *cpu)
{
    if (!pool3_has_empty(&g_pte_pool, PMM_REFILL_BATCH))
        return false;

    pmm_frame_t batch[PMM_REFILL_BATCH];

    for (unsigned i = 0; i < PMM_REFILL_BATCH; ++i) {
        if (!pool2_pop(cpu, &batch[i]))
            return false;
    }

    for (unsigned i = 0; i < PMM_REFILL_BATCH; ++i) {
        if (!pool3_push(&g_pte_pool, batch[i]))
            return false;
    }

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

    /* global 还有少量 slot 时直接归还 global，避免无意义失败。 */
    return pool3_push(&g_pte_pool, frame);
}

bool pmm_alloc2m(pmm_frame_t *out_frame)
{
    return out_frame != NULL && pool3_pop(&g_pde_pool, out_frame);
}

bool pmm_free2m(pmm_frame_t frame)
{
    return pool3_push(&g_pde_pool, frame);
}

/*
 * 初始化阶段是唯一解析物理地址的地方。
 * runtime pool 只移动 opaque 64-bit frame，不解析 PFN/地址。
 *
 * 每个 Conventional range：
 *   - 两端不足 2M 对齐的页 -> global PTE pool
 *   - 中间完整 2M 对齐块     -> global PDE pool
 */
static bool seed_range(uint64_t start, uint64_t pages)
{
    if (pages > UINT64_MAX / PMM_PAGE_4K)
        return false;

    uint64_t bytes = pages * PMM_PAGE_4K;
    if (start > UINT64_MAX - bytes)
        return false;

    uint64_t end = start + bytes;

    while (start < end && (start & (PMM_PAGE_2M - 1)) != 0) {
        if (!pool3_push(&g_pte_pool, start))
            return false;
        start += PMM_PAGE_4K;
    }

    while (end - start >= PMM_PAGE_2M) {
        if (!pool3_push(&g_pde_pool, start))
            return false;
        start += PMM_PAGE_2M;
    }

    while (start < end) {
        if (!pool3_push(&g_pte_pool, start))
            return false;
        start += PMM_PAGE_4K;
    }

    return true;
}

int pmm_init(const BOOT_INFO *bi)
{
    if (bi == NULL || bi->mmap_addr == 0 ||
        bi->mmap_desc_size < sizeof(BOOT_MEMORY_DESCRIPTOR) ||
        bi->mmap_desc_count == 0 ||
        (uint64_t)bi->mmap_desc_count > bi->mmap_size / bi->mmap_desc_size)
        return -1;

    pool3_reset(&g_pte_pool);
    pool3_reset(&g_pde_pool);
    pmm_cpu_init(&g_boot_cpu);

    const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_addr;

    for (uint32_t i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const BOOT_MEMORY_DESCRIPTOR *d =
            (const BOOT_MEMORY_DESCRIPTOR *)(const void *)p;

        if (d->type != MEM_CONVENTIONAL || d->number_of_pages == 0)
            continue;

        if (!seed_range(d->physical_start, d->number_of_pages))
            return -2;
    }

    /*
     * 极端情况下所有 Conventional range 都刚好 2M 对齐，PTE pool 会为空。
     * 初始化阶段允许把一个原生 PDE frame 降级为 512 个 PTE frame；
     * runtime allocator 本身仍然不解释 frame。
     */
    if (g_pte_pool.root == 0 && g_pde_pool.root != 0) {
        pmm_frame_t base;
        if (!pool3_pop(&g_pde_pool, &base))
            return -3;

        for (unsigned i = 0; i < 512U; ++i) {
            if (!pool3_push(&g_pte_pool,
                            base + (uint64_t)i * PMM_PAGE_4K))
                return -3;
        }
    }

    return (g_pte_pool.root != 0 || g_pde_pool.root != 0) ? 0 : -4;
}
