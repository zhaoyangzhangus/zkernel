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
    fm_stats s;
    fm_get_stats(&r.mgr, &s);
    CHECK(s.chunks_owned == 0 && s.free_2m == 20);

    uint64_t a;
    CHECK(fm_alloc_4k(&r.mgr, 1, &a) == FM_OK);
    CHECK((a & FM_PTE_FLAGS) == FM_PTE_FLAGS);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.chunks_owned == 1 && s.free_4k == 510 && s.free_2m == 19);
    uint64_t meta0 = fm_4k_metadata_phys(&r.mgr, 0);
    CHECK(meta0 % FM_2M_SIZE == 0);
    CHECK(phys(a) == meta0 + 511 * FM_4K_SIZE); /* newest PTE first */
    CHECK(r.sim.metadata_4k_va[0] == (meta0 | FM_PTE_FLAGS));
    void *alias = fm_sim_map_alias(&r.sim, meta0);
    CHECK(alias != MAP_FAILED);
    CHECK(((uint64_t *)alias)[0] == r.sim.metadata_4k_va[0]);
    CHECK(((uint64_t *)alias)[511] == r.sim.metadata_4k_va[511]);
    fm_sim_unmap_alias(alias);

    uint64_t b[510];
    CHECK(fm_alloc_4k(&r.mgr, ARRAYSIZE(b), b) == FM_OK);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.free_4k == 0);
    for (size_t i = 0; i < ARRAYSIZE(b); ++i)
        CHECK(phys(b[i]) == meta0 + (510 - i) * FM_4K_SIZE);

    uint64_t c[512];
    CHECK(fm_alloc_4k(&r.mgr, ARRAYSIZE(c), c) == FM_OK);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.chunks_owned == 3 && s.free_4k == 510 && s.free_2m == 17);
    CHECK(fm_4k_metadata_phys(&r.mgr, 1) != fm_4k_metadata_phys(&r.mgr, 2));
    CHECK((uint8_t *)r.sim.metadata_4k_va + FM_4K_SIZE ==
          (uint8_t *)&r.sim.metadata_4k_va[512]);

    uint64_t d[1025];
    CHECK(fm_alloc_4k(&r.mgr, ARRAYSIZE(d), d) == FM_OK);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.chunks_owned == 5 && s.free_4k == 507);
    CHECK(s.total_4k_frames == 5 * 511);
    CHECK(s.high_water_4k_pages == 5 * 511);

    uint64_t freed[4] = {phys(a), phys(c[0]), phys(b[15]), phys(d[27])};
    CHECK(fm_free_4k(&r.mgr, 4, freed) == FM_OK);
    uint64_t again[4];
    CHECK(fm_alloc_4k(&r.mgr, 4, again) == FM_OK);
    for (int i = 0; i < 4; ++i)
        CHECK(phys(again[i]) == freed[3-i]);
    CHECK(fm_free_4k(&r.mgr, 4, freed) == FM_OK);
    /* The caller is responsible for not double-freeing. */
    uint64_t bad_phys[2] = {phys(a), phys(b[0]) + 1};
    fm_get_stats(&r.mgr, &s);
    size_t old_free = s.free_4k;
    CHECK(fm_free_4k(&r.mgr, 2, bad_phys) == FM_EINVAL);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.free_4k == old_free);

    printf("[OK] 4K: 1 / 510 / 512 / 1025, metadata alias, LIFO, alignment\n");
    rig_close(&r);
}

/* A full 64-entry stage is merged in one batch.  Place the main TOP
 * eleven entries before the 511-slot boundary to exercise wraparound. */
