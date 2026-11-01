#include "frame_pool.h"
#include <limits.h>
#if !defined(FM_UEFI)
#include <string.h>
#define fm_mutex_init pthread_mutex_init
#define fm_mutex_lock pthread_mutex_lock
#define fm_mutex_unlock pthread_mutex_unlock
#define fm_mutex_destroy pthread_mutex_destroy
#endif

static int grow_locked(fm_manager *m);

/* The global TOP operates ONLY in 512-FRAME transfers to/from PCP.
 * The metadata pages still have only 511 usable entries each; the self-PTE
 * at slot zero stays untouched, even when a transfer crosses a page boundary.
 * No single-page public allocation is allowed to access this global cursor. */
static inline void global_push_512_locked(fm_manager *m, const uint64_t *entries) {
    size_t done = 0;
    while (done < FM_PCP_BATCH) {
        if (m->top_4k_offset == FM_ENTRIES_PER_META) {
            ++m->top_4k_next; /* skip next metadata header */
            m->top_4k_offset = 1;
        }
        size_t take = FM_ENTRIES_PER_META - m->top_4k_offset;
        if (take > FM_PCP_BATCH - done) take = FM_PCP_BATCH - done;
        for (size_t i = 0; i < take; ++i)
            m->top_4k_next[i] = entries[done + i];
        m->top_4k_next += take;
        m->top_4k_offset += (unsigned)take;
        m->free_4k += take;
        done += take;
    }
}

static inline void global_pop_512_locked(fm_manager *m, uint64_t *entries) {
    size_t done = 0;
    while (done < FM_PCP_BATCH) {
        if (m->top_4k_offset == 1) {
            --m->top_4k_next; /* skip previous metadata header */
            m->top_4k_offset = FM_ENTRIES_PER_META;
        }
        size_t take = m->top_4k_offset - 1;
        if (take > FM_PCP_BATCH - done) take = FM_PCP_BATCH - done;
        const uint64_t *from = m->top_4k_next - take;
        for (size_t i = 0; i < take; ++i)
            entries[FM_PCP_BATCH - 1 - done - i] = from[take - 1 - i];
        m->top_4k_next -= take;
        m->top_4k_offset -= (unsigned)take;
        m->free_4k -= take;
        done += take;
    }
}

/* Return the OLDEST 512 entries so that the PCP preserves its newest
 * 512 entries and keeps the most recently freed pages hot. */
static inline void pcp_drain_512_locked(fm_manager *m) {
    fm_pcp *pcp = &m->pcp;
    if (pcp->count < FM_PCP_BATCH) return;
    global_push_512_locked(m, pcp->entries);
    size_t left = pcp->count - FM_PCP_BATCH;
    for (size_t i = 0; i < left; ++i)
        pcp->entries[i] = pcp->entries[FM_PCP_BATCH + i];
    pcp->count = left;
    ++pcp->drains;
}

static inline void pcp_refill_512_locked(fm_manager *m) {
    /* Caller ensures 512 global entries available. PCP is empty. */
    global_pop_512_locked(m, m->pcp.entries);
    m->pcp.count = FM_PCP_BATCH;
    ++m->pcp.refills;
}

static inline uint64_t pcp_pop_one_locked(fm_manager *m) {
    if (m->pcp.count == 0) pcp_refill_512_locked(m);
    return m->pcp.entries[--m->pcp.count];
}

static inline void pcp_pop_many_locked(fm_manager *m, size_t n, uint64_t *dst) {
    size_t done = 0;
    while (done < n) {
        if (!m->pcp.count) pcp_refill_512_locked(m);
        size_t take = m->pcp.count;
        if (take > n - done) take = n - done;
        for (size_t i = 0; i < take; ++i)
            dst[done + i] = m->pcp.entries[m->pcp.count - i - 1];
        m->pcp.count -= take;
        done += take;
    }
}

/* A request is satisfied entirely through PCP. Verify that the main pool
 * can supply the required number of COMPLETE 512-frame refill operations.
 * Note that each newly mapped metadata chunk contributes only 511 frames. */
