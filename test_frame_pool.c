#define _GNU_SOURCE
#include "frame_pool.h"
#include "sim_host.h"
#include "persist.h"
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    abort(); } } while (0)
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))

typedef struct rig {
    char path[64];
    fm_sim sim;
    fm_manager mgr;
} rig;

static void temp_path(char path[64]) {
    strcpy(path, "/tmp/zkernel-hwm-XXXXXX");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
}
static void rig_start(rig *r, size_t frames) {
    CHECK(fm_sim_open(&r->sim, frames, r->path) == 0);
    fm_config cfg = fm_sim_config(&r->sim);
    CHECK(fm_init(&r->mgr, &cfg) == FM_OK);
}
static void rig_stop(rig *r) {
    fm_destroy(&r->mgr);
    fm_sim_close(&r->sim);
}
static void rig_new(rig *r, size_t frames) {
    memset(r, 0, sizeof(*r));
    temp_path(r->path);
    rig_start(r, frames);
}
static void rig_close(rig *r) {
    rig_stop(r);
    unlink(r->path);
}
static uint64_t phys(uint64_t pte) { return fm_phys_4k(pte); }
static void test_4k_boundary(void) {
    rig r;
    rig_new(&r, 20);
    fm_stats st;
    fm_get_stats(&r.mgr, &st);
    CHECK(st.chunks_owned == 0 && st.free_2m == 20);
    CHECK(st.backed_4k_meta_pages == 0 &&
          st.backed_4k_meta_end_va == (uintptr_t)r.sim.metadata_4k_va);
    uint64_t a;
    CHECK(fm_alloc_4k(&r.mgr, 1, &a) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    /* A 512-FRAME PCP refill requires two 511-frame metadata chunks. */
    CHECK(st.chunks_owned == 2 && st.free_2m == 18);
    CHECK(st.pcp_refills == 1 && st.pcp_free_4k == 511);
    CHECK(st.main_free_4k == 510 && st.free_4k == 1021);
    CHECK(st.backed_4k_meta_pages == 2 &&
          st.backed_4k_meta_end_va ==
              (uintptr_t)(r.sim.metadata_4k_va + 2 * FM_ENTRIES_PER_META));
    uint64_t meta0 = fm_4k_metadata_phys(&r.mgr, 0);
    uint64_t meta1 = fm_4k_metadata_phys(&r.mgr, 1);
    CHECK(meta0 % FM_2M_SIZE == 0 && meta1 % FM_2M_SIZE == 0);
    CHECK(phys(a) == meta1 + 511 * FM_4K_SIZE);
    CHECK(r.sim.metadata_4k_va[0] == (meta0 | FM_PTE_FLAGS));
    CHECK(r.sim.metadata_4k_va[512] == (meta1 | FM_PTE_FLAGS));
    void *alias = fm_sim_map_alias(&r.sim, meta0);
    CHECK(alias != MAP_FAILED);
    CHECK(((uint64_t *)alias)[0] == r.sim.metadata_4k_va[0]);
    CHECK(((uint64_t *)alias)[511] == r.sim.metadata_4k_va[511]);
    fm_sim_unmap_alias(alias);

    uint64_t b[510];
    CHECK(fm_alloc_4k(&r.mgr, ARRAYSIZE(b), b) == FM_OK);
    for (size_t i = 0; i < ARRAYSIZE(b); ++i)
        CHECK(phys(b[i]) == meta1 + (510 - i) * FM_4K_SIZE);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.pcp_free_4k == 1 && st.main_free_4k == 510);
    uint64_t c[512];
    CHECK(fm_alloc_4k(&r.mgr, ARRAYSIZE(c), c) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.chunks_owned == 3 && st.free_4k == 510 && st.pcp_free_4k == 1);
    CHECK(st.pcp_refills == 2);
    CHECK(fm_4k_metadata_phys(&r.mgr, 2) != meta1);
    uint64_t d[1025];
    CHECK(fm_alloc_4k(&r.mgr, ARRAYSIZE(d), d) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.chunks_owned == 5 && st.free_4k == 507);
    CHECK(st.total_4k_frames == 5 * 511);
    CHECK(st.high_water_4k_pages == 5 * 511);

    uint64_t freed[4] = {phys(a), phys(c[0]), phys(b[15]), phys(d[27])};
    CHECK(fm_free_4k(&r.mgr, 4, freed) == FM_OK);
    uint64_t again[4];
    CHECK(fm_alloc_4k(&r.mgr, 4, again) == FM_OK);
    for (int i = 0; i < 4; ++i)
        CHECK(phys(again[i]) == freed[3-i]);
    CHECK(fm_free_4k(&r.mgr, 4, freed) == FM_OK);
    uint64_t bad_phys[2] = {phys(a), phys(b[0]) + 1};
    fm_get_stats(&r.mgr, &st);
    size_t old_free = st.free_4k;
    CHECK(fm_free_4k(&r.mgr, 2, bad_phys) == FM_EINVAL);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.free_4k == old_free);
    printf("[OK] 4K PCP: 1 / 510 / 512 / 1025, metadata alias, LIFO, 512-only refills\n");
    rig_close(&r);
}

