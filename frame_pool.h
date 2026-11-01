#ifndef ZKERNEL_FRAME_POOL_H
#define ZKERNEL_FRAME_POOL_H

/* Host-testable reference implementation. The single-core UEFI build uses
 * no synchronization; native Linux tests retain pthread mutexes.
 * For SMP/preemptive zkernel, supply appropriate synchronization.
 * x86-64 4 KiB PTE / 2 MiB PDE formats. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#if defined(FM_UEFI)
#include "uefi_shim.h"
#else
#include <pthread.h>
typedef pthread_mutex_t fm_mutex_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    FM_OK = 0,
    FM_EINVAL = -1,
    FM_ENOMEM = -2,
    FM_EMAP = -3,
    FM_EPERSIST = -4
};

#define FM_4K_SIZE UINT64_C(4096)
#define FM_2M_SIZE UINT64_C(2097152)
#define FM_ENTRIES_PER_META 512u
#define FM_4K_FRAMES_PER_CHUNK 511u
#define FM_X86_P UINT64_C(0x001)
#define FM_X86_RW UINT64_C(0x002)
#define FM_X86_PS UINT64_C(0x080)
#define FM_X86_NX (UINT64_C(1) << 63)
#define FM_PTE_FLAGS (FM_X86_P | FM_X86_RW | FM_X86_NX)
#define FM_PDE_FLAGS (FM_X86_P | FM_X86_RW | FM_X86_PS | FM_X86_NX)
#define FM_PHYS_MASK_4K UINT64_C(0x000ffffffffff000)
#define FM_PHYS_MASK_2M UINT64_C(0x000fffffffe00000)

/* These callbacks have no implicit allocation. The 4 KiB metadata physical
 * page is exactly the FIRST 4 KiB of the transferred 2 MiB frame.
 * map_meta_page must map it at metadata_va + index*4096 (a previously reserved
 * contiguous virtual range). Callbacks may block; UEFI test is non-reentrant. */
typedef struct fm_hooks {
    void *context;
    int (*map_meta_page)(void *context, size_t index, uint64_t physical_page);
    void (*unmap_meta_page)(void *context, size_t index);
    int (*store_high_water)(void *context, uint64_t max_4k_pages);
} fm_hooks;

typedef struct fm_config {
    /* Root 2 MiB frames, in a caller-provided array, each 2 MiB aligned.
     * Root metadata comes from bootstrap-reserved RAM, NOT the root free pool. */
    const uint64_t *root_physical_frames;
    size_t root_count;
    uint64_t *root_metadata_va;
    size_t root_metadata_entries; /* >= root_count */

    /* Pre-reserved, page-aligned, contiguous virtual address range.
     * Each transferred 2 MiB frame maps exactly ONE metadata page into it. */
    uint64_t *metadata_4k_va;
    size_t metadata_4k_pages_reserved;

    /* Loaded persistent high water, measured in usable 4 KiB frames (511/chunk).
     * On bootstrap we transfer as many chunks as possible to match this value. */
    uint64_t saved_high_water_pages;
    fm_hooks hooks;
} fm_config;

typedef struct fm_manager {
    fm_mutex_t mutex;
    uint64_t *root_meta;
    size_t root_capacity;
    size_t root_free;

    uint64_t *meta_4k;
    size_t chunks_capacity;
    size_t chunks_owned;
    size_t free_4k;
    /* TOP cursor: next free-stack insertion slot in contiguous metadata VA.
     * top_4k_offset is 1..512 (512 means one-past-last on a metadata page).
     * Index 0 is always reserved for the metadata page's own PTE.
     * Both stay synchronized with free_4k; synchronization is caller/build-specific. */
    uint64_t *top_4k_next;
    unsigned top_4k_offset;
    uint64_t high_water_4k_pages;
    bool boot_degraded;
    fm_hooks hooks;
} fm_manager;

typedef struct fm_stats {
    size_t free_2m;
    size_t chunks_owned;
    size_t total_4k_frames; /* 511 * chunks_owned */
    size_t free_4k;
    size_t reserved_4k_meta_pages;
    uint64_t high_water_4k_pages;
    bool boot_degraded;
} fm_stats;

/* fm_init(): bootstraps 4 KiB pool to historical maximum if possible.
 * On memory shortage, succeeds with stats.boot_degraded=true and preserves HWM. */
int fm_init(fm_manager *mgr, const fm_config *config);
void fm_destroy(fm_manager *mgr);
void fm_get_stats(fm_manager *mgr, fm_stats *out);

/* Unified TOP-only, LIFO allocation, for arbitrary N incl. 512 and N>512.
 * For physical allocator the entries returned are ready-to-use PDE/PTE words.
 * Extract physical addresses using fm_phys_2m()/fm_phys_4k().
 * Allocations don't require contiguous physical addresses.
 * A failed allocation NEVER changes the out_entries buffer. */
int fm_alloc_2m(fm_manager *mgr, size_t count, uint64_t *out_entries);
int fm_alloc_4k(fm_manager *mgr, size_t count, uint64_t *out_entries);

/* Direct map allocation: pop ready-made 4 KiB PTEs from the TOP cursor
 * straight into an already-allocated, writable leaf page-table array.
 * The caller must first create all intermediate paging levels, and map
 * those leaf page-table pages contiguously in its own address space.
 * The destination entries must ALL be zero (unmapped); any occupied slot
 * fails atomically without consuming a frame. Successful calls publish
 * PTEs directly, without an intermediate physical-address/output array.
 * Each entry maps the next physical frame (not necessarily contiguous).
 * No TLB maintenance is performed: caller must invalidate a previously
 * used virtual address before reuse, and perform any required SMP shootdown.
 * Only leaf PTEs are installed; page-table-page allocation is not included.
 */
int fm_alloc_map_4k(fm_manager *mgr, size_t count, uint64_t *leaf_ptes);

/* Free uses PHYSICAL addresses, which are converted to default PDE/PTE entries.
 * Neither pool tracks allocated frames: ownership and avoiding double-free are
 * the responsibility of the caller. Invalid alignment is rejected atomically. */
int fm_free_2m(fm_manager *mgr, size_t count, const uint64_t *physical);
int fm_free_4k(fm_manager *mgr, size_t count, const uint64_t *physical);

/* Explicitly add one 2 MiB chunk to 4 KiB pool (511 frames + 1 meta page). */
int fm_expand_4k(fm_manager *mgr);

/* Lookup one physical 4 KiB metadata page by its continuous VA index. */
uint64_t fm_4k_metadata_phys(fm_manager *mgr, size_t index);

static inline uint64_t fm_phys_4k(uint64_t pte) {
    return pte & FM_PHYS_MASK_4K;
}
static inline uint64_t fm_phys_2m(uint64_t pde) {
    return pde & FM_PHYS_MASK_2M;
}

#ifdef __cplusplus
}
#endif
#endif
