#define _GNU_SOURCE
#include "sim_host.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

static int map_meta(void *ctx, size_t i, uint64_t pa) {
    fm_sim *s = ctx;
    if (s->fail_next_map) { s->fail_next_map = 0; return -1; }
    if (i >= s->metadata_reserved || pa < s->base_physical ||
        ((pa - s->base_physical) % FM_4K_SIZE) != 0)
        return -1;
    uint64_t off = pa - s->base_physical;
    if (off > (uint64_t)s->chunks * FM_2M_SIZE - FM_4K_SIZE) return -1;
    void *va = (char *)s->metadata_4k_va + i * FM_4K_SIZE;
    void *p = mmap(va, FM_4K_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_FIXED, s->ram_fd, (off_t)off);
    return p == MAP_FAILED ? -1 : 0;
}

static void unmap_meta(void *ctx, size_t i) {
    fm_sim *s = ctx;
    if (i >= s->metadata_reserved) return;
    void *va = (char *)s->metadata_4k_va + i * FM_4K_SIZE;
    (void)mmap(va, FM_4K_SIZE, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
}

static void pcp_slot_changed(void *ctx, size_t slot, uint64_t va, uint64_t pte) {
    fm_sim *s = ctx;
    if (slot >= FM_PCP_CAPACITY ||
        va != (uintptr_t)s->pcp_virtual_va + slot * FM_4K_SIZE)
        return;
    void *target = (uint8_t *)s->pcp_virtual_va + slot * FM_4K_SIZE;
    if (pte & FM_X86_P) {
        uint64_t pa = fm_phys_4k(pte);
        if (pa < s->base_physical ||
            pa - s->base_physical > (uint64_t)s->chunks * FM_2M_SIZE - FM_4K_SIZE) {
            s->fail_next_map = 1;
            return;
        }
        void *mapped = mmap(target, FM_4K_SIZE, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_FIXED, s->ram_fd,
                            (off_t)(pa - s->base_physical));
        if (mapped == MAP_FAILED) s->fail_next_map = 1;
    } else {
        void *unmapped = mmap(target, FM_4K_SIZE, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (unmapped == MAP_FAILED) s->fail_next_map = 1;
    }
}

static void invalidate_page(void *ctx, uint64_t va) {
    (void)ctx;
    (void)va;
}

static int store_high_water(void *ctx, uint64_t high_water_pages) {
    fm_sim *s = ctx;
    if (s->fail_next_persist) { s->fail_next_persist = 0; return -1; }
    return fm_persist_store(&s->persist, high_water_pages);
}

int fm_sim_open(fm_sim *s, size_t chunks, const char *persist_path) {
    if (!s || !persist_path || !chunks || chunks > SIZE_MAX / FM_2M_SIZE)
        return -1;
    memset(s, 0, sizeof(*s));
    s->ram_fd = -1;
    s->persist.fd = -1;
    s->chunks = chunks;
    s->base_physical = FM_2M_SIZE;
    s->metadata_reserved = chunks;
    /* Sparse, file-backed simulated physical memory. A 1 GiB simulation
     * doesn't allocate 1 GiB real RAM: only touched pages are committed. */
    char ram_path[] = "/tmp/zkernel-ram-XXXXXX";
    s->ram_fd = mkstemp(ram_path);
    if (s->ram_fd < 0) goto fail;
    unlink(ram_path);
    if (ftruncate(s->ram_fd, (off_t)(chunks * FM_2M_SIZE))) goto fail;

    s->root_physical = malloc(chunks * sizeof(uint64_t));
    if (!s->root_physical) goto fail;
    for (size_t i = 0; i < chunks; ++i)
        s->root_physical[i] = s->base_physical + i * FM_2M_SIZE;

    size_t root_meta_bytes = (chunks * sizeof(uint64_t) + 4095u) & ~4095u;
    s->root_metadata = aligned_alloc(4096, root_meta_bytes);
    if (!s->root_metadata) goto fail;
    memset(s->root_metadata, 0, root_meta_bytes);

    s->metadata_4k_va = mmap(NULL, chunks * FM_4K_SIZE, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s->metadata_4k_va == MAP_FAILED) {
        s->metadata_4k_va = NULL;
        goto fail;
    }
    s->pcp_leaf_ptes = aligned_alloc(4096, 2 * 4096);
    if (!s->pcp_leaf_ptes) goto fail;
    memset(s->pcp_leaf_ptes, 0, 2 * 4096);
    s->pcp_virtual_va = mmap(NULL, FM_PCP_CAPACITY * FM_4K_SIZE, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s->pcp_virtual_va == MAP_FAILED) {
        s->pcp_virtual_va = NULL;
        goto fail;
    }
    if (fm_persist_open(&s->persist, persist_path)) goto fail;
    return 0;
fail:
    fm_sim_close(s);
    return -1;
}

fm_config fm_sim_config(fm_sim *s) {
    return (fm_config){
        .root_physical_frames = s->root_physical,
        .root_count = s->chunks,
        .root_metadata_va = s->root_metadata,
        .root_metadata_entries = s->chunks,
        .metadata_4k_va = s->metadata_4k_va,
        .metadata_4k_pages_reserved = s->metadata_reserved,
        .pcp_leaf_ptes = s->pcp_leaf_ptes,
        .pcp_virtual_base = (uintptr_t)s->pcp_virtual_va,
        .saved_high_water_pages = s->persist.high_water_pages,
        .hooks = {
            .context = s,
            .map_meta_page = map_meta,
            .unmap_meta_page = unmap_meta,
            .store_high_water = store_high_water,
            .pcp_slot_changed = pcp_slot_changed,
            .invalidate_page = invalidate_page
        }
    };
}

void *fm_sim_map_alias(fm_sim *s, uint64_t pa) {
    if (!s || pa < s->base_physical) return MAP_FAILED;
    uint64_t off = pa - s->base_physical;
    return mmap(NULL, FM_4K_SIZE, PROT_READ | PROT_WRITE,
                MAP_SHARED, s->ram_fd, (off_t)off);
}

void fm_sim_unmap_alias(void *p) {
    if (p && p != MAP_FAILED) munmap(p, FM_4K_SIZE);
}

void fm_sim_close(fm_sim *s) {
    if (!s) return;
    if (s->metadata_4k_va)
        munmap(s->metadata_4k_va, s->metadata_reserved * FM_4K_SIZE);
    if (s->pcp_virtual_va)
        munmap(s->pcp_virtual_va, FM_PCP_CAPACITY * FM_4K_SIZE);
    free(s->pcp_leaf_ptes);
    free(s->root_metadata);
    free(s->root_physical);
    fm_persist_close(&s->persist);
    if (s->ram_fd >= 0) close(s->ram_fd);
    memset(s, 0, sizeof(*s));
    s->ram_fd = -1;
    s->persist.fd = -1;
}