static int pcp_prepare_allocation_locked(fm_manager *m, size_t n) {
    if (n <= m->pcp.count) return FM_OK;
    size_t need = n - m->pcp.count;
    size_t batches = need / FM_PCP_BATCH + (need % FM_PCP_BATCH != 0);
    if (batches > SIZE_MAX / FM_PCP_BATCH) return FM_ENOMEM;
    size_t required = batches * FM_PCP_BATCH;
    if (required <= m->free_4k) return FM_OK;
    size_t deficit = required - m->free_4k;
    size_t chunks = deficit / FM_4K_FRAMES_PER_CHUNK +
                    (deficit % FM_4K_FRAMES_PER_CHUNK != 0);
    if (chunks > m->root_free || chunks > m->chunks_capacity - m->chunks_owned)
        return FM_ENOMEM;
    for (size_t i = 0; i < chunks; ++i) {
        int rc = grow_locked(m);
        if (rc != FM_OK) return rc;
    }
    return FM_OK;
}

/* Append one transferred 2MiB frame's 511 ready-to-use PTEs. Metadata pages
 * can be physically non-contiguous; logical stack entries can cross page
 * boundaries and occupy previously freed slots. No per-entry division. */
static void fill_4k(fm_manager *m, uint64_t physical_base) {
    size_t done = 0;
    while (done != FM_4K_FRAMES_PER_CHUNK) {
        if (m->top_4k_offset == FM_ENTRIES_PER_META) {
            ++m->top_4k_next;
            m->top_4k_offset = 1;
        }
        size_t take = FM_ENTRIES_PER_META - m->top_4k_offset;
        if (take > FM_4K_FRAMES_PER_CHUNK - done)
            take = FM_4K_FRAMES_PER_CHUNK - done;
        for (size_t i = 0; i < take; ++i)
            m->top_4k_next[i] =
                (physical_base + (done + i + 1) * FM_4K_SIZE) | FM_PTE_FLAGS;
        m->top_4k_next += take;
        m->top_4k_offset += (unsigned)take;
        m->free_4k += take;
        done += take;
    }
}

/* Expects exclusive access. Transactional for one chunk, including persistence.
 * Never publishes the chunk before map + high-water update both succeed. */
static int grow_locked(fm_manager *m) {
    if (m->root_free == 0 || m->chunks_owned >= m->chunks_capacity)
        return FM_ENOMEM;
    const size_t new_chunks = m->chunks_owned + 1;
    if (new_chunks > SIZE_MAX / FM_4K_FRAMES_PER_CHUNK)
        return FM_ENOMEM;

    uint64_t parent_pde = m->root_meta[m->root_free - 1];
    uint64_t parent_pa = fm_phys_2m(parent_pde);
    m->root_free--;

    size_t meta_idx = m->chunks_owned;
    if (m->hooks.map_meta_page(m->hooks.context, meta_idx, parent_pa)) {
        m->root_meta[m->root_free++] = parent_pde;
        return FM_EMAP;
    }

    uint64_t *new_meta = m->meta_4k + meta_idx * FM_ENTRIES_PER_META;
    new_meta[0] = parent_pa | FM_PTE_FLAGS;
    for (size_t i = 1; i < FM_ENTRIES_PER_META; ++i)
        new_meta[i] = (parent_pa + i * FM_4K_SIZE) | FM_PTE_FLAGS;

    uint64_t new_total = (uint64_t)new_chunks * FM_4K_FRAMES_PER_CHUNK;
    if (new_total > m->high_water_4k_pages && m->hooks.store_high_water) {
        if (m->hooks.store_high_water(m->hooks.context, new_total)) {
            m->hooks.unmap_meta_page(m->hooks.context, meta_idx);
            m->root_meta[m->root_free++] = parent_pde;
            return FM_EPERSIST;
        }
    }

    /* New metadata page is mapped. Make it available before writing to the
     * potentially cross-page logical stack slots. */
    m->chunks_owned = new_chunks;
    m->meta_4k_backed_end = new_meta + FM_ENTRIES_PER_META;
    fill_4k(m, parent_pa);
    if (new_total > m->high_water_4k_pages)
        m->high_water_4k_pages = new_total;
    return FM_OK;
}

