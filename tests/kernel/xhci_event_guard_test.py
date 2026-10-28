"""Production event locks in a separate unit with controlled CPU/IF hooks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/usb/xhci/core.c').read_text()
wrappers = source[source.index('void xhci_event_lock('):
                  source.index('bool xhci_core_initialize(')]
common = r'''
#include <assert.h>
#include <kernel/base.h>
#include <kernel/spinlock.h>
#define X86_RFLAGS_INTERRUPT (1ULL << 9)
typedef struct {
    spinlock_t event_lock;
    uint64_t event_lock_saved_flags;
    uint32_t event_lock_owner;
    atomic_bool event_work_waiting;
} xhci_state_t;
void sched_preempt_disable(void);
void sched_preempt_enable(void);
uint32_t x86_current_cpu_index(void);
uint64_t xhci_event_save_flags(void);
void xhci_event_enable_interrupts(void);
void xhci_event_restore_flags(uint64_t flags);
void xhci_event_lock(xhci_state_t *state);
bool xhci_event_try_lock(xhci_state_t *state);
void xhci_event_unlock(xhci_state_t *state);
void xhci_runtime_resume_events(void);
'''
instrumented = common.replace('#include <kernel/spinlock.h>', r'''
bool test_cas(atomic_uint *,unsigned *,unsigned,memory_order,memory_order);
unsigned test_load(atomic_uint *,memory_order);
#undef atomic_compare_exchange_strong_explicit
#undef atomic_load_explicit
#define atomic_compare_exchange_strong_explicit test_cas
#define atomic_load_explicit test_load
#include <kernel/spinlock.h>
''')
client = common + r'''
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
static _Thread_local unsigned depth,cpu;
static _Thread_local bool interrupt_enabled;
static xhci_state_t state;
static atomic_uint attempts,polls,waiting;
static uint64_t value,checksum;
static unsigned resumes;
void xhci_runtime_resume_events(void) {
    assert(!atomic_load(&state.event_lock.state));++resumes;
}
void sched_preempt_disable(void) {++depth;}
void sched_preempt_enable(void) {assert(depth);--depth;}
uint32_t x86_current_cpu_index(void) {assert(depth);return cpu;}
uint64_t xhci_event_save_flags(void) {
    assert(depth);return 2ULL|(interrupt_enabled?X86_RFLAGS_INTERRUPT:0);
}
void xhci_event_enable_interrupts(void) {
    assert(depth && atomic_load(&state.event_lock.state) && state.event_lock_owner==cpu);
    interrupt_enabled=true;
}
void xhci_event_restore_flags(uint64_t flags) {
    assert(depth);
    if(!(flags&X86_RFLAGS_INTERRUPT))interrupt_enabled=false;
}
bool test_cas(atomic_uint *p,unsigned *expected,unsigned desired,memory_order a,memory_order b) {
    assert(depth);atomic_fetch_add(&attempts,1);
    return atomic_compare_exchange_strong_explicit(p,expected,desired,a,b);
}
unsigned test_load(atomic_uint *p,memory_order order) {
    assert(depth);atomic_fetch_add(&polls,1);return atomic_load_explicit(p,order);
}
static void *busy(void *arg) {
    unsigned nesting=(unsigned)(uintptr_t)arg;cpu=5;
    for(unsigned i=0;i<nesting;++i)sched_preempt_disable();
    for(unsigned enabled=0;enabled<2;++enabled) {
        interrupt_enabled=enabled;
        uint64_t flags=state.event_lock_saved_flags;uint32_t owner=state.event_lock_owner;
        assert(!xhci_event_try_lock(&state));
        assert(depth==nesting && interrupt_enabled==(bool)enabled);
        assert(state.event_lock_owner==owner && state.event_lock_saved_flags==flags);
    }
    for(unsigned i=0;i<nesting;++i)sched_preempt_enable();
    assert(!depth);return 0;
}
static void *worker(void *arg) {
    cpu=(unsigned)(uintptr_t)arg;atomic_fetch_add(&waiting,1);
    for(unsigned i=0;i<20000;++i) {
        bool original_if=(i+cpu)&1U;interrupt_enabled=original_if;
        xhci_event_lock(&state);
        assert(depth==1 && interrupt_enabled && state.event_lock_owner==cpu);
        assert((bool)(state.event_lock_saved_flags&X86_RFLAGS_INTERRUPT)==original_if);
        assert(checksum==value*17);++value;checksum=value*17;
        xhci_event_unlock(&state);
        assert(!depth && interrupt_enabled==original_if);
    }
    return 0;
}
int main(void) {
    spinlock_init(&state.event_lock);state.event_lock_owner=UINT32_MAX;
    for(unsigned nesting=0;nesting<32;++nesting)for(unsigned enabled=0;enabled<2;++enabled) {
        for(unsigned i=0;i<nesting;++i)sched_preempt_disable();
        interrupt_enabled=enabled;
        xhci_event_lock(0);xhci_event_unlock(0);
        assert(!xhci_event_try_lock(0) && depth==nesting && interrupt_enabled==(bool)enabled);
        xhci_event_lock(&state);
        assert(depth==nesting+1 && interrupt_enabled && state.event_lock_owner==cpu);
        assert((bool)(state.event_lock_saved_flags&X86_RFLAGS_INTERRUPT)==(bool)enabled);
        pthread_t t;assert(!pthread_create(&t,0,busy,(void *)(uintptr_t)nesting));
        assert(!pthread_join(t,0));
        xhci_event_unlock(&state);
        assert(depth==nesting && interrupt_enabled==(bool)enabled && state.event_lock_owner==UINT32_MAX);
        assert(xhci_event_try_lock(&state) && depth==nesting+1 && interrupt_enabled);
        xhci_event_unlock(&state);
        assert(depth==nesting && interrupt_enabled==(bool)enabled && state.event_lock_owner==UINT32_MAX);
        for(unsigned i=0;i<nesting;++i)sched_preempt_enable();
        assert(!depth);
    }
    atomic_store(&attempts,0);atomic_store(&polls,0);interrupt_enabled=false;
    xhci_event_lock(&state);pthread_t threads[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,worker,(void *)(uintptr_t)(i+1)));
    while(atomic_load(&waiting)<4 || atomic_load(&attempts)<5 || atomic_load(&polls)<20000)sched_yield();
    assert(atomic_load(&attempts)==5);
    xhci_event_unlock(&state);
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    assert(value==80000 && checksum==1360000 && !depth && !interrupt_enabled);
    assert(!atomic_load(&state.event_lock.state) && state.event_lock_owner==UINT32_MAX);
    xhci_event_lock(&state);
    atomic_store(&state.event_work_waiting,true);
    xhci_event_unlock(&state);
    assert(resumes==1 && !atomic_load(&state.event_work_waiting));
    puts("xHCI event lock: 32 nested depths x IF on/off, null and busy/successful try paths, unchanged owner/saved flags on failure, 80000 contended updates and read-only waiting passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-xhci-event-') as directory:
    obj = str(Path(directory)/'lock.o')
    binary = str(Path(directory)/'test')
    flags = ['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
             '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie']
    subprocess.run([*flags,'-x','c','-','-c','-o',obj],input=instrumented+wrappers,
                   text=True,check=True)
    subprocess.run([*flags,'-no-pie','-x','c','-','-x','none',obj,'-o',binary],
                   input=client,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