/* PCP slots are actual virtual mappings backed by the PCP leaf page table.
 * Verify that free maps the frame at a PCP VA and allocation removes that PTE. */
static void test_pcp_virtual_mapping(void) {
    rig r;
    rig_new(&r, 8);
    uint64_t entry;
    CHECK(fm_alloc_4k(&r.mgr, 1, &entry) == FM_OK);
    uint64_t pa = phys(entry);
    CHECK(fm_free_4k(&r.mgr, 1, &pa) == FM_OK);
    size_t slot = (r.mgr.pcp.head + r.mgr.pcp.count - 1) % FM_PCP_CAPACITY;
    CHECK(r.sim.pcp_leaf_ptes[slot] == entry);
    volatile uint64_t *pcp_va = (volatile uint64_t *)
        ((uintptr_t)r.sim.pcp_virtual_va + slot * FM_4K_SIZE);
    void *physical_alias = fm_sim_map_alias(&r.sim, pa);
    CHECK(physical_alias != MAP_FAILED);
    uint64_t old = *(volatile uint64_t *)physical_alias;
    uint64_t pattern = UINT64_C(0x9ac5f007afebc012) ^ pa;
    *pcp_va = pattern;
    CHECK(*(volatile uint64_t *)physical_alias == pattern);
    *pcp_va = old;
    /* mmap() does not manage page-table A/D bits. Emulate the x86 hardware
     * update caused by the PCP virtual write before transferring the PTE. */
    r.sim.pcp_leaf_ptes[slot] |= FM_X86_AD;
    fm_sim_unmap_alias(physical_alias);

    uint64_t again;
    CHECK(fm_alloc_4k(&r.mgr, 1, &again) == FM_OK &&
          (again & ~FM_X86_AD) == entry);
    CHECK((again & FM_X86_AD) == FM_X86_AD);
    CHECK(r.sim.pcp_leaf_ptes[slot] == 0);
    CHECK(fm_free_4k(&r.mgr, 1, &pa) == FM_OK);
    fm_stats st;
    fm_get_stats(&r.mgr, &st);
    CHECK(st.free_4k == st.total_4k_frames);
    CHECK(!r.sim.fail_next_map);
    printf("[OK] PCP virtual mapping: PTE-backed cache slot aliases frame; alloc clears slot\n");
    rig_close(&r);
}

/* Main <-> PCP transfers are ALWAYS 512 USABLE PTEs; capacity is 1024.
 * Verify 511->512, 1023->1024, oldest-512 drain, direct PTE and backing. */
