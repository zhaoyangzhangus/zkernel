"""Actual TX/RX entry points: one lifecycle boundary, reentry and contention."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/net/core.c').read_text()
header = (root/'kernel/drivers/net/core_internal.h').read_text()
assert 'poll_lock' not in source and 'poll_lock' not in header
def region(start, end):
    return source[source.index(start):source.index(end,source.index(start))]
wrappers = region('static bool e1000_lifecycle_try_lock(void)', 'void e1000_configure_link_local(')
send = region('kstatus_t e1000_send_frame(', 'kstatus_t e1000_get_mac_address(')
poll = region('bool e1000_poll(void)', 'bool e1000_interrupt_ready(')
code = r'''
#include <kernel/base.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <sched.h>
static _Thread_local unsigned depth, baseline, attempts;
static _Thread_local bool reenter;
static atomic_uint g_e1000_lifecycle_lock;
static bool g_e1000_runtime_ready;
static atomic_uint active, sends, receives, samples;
static bool online, rx_ready;
static kstatus_t send_result;
static void sched_preempt_disable(void) { ++depth; }
static void sched_preempt_enable(void) { assert(depth); --depth; }
static unsigned checked_exchange(atomic_uint *state, unsigned value, memory_order order) {
    assert((depth==baseline+1 || depth==baseline+2) && state==&g_e1000_lifecycle_lock && value==1);
    ++attempts; return atomic_exchange_explicit(state,value,order);
}
typedef struct { bool link_up; unsigned device_id; } mock_device_t;
typedef struct { mock_device_t device; } mock_pci_t;
typedef struct { bool initialized, link_up; unsigned backend;
                 mock_device_t device; mock_pci_t *pci; } e1000_state_t;
static e1000_state_t g_e1000;
#define E1000_BUFFER_SIZE 2048U
#define E1000_REG_STATUS 0x0008U
#define E1000_STATUS_LU (1U << 1)
#define NET_BACKEND_RTL8126 2U
#define TELEMETRY_CATEGORY_NETWORK_BATCH 1U
static kstatus_t net_device_send(mock_device_t *,const void *,size_t);
static uint64_t telemetry_timestamp(void);
static bool telemetry_record_latency(unsigned,unsigned,uint64_t);
static uint32_t e1000_read(e1000_state_t *,uint32_t);
static bool rtl8126_link_up_state(e1000_state_t *);
static bool e1000_poll_receive(e1000_state_t *);
static bool rtl8126_poll_receive_state(e1000_state_t *);
#undef atomic_exchange_explicit
#define atomic_exchange_explicit checked_exchange
''' + wrappers + send + poll + r'''
#undef atomic_exchange_explicit
static void enter_backend(void) {
    assert(depth==baseline+1 && atomic_load(&g_e1000_lifecycle_lock)==1);
    assert(!atomic_fetch_add(&active,1));
    if(reenter) {
        reenter=false; unsigned char frame=1;
        assert(e1000_send_frame(&frame,1)==K_EBUSY && !e1000_poll());
        assert(depth==baseline+1 && atomic_load(&g_e1000_lifecycle_lock)==1);
    }
    assert(atomic_fetch_sub(&active,1)==1);
}
static kstatus_t net_device_send(mock_device_t *device,const void *frame,size_t length) {
    assert(device==&g_e1000.device && frame && length && length<=E1000_BUFFER_SIZE);
    enter_backend(); atomic_fetch_add(&sends,1); return send_result;
}
static uint64_t telemetry_timestamp(void) { enter_backend(); return 123; }
static bool telemetry_record_latency(unsigned category,unsigned id,uint64_t start) {
    assert(category==TELEMETRY_CATEGORY_NETWORK_BATCH && !id && start==123);
    assert(depth==baseline); atomic_fetch_add(&samples,1); return true;
}
static uint32_t e1000_read(e1000_state_t *state,uint32_t offset) {
    assert(state==&g_e1000 && offset==E1000_REG_STATUS); enter_backend();
    return online ? E1000_STATUS_LU : 0;
}
static bool rtl8126_link_up_state(e1000_state_t *state) {
    assert(state==&g_e1000); enter_backend(); return online;
}
static bool e1000_poll_receive(e1000_state_t *state) {
    assert(state==&g_e1000); enter_backend(); atomic_fetch_add(&receives,1); return rx_ready;
}
static bool rtl8126_poll_receive_state(e1000_state_t *state) { return e1000_poll_receive(state); }
static void balanced(void) { assert(depth==baseline && !atomic_load(&g_e1000_lifecycle_lock)); }
static void *worker(void *opaque) {
    bool tx=(bool)(uintptr_t)opaque; unsigned char frame=1; unsigned successes=0;
    for(unsigned i=0;i<20000;++i) {
        if(tx) {
            kstatus_t status=e1000_send_frame(&frame,1);
            assert(status==K_OK || status==K_EBUSY);
            successes += status==K_OK;
        } else successes += e1000_poll();
        assert(!depth);
    }
    /* try-lock makes no fairness promise; ensure every worker exercises the
     * protected backend even if all its initial bounded attempts lost. */
    while(!successes) {
        sched_yield();
        if(tx) successes += e1000_send_frame(&frame,1)==K_OK;
        else successes += e1000_poll();
        assert(!depth);
    }
    return 0;
}
int main(void) {
    unsigned char frame[E1000_BUFFER_SIZE+1]={0};
    for(baseline=0;baseline<32;++baseline) {
        depth=baseline; attempts=0;
        assert(e1000_send_frame(0,1)==K_EINVAL);
        assert(e1000_send_frame(frame,0)==K_EINVAL);
        assert(e1000_send_frame(frame,sizeof(frame))==K_EINVAL && !attempts); balanced();
        g_e1000.initialized=false; g_e1000_runtime_ready=false;
        assert(e1000_send_frame(frame,1)==K_ENOENT && !e1000_poll()); balanced();
        g_e1000.initialized=true;
        assert(e1000_send_frame(frame,1)==K_ENOENT && !e1000_poll()); balanced();
        g_e1000_runtime_ready=true;
        atomic_store(&g_e1000_lifecycle_lock,1);
        assert(e1000_send_frame(frame,1)==K_EBUSY && !e1000_poll());
        assert(depth==baseline && atomic_load(&g_e1000_lifecycle_lock)==1);
        atomic_store(&g_e1000_lifecycle_lock,0);
        for(unsigned backend=1;backend<=2;++backend) {
            g_e1000.backend=backend; online=true; rx_ready=true; send_result=K_OK;
            attempts=0; assert(e1000_send_frame(frame,E1000_BUFFER_SIZE)==K_OK && attempts==1); balanced();
            attempts=0; assert(e1000_poll() && attempts==1); balanced();
            reenter=true; assert(e1000_send_frame(frame,1)==K_OK && !reenter); balanced();
            reenter=true; assert(e1000_poll() && !reenter); balanced();
            send_result=K_EIO; assert(e1000_send_frame(frame,1)==K_EIO); balanced();
            online=false; assert(!e1000_poll() && !g_e1000.link_up && !g_e1000.device.link_up); balanced();
            online=true; rx_ready=false; assert(!e1000_poll()); balanced();
        }
    }
    baseline=depth=0; online=rx_ready=true; send_result=K_OK;
    atomic_store(&sends,0); atomic_store(&receives,0); atomic_store(&samples,0);
    pthread_t workers[4];
    for(unsigned i=0;i<4;++i) assert(!pthread_create(&workers[i],0,worker,(void *)(uintptr_t)(i%2)));
    for(unsigned i=0;i<4;++i) assert(!pthread_join(workers[i],0));
    balanced(); assert(!atomic_load(&active));
    printf("Concurrent counts: TX=%u RX=%u samples=%u\n",atomic_load(&sends),atomic_load(&receives),atomic_load(&samples));
    fflush(stdout);
    assert(atomic_load(&sends)>0 && atomic_load(&receives)>0 && atomic_load(&samples)==atomic_load(&receives));
    puts("TX/RX: one acquisition per entry, 32 nested depths, offline/busy/link/send failures, both backend branches, callback reentry and 80000+ concurrent attempts passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-net-exclusion-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
