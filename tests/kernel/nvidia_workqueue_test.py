"""Run the RM queue adapter with deterministic port/deferred boundary stubs."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/drivers/gpu/nvidia/rm/nv_rm_workqueue.c').read_text()
assert 'liteos' not in source.lower()
# 仅在宿主测试夹具边界替换供应商头文件和内核头文件。
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int kstatus_t;
typedef int NV_STATUS;
typedef unsigned NvU32;
typedef bool NvBool;
#define NV_API_CALL
enum { K_OK, K_EINVAL, K_EAGAIN, K_ENOSPC };
enum { NV_OK, NV_ERR_OPERATING_SYSTEM, NV_ERR_NOT_SUPPORTED,
       NV_ERR_NOT_READY, NV_ERR_INSUFFICIENT_RESOURCES };
#define NV_TRUE true
#define NV_FALSE false
struct os_work_queue { int unused; };
typedef struct { unsigned char stack[128]; NvU32 size; void *top; } nvidia_stack_t;
typedef struct { atomic_uint state; } nv_port_once_t;
typedef pthread_mutex_t nv_port_spinlock_t;
enum { NV_PORT_WORK_IDLE, NV_PORT_WORK_QUEUED, NV_PORT_WORK_RUNNING, NV_PORT_WORK_CANCELLED };
typedef struct { atomic_uint state; void (*fn)(void *); void *arg; } nv_port_work_t;
static void nv_port_spin_init(nv_port_spinlock_t *p) { assert(!pthread_mutex_init(p, NULL)); }
static void nv_port_spin_lock(nv_port_spinlock_t *p) { assert(!pthread_mutex_lock(p)); }
static void nv_port_spin_unlock(nv_port_spinlock_t *p) { assert(!pthread_mutex_unlock(p)); }
static kstatus_t nv_port_once_run(nv_port_once_t *p, void (*fn)(void *), void *arg) {
    if (!atomic_load(&p->state)) { fn(arg); atomic_store(&p->state, 1); } return K_OK;
}
static bool worker_started = true, block_drain, drain_entered, release_drain;
static bool fail_drain, early_stall;
static unsigned executed;
static kstatus_t queue_result;
static nv_port_work_t *queued[32];
static unsigned queued_count;
static pthread_mutex_t barrier = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool deferred_worker_started(void) { return worker_started; }
static void run(nv_port_work_t *w) {
    if (atomic_load(&w->state) != NV_PORT_WORK_QUEUED) return;
    atomic_store(&w->state, NV_PORT_WORK_RUNNING); w->fn(w->arg);
    atomic_store(&w->state, NV_PORT_WORK_IDLE);
}
static uint32_t deferred_run(uint32_t budget) {
    assert(budget == 64); if (early_stall) return 0;
    for (unsigned i = 0; i < queued_count; ++i) run(queued[i]);
    queued_count = 0; return 1;
}
static void nv_port_work_init(nv_port_work_t *w, void (*fn)(void *), void *arg) {
    atomic_init(&w->state, NV_PORT_WORK_IDLE); w->fn = fn; w->arg = arg;
}
static kstatus_t nv_port_work_queue(nv_port_work_t *w) {
    if (queue_result) return queue_result;
    assert(queued_count < 32); queued[queued_count++] = w;
    atomic_store(&w->state, NV_PORT_WORK_QUEUED); return K_OK;
}
static kstatus_t nv_port_work_drain(nv_port_work_t *w, uint64_t timeout) {
    assert(timeout == UINT64_MAX);
    pthread_mutex_lock(&barrier);
    if (block_drain) {
        drain_entered = true; pthread_cond_broadcast(&changed);
        while (!release_drain) pthread_cond_wait(&changed, &barrier);
    }
    pthread_mutex_unlock(&barrier);
    if (fail_drain) return K_EAGAIN;
    run(w); return K_OK;
}
static void rm_execute_work_item(nvidia_stack_t *s, void *data) {
    assert(s->size == sizeof(s->stack) && s->top == s->stack + sizeof(s->stack));
    assert(data); ++executed;
}
''' + source + r'''
static void *flush(void *unused) {
    (void)unused; assert(os_flush_work_queue(NULL, NV_FALSE) == NV_OK); return NULL;
}
int main(void) {
    int data = 1; struct os_work_queue private_queue;
    nv_rm_work_entry_t cancelled = {0};
    atomic_init(&cancelled.work.state, NV_PORT_WORK_CANCELLED);
    assert(nv_rm_work_drain_entry(NULL) == K_EINVAL);
    assert(nv_rm_work_drain_entry(&cancelled) == K_OK);
    atomic_store(&cancelled.work.state, NV_PORT_WORK_IDLE);
    assert(nv_rm_work_drain_entry(&cancelled) == K_OK);
    assert(os_queue_work_item(&private_queue, &data) == NV_ERR_NOT_SUPPORTED);
    assert(os_queue_work_item(NULL, NULL) == NV_ERR_NOT_SUPPORTED);
    assert(os_flush_work_queue(&private_queue, NV_FALSE) == NV_ERR_NOT_SUPPORTED);
    for (unsigned round = 0; round < 4; ++round) {
        for (unsigned i = 0; i < 32; ++i) assert(os_queue_work_item(NULL, &data) == NV_OK);
        assert(os_queue_work_item(NULL, &data) == NV_ERR_INSUFFICIENT_RESOURCES);
        assert(os_flush_work_queue(NULL, NV_FALSE) == NV_OK); queued_count = 0;
    }
    assert(executed == 128);
    queue_result = K_ENOSPC;
    assert(os_queue_work_item(NULL, &data) == NV_ERR_INSUFFICIENT_RESOURCES);
    queue_result = K_EAGAIN;
    assert(os_queue_work_item(NULL, &data) == NV_ERR_OPERATING_SYSTEM);
    queue_result = K_OK;
    assert(os_queue_work_item(NULL, &data) == NV_OK);
    block_drain = true;
    pthread_t thread; assert(!pthread_create(&thread, NULL, flush, NULL));
    pthread_mutex_lock(&barrier);
    while (!drain_entered) pthread_cond_wait(&changed, &barrier);
    pthread_mutex_unlock(&barrier);
    assert(os_is_queue_flush_ongoing(NULL));
    assert(os_flush_work_queue(NULL, NV_FALSE) == NV_ERR_NOT_READY);
    assert(os_is_queue_flush_ongoing(NULL));
    assert(os_queue_work_item(NULL, &data) == NV_ERR_NOT_READY);
    pthread_mutex_lock(&barrier); release_drain = true;
    pthread_cond_broadcast(&changed); pthread_mutex_unlock(&barrier);
    assert(!pthread_join(thread, NULL)); block_drain = false; queued_count = 0;
    assert(!os_is_queue_flush_ongoing(NULL) && executed == 129);
    assert(os_queue_work_item(NULL, &data) == NV_OK);
    fail_drain = true;
    assert(os_flush_work_queue(NULL, NV_FALSE) == NV_ERR_OPERATING_SYSTEM);
    assert(!os_is_queue_flush_ongoing(NULL));
    fail_drain = false; assert(os_flush_work_queue(NULL, NV_FALSE) == NV_OK); queued_count = 0;
    worker_started = false;
    assert(os_queue_work_item(NULL, &data) == NV_OK); early_stall = true;
    assert(os_flush_work_queue(NULL, NV_FALSE) == NV_ERR_OPERATING_SYSTEM);
    early_stall = false;
    assert(os_flush_work_queue(NULL, NV_FALSE) == NV_OK);
    assert(executed == 131 && !os_is_queue_flush_ongoing(NULL));
    puts("NVIDIA_RM_WORKQUEUE_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='nvidia-rm-queue-') as tmp:
    fixture = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    fixture.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