static void test_pcp_handoff(void) {
    rig r;
    rig_new(&r, 6);
    CHECK(fm_expand_4k(&r.mgr) == FM_OK);
    fm_stats st;
    fm_get_stats(&r.mgr, &st);
    CHECK(st.backed_4k_meta_pages == 1 && st.main_free_4k == 511);
    CHECK(st.backed_4k_meta_end_va ==
          (uintptr_t)(r.sim.metadata_4k_va + FM_ENTRIES_PER_META));
    /* Strict 512-frame transfer cannot pull from a lone 511-frame chunk:
     * the first refill must grow another physical metadata chunk. */
    uint64_t first;
    CHECK(fm_alloc_4k(&r.mgr, 1, &first) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.chunks_owned == 2 && st.pcp_refills == 1 && st.pcp_free_4k == 511);
    CHECK(st.main_free_4k == 510);

    uint64_t pre[511], pre_pa[511];
    CHECK(fm_alloc_4k(&r.mgr, 511, pre) == FM_OK);
    for (size_t i = 0; i < 511; ++i) pre_pa[i] = phys(pre[i]);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.pcp_free_4k == 0 && st.main_free_4k == 510);

    /* 511 partial releases don't touch the global pool. */
    CHECK(fm_free_4k(&r.mgr, 511, pre_pa) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.pcp_free_4k == 511 && st.main_free_4k == 510);
    CHECK(fm_pcp_drain_4k(&r.mgr) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.pcp_free_4k == 511 && st.main_free_4k == 510 && st.pcp_drains == 0);

    /* The 512th released frame allows an explicit whole batch transfer. */
    uint64_t first_pa = phys(first);
    CHECK(fm_free_4k(&r.mgr, 1, &first_pa) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.pcp_free_4k == 512 && st.main_free_4k == 510);
    CHECK(fm_pcp_drain_4k(&r.mgr) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.pcp_free_4k == 0 && st.main_free_4k == 1022 && st.pcp_drains == 1);

    /* Refill must consume precisely 512 frames, with LIFO intact. */
    uint64_t alloc[512], pa[512];
    CHECK(fm_alloc_4k(&r.mgr, 512, alloc) == FM_OK);
    for (size_t i = 0; i < 512; ++i) pa[i] = phys(alloc[i]);
    CHECK(pa[0] == first_pa);
    for (size_t i = 1; i < 512; ++i) CHECK(pa[i] == pre_pa[511-i]);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.pcp_free_4k == 0 && st.main_free_4k == 510 && st.pcp_refills == 2);
    CHECK(fm_free_4k(&r.mgr, 512, pa) == FM_OK);
    CHECK(fm_pcp_drain_4k(&r.mgr) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.pcp_free_4k == 0 && st.main_free_4k == 1022 && st.pcp_drains == 2);

    /* Allocate 1024 distinct frames from global in 2 x 512 batches.
     * Their release fills PCP to the limit; it drains EXACTLY 512 oldest,
     * retaining 512 newest and leaving no allocated frames. */
    uint64_t all[1024], all_pa[1024];
    CHECK(fm_alloc_4k(&r.mgr, 1024, all) == FM_OK);
    for (size_t i=0;i<1024;++i) all_pa[i] = phys(all[i]);
    fm_get_stats(&r.mgr,&st);
    CHECK(st.chunks_owned == 3 && st.pcp_free_4k == 0 &&
          st.main_free_4k == 509 && st.pcp_refills == 4);
    CHECK(fm_free_4k(&r.mgr, 1023, all_pa) == FM_OK);
    fm_get_stats(&r.mgr,&st);
    CHECK(st.pcp_free_4k == 1023 && st.main_free_4k == 509 && st.pcp_drains == 2);
    CHECK(fm_free_4k(&r.mgr, 1, &all_pa[1023]) == FM_OK);
    fm_get_stats(&r.mgr,&st);
    CHECK(st.pcp_free_4k == 512 && st.main_free_4k == 1021 && st.pcp_drains == 3);
    CHECK(st.free_4k == 1533 && st.total_4k_frames == 1533);
    CHECK(st.backed_4k_meta_pages == st.chunks_owned &&
          st.backed_4k_meta_end_va ==
            (uintptr_t)(r.sim.metadata_4k_va + st.chunks_owned * FM_ENTRIES_PER_META));

    /* Direct leaf PTE allocation uses PCP only, no global access. */
    uint64_t leaf[7] = {0};
    CHECK(fm_alloc_map_4k(&r.mgr, 7, leaf) == FM_OK);
    for (size_t i=0;i<7;++i) CHECK(phys(leaf[i]) == all_pa[1023-i]);
    CHECK(fm_free_unmap_4k(&r.mgr, 7, leaf,
                            UINT64_C(0x700000100000)) == FM_OK);
    for (size_t i=0;i<7;++i) CHECK(leaf[i] == 0);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.pcp_free_4k == 512 && st.main_free_4k == 1021 && st.pcp_drains == 3);
    CHECK(fm_pcp_drain_4k(&r.mgr) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.pcp_free_4k == 0 && st.main_free_4k == 1533 && st.pcp_drains == 4);
    uint64_t bad = pa[0] + 1;
    CHECK(fm_free_4k(&r.mgr, 1, &bad) == FM_EINVAL);
    printf("[OK] PCP: 1024 limit, 512-only transfers, LIFO, direct PTE, mapped metadata\n");
    rig_close(&r);
}

/* One 2 MiB chunk yields only 511 usable frames, which cannot satisfy
 * a strict 512-frame global-to-PCP refill. Failure must be atomic. */