static void test_staged_free(void) {
    rig r;
    rig_new(&r, 4);
    CHECK(fm_expand_4k(&r.mgr) == FM_OK);
    CHECK(fm_expand_4k(&r.mgr) == FM_OK);
    uint64_t entries[522], addresses[522], again[64];
    CHECK(fm_alloc_4k(&r.mgr, ARRAYSIZE(entries), entries) == FM_OK);
    for (size_t i = 0; i < ARRAYSIZE(entries); ++i)
        addresses[i] = phys(entries[i]);
    fm_stats st;
    fm_get_stats(&r.mgr, &st);
    CHECK(st.free_4k == 500 && st.main_free_4k == 500 && st.staged_4k == 0);
    CHECK(r.mgr.top_4k_offset == 501);

    /* 63 releases do not touch the main metadata TOP. */
    CHECK(fm_free_4k(&r.mgr, 63, addresses) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.staged_4k == 63 && st.main_free_4k == 500 && st.free_4k == 563);
    CHECK(r.mgr.top_4k_offset == 501);
    CHECK(fm_free_4k(&r.mgr, 1, &addresses[63]) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.staged_4k == 0 && st.main_free_4k == 564 && st.free_4k == 564);
    CHECK(r.mgr.top_4k_offset == 54); /* 501 -> 512 -> next metadata page */

    CHECK(fm_alloc_4k(&r.mgr, 64, again) == FM_OK);
    for (size_t i = 0; i < 64; ++i)
        CHECK(phys(again[i]) == addresses[63 - i]);

    /* Direct mapping must consume the staged entries without flushing them. */
    CHECK(fm_free_4k(&r.mgr, 7, addresses) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.staged_4k == 7 && st.main_free_4k == 500);
    uint64_t leaf[7] = {0};
    CHECK(fm_alloc_map_4k(&r.mgr, 7, leaf) == FM_OK);
    for (size_t i = 0; i < 7; ++i) {
        CHECK(phys(leaf[i]) == addresses[6 - i]);
        leaf[i] = 0;
    }
    CHECK(fm_free_4k(&r.mgr, 7, addresses) == FM_OK);
    CHECK(fm_flush_staged_4k(&r.mgr) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.staged_4k == 0 && st.main_free_4k == 507);
    CHECK(fm_flush_staged_4k(&r.mgr) == FM_OK); /* idempotent */
    uint64_t bad = addresses[64] + 1;
    CHECK(fm_free_4k(&r.mgr, 1, &bad) == FM_EINVAL);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.staged_4k == 0 && st.free_4k == 507);
    CHECK(fm_free_4k(&r.mgr, ARRAYSIZE(entries) - 64,
                     addresses + 64) == FM_OK);
    /* Return the other 57 allocated pages from the first flushed batch. */
    CHECK(fm_free_4k(&r.mgr, 57, addresses + 7) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.free_4k == 1022 && st.free_4k == st.main_free_4k + st.staged_4k);
    CHECK(fm_flush_staged_4k(&r.mgr) == FM_OK);
    fm_get_stats(&r.mgr, &st);
    CHECK(st.main_free_4k == 1022 && st.staged_4k == 0);
    printf("[OK] staged free: 63/64 threshold, cross-page flush, direct PTE, explicit flush\n");
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
    CHECK(s.free_2m == 8 && s.chunks_owned == 0);

    r.sim.fail_next_persist = 1;
    CHECK(fm_alloc_4k(&r.mgr, 1, &sentinel) == FM_EPERSIST);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.free_2m == 8 && s.chunks_owned == 0);
    CHECK(r.sim.persist.high_water_pages == 0);

    CHECK(fm_alloc_4k(&r.mgr, 1, &sentinel) == FM_OK);
    CHECK(sentinel != UINT64_C(0xfacefeeddeadbeef));
    fm_get_stats(&r.mgr, &s);
    CHECK(s.free_2m == 7 && s.chunks_owned == 1);
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
    CHECK(s.high_water_4k_pages == 1533 && s.chunks_owned == 3);
    uint64_t old_meta = fm_4k_metadata_phys(&r.mgr, 0);
    rig_stop(&r);

    /* Simulate reboot with a DIFFERENT physical frame selection order. */
    CHECK(fm_sim_open(&r.sim, 12, r.path) == 0);
    CHECK(r.sim.persist.high_water_pages == 1533);
    uint64_t tmp = r.sim.root_physical[0];
    r.sim.root_physical[0] = r.sim.root_physical[11];
    r.sim.root_physical[11] = tmp;
    fm_config config = fm_sim_config(&r.sim);
    CHECK(fm_init(&r.mgr, &config) == FM_OK);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.chunks_owned == 3 && s.total_4k_frames == 1533 &&
          s.free_4k == 1533 && s.free_2m == 9 && !s.boot_degraded);
    CHECK(fm_4k_metadata_phys(&r.mgr, 0) != old_meta);
    uint64_t all[1533];
    CHECK(fm_alloc_4k(&r.mgr, 1533, all) == FM_OK);
    CHECK(fm_alloc_4k(&r.mgr, 1, all) == FM_OK);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.high_water_4k_pages == 2044 && s.chunks_owned == 4);
    rig_stop(&r);

    /* Reboot with LESS memory than recorded historical maximum. */
    rig_start(&r, 2);
    fm_get_stats(&r.mgr, &s);
    CHECK(s.boot_degraded && s.chunks_owned == 2 &&
          s.high_water_4k_pages == 2044 && s.free_2m == 0);
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
    test_staged_free();
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
