"""Actual IRQ registry: masking, deferred delivery, callback reentry and SMP."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/arch/x86_64/irq/irq.c').read_text()
source = source[source.index('typedef struct irq_slot'):source.index('static void irq_self_test_handler')]
# 只替换特权标志指令；注册表操作、首次发布、错误路径和生产自旋锁仍会实际执行。
save = '__asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");'
restore = '__asm__ volatile ("sti" : : : "memory");'
assert source.count(save) == source.count(restore) == 1
source = source.replace(save, 'flags = test_irq_save();').replace(restore, 'test_irq_enable();')
source = source.replace('static void irq_initialize(void) {',
                        'static void irq_initialize(void) { assert(!irq_enabled);')
initialize = source[source.index('static void irq_initialize('):source.index('static uint64_t irq_lock(')]
fast_check = '    if (atomic_load_explicit(&g_irq_initialized, memory_order_acquire) == 2U) return;\n'
assert initialize.count(fast_check) == 1
previous = initialize.replace('irq_initialize(', 'previous_initialize(').replace(fast_check, '')
program = '\n'.join('#include "' + str(root / path) + '"' for path in (
    'include/kernel/irq.h', 'include/kernel/spinlock.h', 'include/arch/x86_64/context.h')) + r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
static _Thread_local bool irq_enabled, inject, pending;
static _Thread_local unsigned saves, restores, locks, unlocks, eois, calls;
static _Thread_local uint8_t delivered_vector;
static _Thread_local unsigned lock_depth;
static atomic_uint total_calls;
static atomic_uint ready;
static atomic_bool go;
static atomic_uint init_claims;
static bool test_claim(atomic_uint *state,unsigned *expected,unsigned desired,
                       memory_order success,memory_order failure) {
    atomic_fetch_add_explicit(&init_claims,1,memory_order_relaxed);
    bool won=atomic_compare_exchange_strong_explicit(state,expected,desired,success,failure);
    /* 等待四个启动 CPU 全部进入后，再允许第一个发布者继续；测试状态 1，而不只是
     * 测试发布后才到达的四个读取者。 */
    if(won)while(atomic_load_explicit(&init_claims,memory_order_acquire)<4) {
        __asm__ volatile ("pause");
    }
    return won;
}
static uint64_t test_irq_save(void) {
    uint64_t flags=irq_enabled?1ULL<<9:0;
    irq_enabled=false;++saves;return flags;
}
static void test_irq_enable(void) {
    assert(!irq_enabled&&!lock_depth);++restores;irq_enabled=true;
    if(pending) {
        pending=false;irq_enabled=false;
        struct arch_trap_frame frame={0};frame.vector=delivered_vector;
        x86_irq_dispatch(&frame);
        assert(!irq_enabled);irq_enabled=true;
    }
}
static void test_lock(spinlock_t *lock) {
    assert(!irq_enabled&&!lock_depth);
    spinlock_lock(lock);++lock_depth;++locks;
    if(inject) {inject=false;pending=true;}
}
static void test_unlock(spinlock_t *lock) {
    assert(!irq_enabled&&lock_depth==1);
    --lock_depth;++unlocks;spinlock_unlock(lock);
}
static void lapic_end_of_interrupt(void) {assert(!lock_depth);++eois;}
#define spinlock_lock test_lock
#define spinlock_unlock test_unlock
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit test_claim
''' + source + previous + r'''
static void handler(uint8_t vector,struct arch_trap_frame *frame,void *context) {
    assert(!lock_depth&&frame&&frame->vector==vector&&context==&calls);
    ++calls;atomic_fetch_add_explicit(&total_calls,1,memory_order_relaxed);
}
static void remove_self(uint8_t vector,struct arch_trap_frame *frame,void *context) {
    handler(vector,frame,context);
    bool before=irq_enabled;
    assert(irq_unregister(vector,remove_self,context)==K_OK);
    assert(irq_enabled==before&&!lock_depth);
}
static void reset(bool enabled) {
    irq_enabled=enabled;inject=pending=false;
    saves=restores=locks=unlocks=eois=calls=lock_depth=0;
}
static void balanced(bool enabled) {
    assert(irq_enabled==enabled&&!pending&&!lock_depth);
    assert(saves==locks&&locks==unlocks);
    assert(restores==(enabled?saves:0));
}
static void *worker(void *argument) {
    uint8_t vector=(uint8_t)(uintptr_t)argument;
    reset(true);
    atomic_fetch_add_explicit(&ready,1,memory_order_release);
    while(!atomic_load_explicit(&go,memory_order_acquire)) { __asm__ volatile ("pause"); }
    for(unsigned n=0;n<2000;++n) {
        assert(irq_register(vector,handler,&calls)==K_OK);
        struct arch_trap_frame frame={0};frame.vector=vector;
        x86_irq_dispatch(&frame);
        assert(irq_unregister(vector,handler,&calls)==K_OK);
    }
    assert(calls==2000&&eois==2000);balanced(true);return 0;
}
static void run_workers(void) {
    pthread_t threads[4];atomic_store(&ready,0);atomic_store(&go,false);
    for(unsigned n=0;n<4;++n)assert(!pthread_create(&threads[n],0,worker,(void *)(uintptr_t)(64+n)));
    while(atomic_load_explicit(&ready,memory_order_acquire)!=4) { __asm__ volatile ("pause"); }
    atomic_store_explicit(&go,true,memory_order_release);
    for(unsigned n=0;n<4;++n)assert(!pthread_join(threads[n],0));
}
int main(void) {
    assert(atomic_load(&g_irq_initialized)==0);
    memset(g_irq_slots,0xa5,sizeof(g_irq_slots));run_workers();
    assert(atomic_load(&g_irq_initialized)==2);
    unsigned initial_claims=atomic_load(&init_claims);
    assert(initial_claims==4);
    /* 在表中存在有效项时比较旧版和当前的已初始化路径：两者都不得重置表或更改其锁。 */
    reset(false);assert(irq_register(50,handler,&calls)==K_OK);
    irq_slot_t slots[IRQ_VECTOR_LAST+1];memcpy(slots,g_irq_slots,sizeof(slots));
    unsigned before=atomic_load(&init_claims);
    for(unsigned n=0;n<10000;++n)previous_initialize();
    assert(atomic_load(&init_claims)-before==10000);
    before=atomic_load(&init_claims);
    for(unsigned n=0;n<10000;++n)irq_initialize();
    assert(atomic_load(&init_claims)==before);
    assert(!memcmp(slots,g_irq_slots,sizeof(slots))&&!atomic_load(&g_irq_lock.state));
    assert(irq_unregister(50,handler,&calls)==K_OK);
    unsigned cases=0;
    for(unsigned state=0;state<2;++state)
    for(unsigned vector=0;vector<256;++vector) {
        reset(state);bool valid=vector>=IRQ_VECTOR_FIRST&&vector<=IRQ_VECTOR_LAST;
        assert(irq_register(vector,0,&calls)==K_EINVAL);
        assert(irq_register(vector,handler,&calls)==(valid?K_OK:K_EINVAL));
        if(valid) {
            assert(irq_register(vector,handler,&calls)==K_EBUSY);
            assert(irq_unregister(vector,remove_self,&calls)==K_ENOENT);
            assert(irq_unregister(vector,handler,0)==K_ENOENT);
        }
        struct arch_trap_frame frame={0};frame.vector=vector;
        x86_irq_dispatch(&frame);assert(calls==(unsigned)valid&&eois==1);
        assert(irq_unregister(vector,handler,&calls)==(valid?K_OK:K_EINVAL));
        if(valid)assert(irq_unregister(vector,handler,&calls)==K_ENOENT);
        x86_irq_dispatch(&frame);assert(calls==(unsigned)valid&&eois==2);
        x86_irq_dispatch(0);frame.vector=UINT64_MAX;x86_irq_dispatch(&frame);
        assert(eois==4);balanced(state);++cases;
    }
    /* 注册线程持有表锁期间到达的 IRQ 必须等到发布/解锁之后再处理；其回调可以自行移除。 */
    for(unsigned vector=IRQ_VECTOR_FIRST;vector<=IRQ_VECTOR_LAST;++vector) {
        reset(true);inject=true;delivered_vector=vector;
        assert(irq_register(vector,remove_self,&calls)==K_OK);
        assert(calls==1&&eois==1&&!pending&&irq_enabled);
        assert(irq_unregister(vector,remove_self,&calls)==K_ENOENT);
        assert(locks==unlocks&&saves==locks&&!lock_depth);++cases;
    }
    run_workers();
    for(unsigned n=0;n<=IRQ_VECTOR_LAST;++n)assert(!g_irq_slots[n].handler&&!g_irq_slots[n].context);
    assert(atomic_load(&g_irq_initialized)==2);
    assert(atomic_load(&init_claims)==before);
    printf("IRQ registry: %u flag/error/dispatch/deferred-reentry cases; 16000 concurrent SMP lifecycles; callbacks outside lock, first publication and IRQ state preserved\n",cases);
    printf("Initialized IRQ registry: 10000 old init CAS attempts -> 0; every subsequent registration/dispatch/unregistration also bypasses init CAS\n");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-irq-registry-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