static void test_pcp_one_chunk_shortage(void) {
    rig r;
    rig_new(&r, 1);
    CHECK(fm_expand_4k(&r.mgr) == FM_OK);
    uint64_t sentinel = UINT64_C(0xfeedface12345678);
    CHECK(fm_alloc_4k(&r.mgr, 1, &sentinel) == FM_ENOMEM);
    CHECK(sentinel == UINT64_C(0xfeedface12345678));
    fm_stats st;
    fm_get_stats(&r.mgr, &st);
    CHECK(st.main_free_4k == 511 && st.pcp_free_4k == 0 &&
          st.pcp_refills == 0 && st.chunks_owned == 1 &&
          st.backed_4k_meta_pages == 1);
    CHECK(fm_pcp_drain_4k(&r.mgr) == FM_OK);
    fm_get_stats(&r.mgr,&st);
    CHECK(st.pcp_drains == 0);
    printf("[OK] PCP: one 511-frame chunk cannot refill 512; atomic ENOMEM\n");
    rig_close(&r);
}

static void test_root_bulk(void) {
    rig r;
    rig_new(&r, 540);
    uint64_t pages[512], phys_pages[512];
    CHECK(fm_alloc_2m(&r.mgr, 512, pages) == FM_OK);
    for (size_t i = 0; i < ARRAYSIZE(pages); ++i) {
        CHECK((pages[i] & FM_PDE_FLAGS) == FM_PDE_FLAGS);
        phys_pages[i] = fm_phys_2m(pages[i]);
        CHECK((phys_pages[i] % FM_2M_SIZE) == 0);
        if (i) CHECK(phys_pages[i] != phys_pages[i-1]);
    }
    fm_stats s;
    fm_get_stats(&r.mgr, &s);
    CHECK(s.free_2m == 28);
    CHECK(fm_free_2m(&r.mgr, 512, phys_pages) == FM_OK);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.free_2m == 540);
    CHECK(fm_alloc_2m(&r.mgr, 1, pages) == FM_OK);
    CHECK(fm_phys_2m(pages[0]) == phys_pages[511]);
    printf("[OK] 2M: 512-item TOP allocation and LIFO restoration\n");
    rig_close(&r);
}

static void test_fail_and_rollback(void) {
    rig r;
    rig_new(&r, 8);
    uint64_t sentinel = UINT64_C(0xfacefeeddeadbeef);
    CHECK(fm_alloc_4k(&r.mgr, 5000, &sentinel) == FM_ENOMEM);
    CHECK(sentinel == UINT64_C(0xfacefeeddeadbeef));
    r.sim.fail_next_map = 1;
    CHECK(fm_alloc_4k(&r.mgr, 1, &sentinel) == FM_EMAP);
    CHECK(sentinel == UINT64_C(0xfacefeeddeadbeef));
    fm_stats s;
    fm_get_stats(&r.mgr, &s);
    CHECK(s.free_2m == 8 && s.chunks_owned == 0 &&
          s.backed_4k_meta_pages == 0 &&
          s.backed_4k_meta_end_va == (uintptr_t)r.sim.metadata_4k_va);

    r.sim.fail_next_persist = 1;
    CHECK(fm_alloc_4k(&r.mgr, 1, &sentinel) == FM_EPERSIST);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.free_2m == 8 && s.chunks_owned == 0 &&
          s.backed_4k_meta_pages == 0 &&
          s.backed_4k_meta_end_va == (uintptr_t)r.sim.metadata_4k_va);
    CHECK(r.sim.persist.high_water_pages == 0);

    CHECK(fm_alloc_4k(&r.mgr, 1, &sentinel) == FM_OK);
    CHECK(sentinel != UINT64_C(0xfacefeeddeadbeef));
    fm_get_stats(&r.mgr, &s);
    CHECK(s.free_2m == 6 && s.chunks_owned == 2);
    printf("[OK] errors: OOM, mapping rollback, HWM write rollback\n");
    rig_close(&r);
}

