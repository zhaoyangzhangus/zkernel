"""Exercise the production first-OOM snapshot without allocating or waiting."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/backend.c').read_text()
snapshot = source[source.index('struct {\n    uint64_t failures;'):
                  source.index('static kstatus_t vfs_memory_reserve(')]
program = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int kstatus_t;
typedef struct { uint64_t size, data_capacity; } vfs_backend_node_t;
static unsigned stats_calls, entered, release_stats;
static bool fail_stats;
static kstatus_t page_memory_get_stats(uint64_t *total, uint64_t *available) {
    __atomic_fetch_add(&stats_calls, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&entered, 1U, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release_stats, __ATOMIC_ACQUIRE)) { }
    if (fail_stats) return -1;
    *total=1000; *available=750; return 0;
}
''' + snapshot + r'''
static void *first(void *unused) {
    (void)unused;
    vfs_backend_node_t backend={17,4096};
    vfs_memory_note_failure(&backend,2097152,2097152);
    return 0;
}
static void *later(void *unused) {
    (void)unused;
    vfs_backend_node_t backend={23,8192};
    for (unsigned i=0;i<10000;++i)
        vfs_memory_note_failure(&backend,8388608,8388608);
    return 0;
}
int main(void) {
    pthread_t writer, competitors[8];
    assert(!pthread_create(&writer,0,first,0));
    while (!__atomic_load_n(&entered,__ATOMIC_ACQUIRE)) { }
    assert(__atomic_load_n(&g_vfs_memory_allocation_failure.state,
                           __ATOMIC_ACQUIRE)==1);
    for(unsigned i=0;i<8;++i)
        assert(!pthread_create(&competitors[i],0,later,0));
    /* Competitors must finish even while the snapshot owner is held here. */
    for(unsigned i=0;i<8;++i)assert(!pthread_join(competitors[i],0));
    assert(__atomic_load_n(&stats_calls,__ATOMIC_RELAXED)==1);
    __atomic_store_n(&release_stats,1U,__ATOMIC_RELEASE);
    assert(!pthread_join(writer,0));
    assert(__atomic_load_n(&g_vfs_memory_allocation_failure.state,
                           __ATOMIC_ACQUIRE)==2);
    assert(g_vfs_memory_allocation_failure.failures==80001);
    assert(g_vfs_memory_allocation_failure.required==2097152);
    assert(g_vfs_memory_allocation_failure.attempted_capacity==2097152);
    assert(g_vfs_memory_allocation_failure.previous_size==17);
    assert(g_vfs_memory_allocation_failure.previous_capacity==4096);
    assert(g_vfs_memory_allocation_failure.total_pages==1000);
    assert(g_vfs_memory_allocation_failure.free_pages==750);
    assert(g_vfs_memory_allocation_failure.stats_status==0);
    /* No active writers: simulate a fresh boot with unavailable page stats. */
    memset(&g_vfs_memory_allocation_failure,0,
           sizeof(g_vfs_memory_allocation_failure));
    fail_stats=true;
    first(0);
    assert(g_vfs_memory_allocation_failure.state==2);
    assert(g_vfs_memory_allocation_failure.failures==1);
    assert(g_vfs_memory_allocation_failure.stats_status==-1);
    assert(!g_vfs_memory_allocation_failure.total_pages);
    assert(!g_vfs_memory_allocation_failure.free_pages);
    puts("PASS: first failure retained, 80000 concurrent failures do not wait, failed stats explicit");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-oom-snapshot-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=15)