int fm_init(fm_manager *m, const fm_config *c) {
    if (!m || !c || !c->root_metadata_va || !c->metadata_4k_va ||
        (!c->root_physical_frames && c->root_count) ||
        c->root_metadata_entries < c->root_count ||
        !c->hooks.map_meta_page || !c->hooks.unmap_meta_page ||
        ((uintptr_t)c->root_metadata_va & 4095u) ||
        ((uintptr_t)c->metadata_4k_va & 4095u) ||
        c->metadata_4k_pages_reserved > SIZE_MAX / FM_ENTRIES_PER_META ||
        c->root_count > SIZE_MAX / sizeof(uint64_t))
        return FM_EINVAL;

    memset(m, 0, sizeof(*m));
    if (fm_mutex_init(&m->mutex, NULL))
        return FM_EINVAL;
    m->hooks = c->hooks;
    m->root_meta = c->root_metadata_va;
    m->root_capacity = c->root_metadata_entries;
    m->root_free = c->root_count;
    m->meta_4k = c->metadata_4k_va;
    m->top_4k_next = m->meta_4k + 1;
    m->top_4k_offset = 1;
    m->meta_4k_backed_end = m->meta_4k;
    m->chunks_capacity = c->metadata_4k_pages_reserved;
    m->high_water_4k_pages = c->saved_high_water_pages;

    for (size_t i = 0; i < c->root_count; ++i) {
        uint64_t pa = c->root_physical_frames[i];
        if ((pa & (FM_2M_SIZE - 1)) || (pa & ~FM_PHYS_MASK_2M)) {
            fm_mutex_destroy(&m->mutex);
            memset(m, 0, sizeof(*m));
            return FM_EINVAL;
        }
        m->root_meta[i] = pa | FM_PDE_FLAGS;
    }

    uint64_t target = c->saved_high_water_pages /
                      FM_4K_FRAMES_PER_CHUNK;
    if (c->saved_high_water_pages % FM_4K_FRAMES_PER_CHUNK)
        ++target;
    /* Initialize metadata and usable frames to last run's HWM. */
    for (uint64_t i = 0; i < target; ++i) {
        if (grow_locked(m) != FM_OK) {
            m->boot_degraded = true;
            break;
        }
    }
    return FM_OK;
}

void fm_destroy(fm_manager *m) {
    if (!m) return;
    fm_mutex_destroy(&m->mutex);
    memset(m, 0, sizeof(*m));
}

void fm_get_stats(fm_manager *m, fm_stats *out) {
    if (!m || !out) return;
    fm_mutex_lock(&m->mutex);
    *out = (fm_stats){
        .free_2m = m->root_free,
        .chunks_owned = m->chunks_owned,
        .total_4k_frames = m->chunks_owned * FM_4K_FRAMES_PER_CHUNK,
        .free_4k = m->free_4k + m->pcp.count,
        .pcp_free_4k = m->pcp.count,
        .pcp_refills = m->pcp.refills,
        .pcp_drains = m->pcp.drains,
        .main_free_4k = m->free_4k,
        .reserved_4k_meta_pages = m->chunks_capacity,
        .backed_4k_meta_pages = (size_t)(m->meta_4k_backed_end - m->meta_4k) /
                                FM_ENTRIES_PER_META,
        .backed_4k_meta_end_va = (uintptr_t)m->meta_4k_backed_end,
        .high_water_4k_pages = m->high_water_4k_pages,
        .boot_degraded = m->boot_degraded
    };
    fm_mutex_unlock(&m->mutex);
}

int fm_alloc_2m(fm_manager *m, size_t n, uint64_t *out) {
    if (!m || !n || !out) return FM_EINVAL;
    fm_mutex_lock(&m->mutex);
    if (n > m->root_free) {
        fm_mutex_unlock(&m->mutex);
        return FM_ENOMEM;
    }
    for (size_t i = 0; i < n; ++i)
        out[i] = m->root_meta[--m->root_free];
    fm_mutex_unlock(&m->mutex);
    return FM_OK;
}

int fm_free_2m(fm_manager *m, size_t n, const uint64_t *pa) {
    if (!m || !n || !pa) return FM_EINVAL;
    for (size_t i = 0; i < n; ++i)
        if ((pa[i] & (FM_2M_SIZE - 1)) || (pa[i] & ~FM_PHYS_MASK_2M))
            return FM_EINVAL;
    fm_mutex_lock(&m->mutex);
    if (n > m->root_capacity - m->root_free) {
        fm_mutex_unlock(&m->mutex);
        return FM_ENOMEM;
    }
    for (size_t i = 0; i < n; ++i)
        m->root_meta[m->root_free++] = pa[i] | FM_PDE_FLAGS;
    fm_mutex_unlock(&m->mutex);
    return FM_OK;
}