static void test_restart(void) {
    rig r;
    rig_new(&r, 12);
    uint64_t pages[1025];
    CHECK(fm_alloc_4k(&r.mgr, 1025, pages) == FM_OK);
    fm_stats s;
    fm_get_stats(&r.mgr, &s);
    CHECK(s.high_water_4k_pages == 2044 && s.chunks_owned == 4);
    uint64_t old_meta = fm_4k_metadata_phys(&r.mgr, 0);
    rig_stop(&r);

    /* Simulate reboot with a DIFFERENT physical frame selection order. */
    CHECK(fm_sim_open(&r.sim, 12, r.path) == 0);
    CHECK(r.sim.persist.high_water_pages == 2044);
    uint64_t tmp = r.sim.root_physical[0];
    r.sim.root_physical[0] = r.sim.root_physical[11];
    r.sim.root_physical[11] = tmp;
    fm_config config = fm_sim_config(&r.sim);
    CHECK(fm_init(&r.mgr, &config) == FM_OK);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.chunks_owned == 4 && s.total_4k_frames == 2044 &&
          s.free_4k == 2044 && s.free_2m == 8 && !s.boot_degraded);
    CHECK(fm_4k_metadata_phys(&r.mgr, 0) != old_meta);
    uint64_t all[2044];
    CHECK(fm_alloc_4k(&r.mgr, 2044, all) == FM_OK);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.high_water_4k_pages == 2555 && s.chunks_owned == 5);
    rig_stop(&r);

    /* Reboot with LESS memory than recorded historical maximum. */
    rig_start(&r, 2);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.boot_degraded && s.chunks_owned == 2 &&
          s.high_water_4k_pages == 2555 && s.free_2m == 0);
    printf("[OK] restart: HWM persists, rebases physical frames, degraded boot\n");
    rig_close(&r);
}

static uint32_t rng(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}

static void test_random(void) {
    rig r;
    rig_new(&r, 80);
    const size_t max_live = 40000;
    uint64_t *active = malloc(max_live * sizeof(uint64_t));
    uint8_t *seen = calloc(80 * 512, 1);
    CHECK(active && seen);
    size_t live = 0;
    uint32_t seed = 0x5eed1234u;
    for (int t = 0; t < 12000; ++t) {
        int want_alloc = (rng(&seed) & 3u) != 0;
        if (want_alloc && live + 800 < max_live) {
            size_t n = rng(&seed) % 800u + 1;
            uint64_t *entries = malloc(n * sizeof(uint64_t));
            CHECK(entries);
            int rc = fm_alloc_4k(&r.mgr, n, entries);
            if (rc == FM_OK) {
                for (size_t i = 0; i < n; ++i) {
                    uint64_t p = phys(entries[i]);
                    CHECK((p - r.sim.base_physical) % FM_2M_SIZE != 0);
                    size_t idx = (size_t)((p - r.sim.base_physical)/FM_4K_SIZE);
                    CHECK(idx < 80 * 512 && !seen[idx]);
                    seen[idx] = 1;
                    active[live++] = p;
                }
            } else CHECK(rc == FM_ENOMEM);
            free(entries);
        } else if (live) {
            size_t n = rng(&seed) % 200u + 1;
            if (n > live) n = live;
            uint64_t *returned = malloc(n * sizeof(uint64_t));
            CHECK(returned);
            for (size_t i = 0; i < n; ++i) {
                size_t at = rng(&seed) % live;
                uint64_t p = active[at];
                size_t idx = (size_t)((p - r.sim.base_physical)/FM_4K_SIZE);
                CHECK(seen[idx]);
                seen[idx] = 0;
                returned[i] = p;
                active[at] = active[--live];
            }
            CHECK(fm_free_4k(&r.mgr, n, returned) == FM_OK);
            free(returned);
        }
    }
    if (live) CHECK(fm_free_4k(&r.mgr, live, active) == FM_OK);
    fm_stats s;
    fm_get_stats(&r.mgr, &s);
    CHECK(s.free_4k == s.total_4k_frames);
    CHECK(s.chunks_owned <= 80 && s.high_water_4k_pages >= s.total_4k_frames);
    free(seen);
    free(active);
    printf("[OK] randomized: 12000 allocations/frees, no duplicate/metadata frames\n");
    rig_close(&r);
}

typedef struct thread_ctx {
    fm_manager *mgr;
    fm_sim *sim;
    pthread_mutex_t *seen_mutex;
    uint8_t *seen;
    uint32_t seed;
} thread_ctx;

