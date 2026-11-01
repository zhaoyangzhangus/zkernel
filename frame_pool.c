#include "frame_pool.h"
#include <limits.h>
#if !defined(FM_UEFI)
#include <string.h>
#define fm_mutex_init pthread_mutex_init
#define fm_mutex_lock pthread_mutex_lock
#define fm_mutex_unlock pthread_mutex_unlock
#define fm_mutex_destroy pthread_mutex_destroy
#endif

/* Dense 4K free stack: each mapped metadata page has 511 ordinary slots
 * (entries 1..511); entry 0 is its own physical metadata-page PTE.
 * The TOP cursor points to the NEXT insertion slot in a contiguous VA range.
 * offset 1..512 is the position within its current metadata page; offset 512
 * denotes one-past-last, so neither single-item path needs /511 or %511.
 * A cursor may point at the next page's header only when offset == 512;
 * that header is NEVER dereferenced by a push or pop.
 * Caller must exclude concurrent access in SMP/preemptive configurations. */
static inline uint64_t pop_4k_locked(fm_manager *m) {
    if (m->top_4k_offset == 1) {
        /* Previous page ended at the current page's entry 0. */
        --m->top_4k_next;
        m->top_4k_offset = FM_ENTRIES_PER_META;
    }
    --m->top_4k_next;
    --m->top_4k_offset;
    --m->free_4k;
    return *m->top_4k_next;
}

static inline void push_4k_locked(fm_manager *m, uint64_t entry) {
    if (m->top_4k_offset == FM_ENTRIES_PER_META) {
        /* Skip metadata self-PTE (entry 0 of next mapped page). */
        ++m->top_4k_next;
        m->top_4k_offset = 1;
    }
    *m->top_4k_next++ = entry;
    ++m->top_4k_offset;
    ++m->free_4k;
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
        .free_4k = m->free_4k,
        .reserved_4k_meta_pages = m->chunks_capacity,
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

    if (n > m->free_4k) {
        size_t need = n - m->free_4k;
        size_t chunks = need / FM_4K_FRAMES_PER_CHUNK +
                        (need % FM_4K_FRAMES_PER_CHUNK != 0);
        /* Preflight: a failed allocation due to insufficient total resources
         * does not even cause partial expansion. */
        if (chunks > m->root_free ||
            chunks > m->chunks_capacity - m->chunks_owned) {
            fm_mutex_unlock(&m->mutex);
            return FM_ENOMEM;
        }
        for (size_t i = 0; i < chunks; ++i) {
            int rc = grow_locked(m);
            if (rc != FM_OK) {
                /* Completed prior expansions remain in the free pool. No
                 * requested frames are allocated, and out is untouched. */
                fm_mutex_unlock(&m->mutex);
                return rc;
            }
        }
    }

    /* Single-frame hot path: two cursor adjustments and one read,
     * with no /511, %511, slot lookup, or generic batch loop. */
    if (n == 1) {
        out[0] = pop_4k_locked(m);
    } else {
        /* Batch path: transfer segments inside each metadata page. */
        size_t left = n, written = 0;
        while (left) {
            if (m->top_4k_offset == 1) {
                --m->top_4k_next;
                m->top_4k_offset = FM_ENTRIES_PER_META;
            }
            size_t take = m->top_4k_offset - 1;
            if (take > left) take = left;
            const uint64_t *from = m->top_4k_next - take;
            for (size_t j = 0; j < take; ++j)
                out[written + j] = from[take - 1 - j];
            m->top_4k_next -= take;
            m->top_4k_offset -= (unsigned)take;
            m->free_4k -= take;
            left -= take;
            written += take;
        }
    }
    fm_mutex_unlock(&m->mutex);
    return FM_OK;
}

/* Map directly from the metadata TOP cursor into caller-owned leaf PTEs.
 * This shares the same expansion and locking rules as fm_alloc_4k(), but
 * never creates a separate allocated-frame list.  dst[i] must be empty.
 * Checking the whole destination before removing any frames guarantees
 * that EBUSY-style failures leave the destination and free stack untouched.
 */
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
    if (n > m->free_4k) {
        size_t need = n - m->free_4k;
        size_t chunks = need / FM_4K_FRAMES_PER_CHUNK +
                        (need % FM_4K_FRAMES_PER_CHUNK != 0);
        if (chunks > m->root_free ||
            chunks > m->chunks_capacity - m->chunks_owned) {
            fm_mutex_unlock(&m->mutex);
            return FM_ENOMEM;
        }
        for (size_t i = 0; i < chunks; ++i) {
            int rc = grow_locked(m);
            if (rc != FM_OK) {
                fm_mutex_unlock(&m->mutex);
                return rc;
            }
        }
    }
    if (n == 1) {
        /* Hot path: write the PTE directly; no /511, %511 or staging array. */
        dst[0] = pop_4k_locked(m);
    } else {
        size_t remaining = n;
        size_t position = 0;
        while (remaining) {
            if (m->top_4k_offset == 1) {
                --m->top_4k_next;
                m->top_4k_offset = FM_ENTRIES_PER_META;
            }
            size_t take = m->top_4k_offset - 1;
            if (take > remaining) take = remaining;
            const uint64_t *src = m->top_4k_next - take;
            for (size_t j = 0; j < take; ++j)
                dst[position + j] = src[take - 1 - j];
            m->top_4k_next -= take;
            m->top_4k_offset -= (unsigned)take;
            m->free_4k -= take;
            position += take;
            remaining -= take;
        }
    }
    fm_mutex_unlock(&m->mutex);
    return FM_OK;
}

int fm_free_4k(fm_manager *m, size_t n, const uint64_t *pa) {
    if (!m || !n || !pa) return FM_EINVAL;
    /* Validate entire input BEFORE touching the pool. */
    for (size_t i = 0; i < n; ++i)
        if ((pa[i] & (FM_4K_SIZE - 1)) || (pa[i] & ~FM_PHYS_MASK_4K))
            return FM_EINVAL;
    fm_mutex_lock(&m->mutex);
    size_t capacity = m->chunks_owned * FM_4K_FRAMES_PER_CHUNK;
    if (n > capacity - m->free_4k) {
        fm_mutex_unlock(&m->mutex);
        return FM_ENOMEM;
    }
    if (n == 1) {
        push_4k_locked(m, pa[0] | FM_PTE_FLAGS);
    } else {
        size_t left = n, consumed = 0;
        while (left) {
            if (m->top_4k_offset == FM_ENTRIES_PER_META) {
                ++m->top_4k_next;
                m->top_4k_offset = 1;
            }
            size_t take = FM_ENTRIES_PER_META - m->top_4k_offset;
            if (take > left) take = left;
            for (size_t j = 0; j < take; ++j)
                m->top_4k_next[j] = pa[consumed + j] | FM_PTE_FLAGS;
            m->top_4k_next += take;
            m->top_4k_offset += (unsigned)take;
            m->free_4k += take;
            consumed += take;
            left -= take;
        }
    }
    fm_mutex_unlock(&m->mutex);
    return FM_OK;
}

uint64_t fm_4k_metadata_phys(fm_manager *m, size_t index) {
    if (!m) return UINT64_MAX;
    fm_mutex_lock(&m->mutex);
    uint64_t p = index < m->chunks_owned
        ? fm_phys_4k(m->meta_4k[index * FM_ENTRIES_PER_META])
        : UINT64_MAX;
    fm_mutex_unlock(&m->mutex);
    return p;
}