int fm_expand_4k(fm_manager *m) {
    if (!m) return FM_EINVAL;
    fm_mutex_lock(&m->mutex);
    int rc = grow_locked(m);
    fm_mutex_unlock(&m->mutex);
    return rc;
}

int fm_alloc_4k(fm_manager *m, size_t n, uint64_t *out) {
    if (!m || !n || !out) return FM_EINVAL;
    fm_mutex_lock(&m->mutex);
    int rc = pcp_prepare_allocation_locked(m, n);
    if (rc == FM_OK) {
        if (n == 1) out[0] = pcp_pop_one_locked(m);
        else pcp_pop_many_locked(m, n, out);
    }
    fm_mutex_unlock(&m->mutex);
    return rc;
}

/* Write caller-owned leaf PTEs directly from the PCP (no staging list).
 * As with fm_alloc_4k, the global pool is accessed only in 512-frame refills. */
int fm_alloc_map_4k(fm_manager *m, size_t n, uint64_t *dst) {
    if (!m || !n || !dst || n > SIZE_MAX / sizeof(*dst))
        return FM_EINVAL;
    fm_mutex_lock(&m->mutex);
    for (size_t i = 0; i < n; ++i) {
        if (dst[i] != 0) {
            fm_mutex_unlock(&m->mutex);
            return FM_EINVAL;
        }
    }
    int rc = pcp_prepare_allocation_locked(m, n);
    if (rc == FM_OK) {
        if (n == 1) dst[0] = pcp_pop_one_locked(m);
        else pcp_pop_many_locked(m, n, dst);
    }
    fm_mutex_unlock(&m->mutex);
    return rc;
}

int fm_free_4k(fm_manager *m, size_t n, const uint64_t *pa) {
    if (!m || !n || !pa) return FM_EINVAL;
    /* Validate entire batch before any page becomes reusable. */
    for (size_t i = 0; i < n; ++i)
        if ((pa[i] & (FM_4K_SIZE - 1)) || (pa[i] & ~FM_PHYS_MASK_4K))
            return FM_EINVAL;
    fm_mutex_lock(&m->mutex);
    size_t capacity = m->chunks_owned * FM_4K_FRAMES_PER_CHUNK;
    if (n > capacity - m->free_4k - m->pcp.count) {
        fm_mutex_unlock(&m->mutex);
        return FM_ENOMEM;
    }
    size_t done = 0;
    while (done < n) {
        /* No caller touches global directly; PCP exports precisely 512. */
        if (m->pcp.count == FM_PCP_CAPACITY) pcp_drain_512_locked(m);
        size_t take = FM_PCP_CAPACITY - m->pcp.count;
        if (take > n - done) take = n - done;
        for (size_t i = 0; i < take; ++i)
            m->pcp.entries[m->pcp.count + i] = pa[done + i] | FM_PTE_FLAGS;
        m->pcp.count += take;
        done += take;
        if (m->pcp.count == FM_PCP_CAPACITY) pcp_drain_512_locked(m);
    }
    fm_mutex_unlock(&m->mutex);
    return FM_OK;
}

int fm_pcp_drain_4k(fm_manager *m) {
    if (!m) return FM_EINVAL;
    fm_mutex_lock(&m->mutex);
    if (m->pcp.count >= FM_PCP_BATCH) pcp_drain_512_locked(m);
    fm_mutex_unlock(&m->mutex);
    return FM_OK;
}

uint64_t fm_4k_metadata_phys(fm_manager *m, size_t index) {
    if (!m) return UINT64_MAX;
    fm_mutex_lock(&m->mutex);
    uint64_t p = index < (size_t)(m->meta_4k_backed_end - m->meta_4k) /
                         FM_ENTRIES_PER_META
        ? fm_phys_4k(m->meta_4k[index * FM_ENTRIES_PER_META])
        : UINT64_MAX;
    fm_mutex_unlock(&m->mutex);
    return p;
}