static void *thread_run(void *arg) {
    thread_ctx *c = arg;
    for (int t = 0; t < 1500; ++t) {
        size_t n = rng(&c->seed) % 32 + 1;
        uint64_t entries[32], pa[32];
        CHECK(fm_alloc_4k(c->mgr, n, entries) == FM_OK);
        pthread_mutex_lock(c->seen_mutex);
        for (size_t i = 0; i < n; ++i) {
            pa[i] = phys(entries[i]);
            size_t at = (size_t)((pa[i] - c->sim->base_physical) / FM_4K_SIZE);
            CHECK(at < c->sim->chunks*512 && !c->seen[at]);
            c->seen[at] = 1;
        }
        pthread_mutex_unlock(c->seen_mutex);
        sched_yield();
        pthread_mutex_lock(c->seen_mutex);
        for (size_t i = 0; i < n; ++i) {
            size_t at = (size_t)((pa[i] - c->sim->base_physical) / FM_4K_SIZE);
            CHECK(c->seen[at]);
            c->seen[at] = 0;
        }
        pthread_mutex_unlock(c->seen_mutex);
        CHECK(fm_free_4k(c->mgr, n, pa) == FM_OK);
    }
    return NULL;
}
static void test_threads(void) {
    rig r;
    rig_new(&r, 8);
    pthread_t threads[4];
    uint8_t seen[8*512] = {0};
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    thread_ctx ctx[4];
    for (int i = 0; i < 4; ++i) {
        ctx[i] = (thread_ctx){&r.mgr, &r.sim, &mutex, seen,
                              (uint32_t)(0xdefabc00u + (unsigned)i*12345u)};
        CHECK(!pthread_create(&threads[i], NULL, thread_run, &ctx[i]));
    }
    for (int i = 0; i < 4; ++i) CHECK(!pthread_join(threads[i], NULL));
    fm_stats s;
    fm_get_stats(&r.mgr, &s);
    CHECK(s.free_4k == s.total_4k_frames);
    pthread_mutex_destroy(&mutex);
    printf("[OK] concurrency: 4 threads x 1500 cycles, ownership checks\n");
    rig_close(&r);
}

static void test_persist_fallback(void) {
    char path[64]; temp_path(path);
    fm_persist p;
    CHECK(fm_persist_open(&p, path) == 0);
    CHECK(fm_persist_store(&p, 511) == 0);
    CHECK(fm_persist_store(&p, 1022) == 0);
    CHECK(p.high_water_pages == 1022);
    /* The sequence 2 record is stored in slot zero. Corrupt only that slot. */
    unsigned char garbage[40] = {0};
    CHECK(pwrite(p.fd, garbage, sizeof(garbage), 0) == (ssize_t)sizeof(garbage));
    fm_persist_close(&p);
    CHECK(fm_persist_open(&p, path) == 0);
    CHECK(p.high_water_pages == 511);
    fm_persist_close(&p);
    unlink(path);
    printf("[OK] persistence: recovery from torn/corrupted latest record\n");
}

static uint64_t nanos(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static void bench_one(rig *r, size_t count, int iterations) {
    uint64_t *entries = malloc(count * sizeof(uint64_t));
    uint64_t *phys_list = malloc(count * sizeof(uint64_t));
    CHECK(entries && phys_list);
    uint64_t start = nanos();
    for (int j = 0; j < iterations; ++j) {
        CHECK(fm_alloc_4k(&r->mgr, count, entries) == FM_OK);
        for (size_t i = 0; i < count; ++i)
            phys_list[i] = phys(entries[i]);
        CHECK(fm_free_4k(&r->mgr, count, phys_list) == FM_OK);
    }
    uint64_t end = nanos();
    printf("  %4zu frames, allocation+free: %.1f ns/cycle (%.2f ns/frame)\n",
           count, (double)(end-start)/iterations,
           (double)(end-start)/(iterations*count));
    free(entries);
    free(phys_list);
}
static void benchmarks(void) {
    rig r;
    rig_new(&r, 40);
    for (int i = 0; i < 12; ++i) CHECK(fm_expand_4k(&r.mgr) == FM_OK);
    puts("[bench] current machine; includes mutex and return-list preparation; no page mapping in timed loop");
    bench_one(&r, 1, 150000);
    bench_one(&r, 511, 30000);
    bench_one(&r, 512, 30000);
    bench_one(&r, 1025, 15000);
    rig_close(&r);
}
int main(int argc, char **argv) {
    test_4k_boundary();
    test_pcp_virtual_mapping();
    test_pcp_handoff();
    test_pcp_one_chunk_shortage();
    test_root_bulk();
    test_fail_and_rollback();
    test_restart();
    test_random();
    test_threads();
    test_persist_fallback();
    puts("ALL TESTS PASSED");
    if (argc > 1 && strcmp(argv[1], "--bench") == 0)
        benchmarks();
    return 0;
}
