"""Exercise the production RX completion recheck for both DMA ring formats."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/core.c').read_text()
start = source.index('void e1000_deferred_poll(void *argument) {')
end = source.index('uint32_t e1000_last_error', start)
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#define NET_BACKEND_E1000 1
#define NET_BACKEND_RTL8126 2
#define E1000_RX_STATUS_DD 1U
typedef struct { unsigned status; } intel_desc;
typedef struct { uint32_t options1; } realtek_desc;
static struct {
    bool initialized;
    unsigned backend, rx_clean, rtl_rx_next;
    intel_desc *rx_ring;
    realtek_desc *rtl_rx_ring;
    unsigned rx_ring_dma, rtl_rx_ring_dma;
} g_e1000;
static bool g_e1000_runtime_ready=true, lock_available=true, inject;
static atomic_bool g_e1000_poll_queued, g_e1000_poll_retry;
static unsigned kicks, syncs, polls, unlocks;
static unsigned *last_dma;
static void e1000_record_poll_wait(void) {}
static bool e1000_lifecycle_try_lock(void) { return lock_available; }
static void e1000_lifecycle_unlock(void) { ++unlocks; }
static void dma_sync_for_cpu(unsigned *dma) { ++syncs;last_dma=dma; }
static bool e1000_schedule_deferred_poll(void) {
    assert(!atomic_load(&g_e1000_poll_queued));++kicks;return true;
}
static void rtl8126_rearm_interrupts(void *s) {
    assert(s==&g_e1000 && !atomic_load(&g_e1000_poll_queued));
}
static bool e1000_poll(void) {
    ++polls;
    /* Simulate a coalesced IRQ at the end of the receive worker. */
    if(inject)g_e1000.rtl_rx_ring[g_e1000.rtl_rx_next].options1=0;
    return false;
}
''' + source[start:end] + r'''
int main(void) {
    realtek_desc rtl[64];intel_desc intel[16]={0};
    for(unsigned i=0;i<64;++i)rtl[i].options1=1U<<31;
    g_e1000.initialized=true;g_e1000.backend=NET_BACKEND_RTL8126;
    g_e1000.rtl_rx_ring=rtl;
    for(unsigned i=0;i<64;++i) {
        g_e1000.rtl_rx_next=i;
        unsigned before=kicks;
        atomic_store(&g_e1000_poll_queued,true);
        e1000_deferred_poll(0);assert(kicks==before);
        inject=true;atomic_store(&g_e1000_poll_queued,true);
        e1000_deferred_poll(0);assert(kicks==before+1);
        assert(last_dma==&g_e1000.rtl_rx_ring_dma);
        inject=false;rtl[i].options1=1U<<31;
    }
    g_e1000.backend=NET_BACKEND_E1000;g_e1000.rx_ring=intel;
    for(unsigned i=0;i<16;++i) {
        g_e1000.rx_clean=i;unsigned before=kicks;
        e1000_deferred_poll(0);assert(kicks==before);
        intel[i].status=E1000_RX_STATUS_DD;
        e1000_deferred_poll(0);assert(kicks==before+1);
        assert(last_dma==&g_e1000.rx_ring_dma);intel[i].status=0;
    }
    unsigned before=syncs;g_e1000_runtime_ready=false;
    e1000_deferred_poll(0);assert(syncs==before);
    g_e1000_runtime_ready=true;g_e1000.initialized=false;
    e1000_deferred_poll(0);assert(syncs==before);
    g_e1000.initialized=true;g_e1000.rx_ring=0;
    e1000_deferred_poll(0);assert(syncs==before);
    g_e1000.backend=NET_BACKEND_RTL8126;g_e1000.rtl_rx_ring=0;
    e1000_deferred_poll(0);assert(syncs==before);
    lock_available=false;unsigned old_unlocks=unlocks;
    e1000_deferred_poll(0);
    assert(atomic_load(&g_e1000_poll_retry) && unlocks==old_unlocks);
    assert(polls==unlocks+1);
}
'''
with tempfile.TemporaryDirectory(prefix='net-rx-rearm-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True)
print('RX rearm: RTL8126 all 64 slots, Intel all 16 slots, coalesced IRQ, shutdown and contended lock PASS')
