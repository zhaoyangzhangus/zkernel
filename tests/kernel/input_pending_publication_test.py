"""Actual input count publication with concurrent producers and a consumer."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/input/core.c').read_text()
assert 'atomic_fetch_add_explicit(&g_input.pending' not in source
assert 'atomic_fetch_sub_explicit(&g_input.pending' not in source
source = source.replace('atomic_store_explicit(&g_input.pending,', 'checked_publish(&g_input.pending,')
code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
static _Thread_local unsigned depth, baseline;
static atomic_uint publications,done;
static void checked_publish(atomic_uint *,unsigned,memory_order);
void sched_preempt_disable(void) { ++depth; }
void sched_preempt_enable(void) { assert(depth); --depth; }
''' + source + r'''
static void checked_publish(atomic_uint *target,unsigned value,memory_order order) {
    assert(target==&g_input.pending && atomic_load(&g_input.lock.state));
    assert(depth==baseline+1 && value==g_input.count && value<=INPUT_CORE_CAPACITY);
    assert(order==memory_order_release);
    atomic_store_explicit(target,value,order); atomic_fetch_add(&publications,1);
}
void wait_queue_init(wait_queue_t *queue) { memset(queue,0,sizeof(*queue)); }
uint32_t wake_one(wait_queue_t *queue) { assert(queue && depth==baseline); return 0; }
kstatus_t wait_on_queue(wait_queue_t *queue,bool (*ready)(void *),void *context,uint64_t timeout) {
    assert(queue && depth==baseline && !ready(context)); (void)timeout; return K_ETIMEDOUT;
}
static input_event_t make_event(unsigned id) {
    return (input_event_t){.timestamp=id+1,.device_id=id/10000,.type=INPUT_EVENT_KEY,
                           .code=id,.value=(int32_t)id,.value2=-123,.value3=456};
}
static void *producer(void *opaque) {
    unsigned id=(unsigned)(uintptr_t)opaque;
    for(unsigned i=0;i<10000;++i) {
        input_event_t event=make_event(id*10000+i);
        assert(input_core_push(&event)==K_OK && !depth);
    }
    atomic_fetch_add(&done,1); return 0;
}
int main(void) {
    assert(input_core_init() && input_core_self_test());
    input_event_t out;
    for(baseline=0;baseline<32;++baseline) {
        depth=baseline; input_event_t event=make_event(1);
        assert(input_core_push(&event)==K_OK && depth==baseline && input_core_pending()==1);
        assert(input_core_pop(&out)==K_OK && depth==baseline && !input_core_pending());
        assert(!memcmp(&out,&event,sizeof(out)));
        assert(input_core_pop(&out)==K_EAGAIN && depth==baseline);
        assert(input_core_push(0)==K_EINVAL && input_core_pop(0)==K_EINVAL);
        assert(input_core_read(&out,1)==K_ETIMEDOUT && depth==baseline);
    }
    baseline=depth=0; atomic_store(&publications,0);
    /* 测试压力合并和拒绝运动事件时不发布计数。 */
    for(unsigned i=0;i<INPUT_CORE_CAPACITY;++i) { input_event_t event=make_event(i); assert(input_core_push(&event)==K_OK); }
    unsigned before=atomic_load(&publications); input_event_t motion=make_event(99); motion.type=INPUT_EVENT_RELATIVE;
    assert(input_core_push(&motion)==K_EAGAIN && input_core_pending()==INPUT_CORE_CAPACITY);
    assert(atomic_load(&publications)==before);
    input_event_t key=make_event(999); assert(input_core_push(&key)==K_OK && input_core_pending()==INPUT_CORE_CAPACITY);
    while(input_core_pop(&out)==K_OK) {}
    assert(!input_core_pending());
    input_core_init(); atomic_store(&publications,0);
    motion.value=3;
    for(unsigned i=0;i<INPUT_COALESCE_THRESHOLD;++i) assert(input_core_push(&motion)==K_OK);
    before=atomic_load(&publications);
    assert(input_core_push(&motion)==K_OK && input_core_pending()==INPUT_COALESCE_THRESHOLD);
    assert(atomic_load(&publications)==before);
    while(input_core_pop(&out)==K_OK) {}
    input_core_init(); atomic_store(&publications,0);
    pthread_t workers[4]; unsigned received=0,last[4]={0}; bool seen[4]={0};
    for(unsigned i=0;i<4;++i) assert(!pthread_create(&workers[i],0,producer,(void *)(uintptr_t)i));
    while(atomic_load(&done)<4 || input_core_pending()) {
        kstatus_t status=input_core_pop(&out); assert(!depth);
        if(status==K_EAGAIN) { sched_yield(); continue; }
        assert(status==K_OK && out.device_id<4 && out.code<40000);
        unsigned id=out.device_id, sequence=out.code%10000;
        assert(!seen[id] || sequence>last[id]); seen[id]=true; last[id]=sequence;
        input_event_t expected=make_event(out.code); assert(!memcmp(&out,&expected,sizeof(out)));
        ++received;
        assert(input_core_pending()<=INPUT_CORE_CAPACITY);
    }
    for(unsigned i=0;i<4;++i) assert(!pthread_join(workers[i],0));
    assert(received>0 && received+input_core_dropped()==40000);
    assert(!depth && !g_input.count && !input_core_pending() && !atomic_load(&g_input.lock.state));
    printf("Input publication: 32 nested depths, empty/invalid/wait/merge/overflow exits and 40000 concurrent events with FIFO and exact received+dropped accounting passed\n");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-input-publication-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
