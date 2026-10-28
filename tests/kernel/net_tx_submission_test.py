"""Exercise the actual socket TX boundary with concurrent callers and full rings."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/core.c').read_text()
start = source.index('kstatus_t e1000_transmit_status(')
end = source.index('e1000_state_t *e1000_controller_state(', start)
protocol = (root / 'kernel/drivers/net/protocol.c').read_text()
callback = protocol[protocol.index('kstatus_t e1000_device_transmit('):
                    protocol.index('static uint32_t e1000_ipv4_mask(')]
code = r'''
#include <kernel/base.h>
#include <kernel/spinlock.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#define E1000_BUFFER_SIZE 2048U
#define E1000_TX_STATUS_DD 1U
#define NET_BACKEND_E1000 1U
#define NET_BACKEND_RTL8126 2U
typedef struct { uint32_t options1; uint8_t status; } descriptor_t;
typedef struct {
    unsigned backend, tx_tail, rtl_tx_next;
    bool initialized;
    spinlock_t tx_lock;
    descriptor_t *tx_ring, *rtl_tx_ring;
    unsigned tx_ring_dma, rtl_tx_ring_dma;
} e1000_state_t;
static _Thread_local unsigned depth;
static atomic_uint active, submissions;
static bool backend_ok = true;
static void sched_preempt_disable(void) { ++depth; }
static void sched_preempt_enable(void) { assert(depth); --depth; }
static void dma_sync_for_cpu(void *mapping) { assert(mapping && depth); }
static bool e1000_transmit_locked(e1000_state_t *state,
                                  const uint8_t *frame, size_t length) {
    assert(state && frame && length && depth == 1);
    assert(!atomic_fetch_add(&active, 1));
    for (unsigned i = 0; i < 100; ++i) __asm__ volatile("pause");
    atomic_fetch_add(&submissions, 1);
    assert(atomic_fetch_sub(&active, 1) == 1);
    return backend_ok;
}
kstatus_t rtl8126_transmit_status(e1000_state_t *state,
                                 const uint8_t *frame, size_t length) {
    assert(state->backend==NET_BACKEND_RTL8126 && depth==1);
    if (!state->rtl_tx_ring) return K_EIO;
    dma_sync_for_cpu(&state->rtl_tx_ring_dma);
    if (state->rtl_tx_ring[state->rtl_tx_next].options1 & (1U<<31)) return K_EAGAIN;
    return e1000_transmit_locked(state,frame,length)?K_OK:K_EIO;
}
''' + source[start:end] + callback + r'''
static void *worker(void *opaque) {
    uint8_t frame = 1;
    for (unsigned i = 0; i < 10000; ++i) {
        assert(e1000_device_transmit(opaque, &frame, 1) == K_OK);
        assert(!depth);
    }
    return 0;
}
int main(void) {
    descriptor_t ring = {.status = 1};
    e1000_state_t state = {.initialized = true, .backend = NET_BACKEND_E1000,
                           .tx_ring = &ring, .rtl_tx_ring = &ring};
    spinlock_init(&state.tx_lock);
    uint8_t frame = 1;
    assert(e1000_transmit_status(0, &frame, 1) == K_EINVAL);
    assert(e1000_transmit_status(&state, 0, 1) == K_EINVAL);
    assert(e1000_transmit_status(&state, &frame, 0) == K_EINVAL);
    assert(e1000_transmit_status(&state, &frame, 2049) == K_EINVAL);
    for (unsigned backend = 1; backend <= 2; ++backend) {
        state.backend = backend;
        ring.status = 0; ring.options1 = 1U << 31;
        unsigned before = atomic_load(&submissions);
        assert(e1000_device_transmit(&state, &frame, 1) == K_EAGAIN);
        assert(!e1000_transmit(&state, &frame, 1));
        assert(before == atomic_load(&submissions) && !depth);
        ring.status = 1; ring.options1 = 0;
        backend_ok = false;
        assert(e1000_device_transmit(&state, &frame, 1) == K_EIO);
        backend_ok = true;
        state.initialized = false;
        assert(e1000_device_transmit(&state, &frame, 1) == K_EIO);
        state.initialized = true;
        pthread_t threads[4];
        for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], 0, worker, &state));
        for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], 0));
    }
    assert(!depth && !atomic_load(&active));
    puts("TX submission: concurrent socket callers, backpressure and errors passed");
}
'''
with tempfile.TemporaryDirectory(prefix='net-tx-submission-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-I', str(root / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
