"""Production input queue: full-ring motion eviction and shortest-side stable erase."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/input/core.c').read_text()
source = source.replace('g_input.events[target] = g_input.events[next];',
                        '++moves; g_input.events[target] = g_input.events[next];')
source = source.replace('g_input.events[target] = g_input.events[previous];',
                        '++moves; g_input.events[target] = g_input.events[previous];')
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned depth, baseline, moves;
void sched_preempt_disable(void) { ++depth; }
void sched_preempt_enable(void) { assert(depth); --depth; }
''' + source + r'''
void wait_queue_init(wait_queue_t *q) { memset(q,0,sizeof(*q)); }
uint32_t wake_one(wait_queue_t *q) { (void)q; assert(depth==baseline); return 0; }
kstatus_t wait_on_queue(wait_queue_t *q,bool (*ready)(void *),void *context,uint64_t timeout) {
    (void)q; (void)ready; (void)context; (void)timeout; assert(depth==baseline); return K_ETIMEDOUT;
}
static input_event_t event(unsigned id) {
    input_event_t e={.timestamp=id+1,.device_id=id+7,.type=INPUT_EVENT_KEY,
                     .flags=(uint16_t)id,.code=id+4,.value=(int32_t)id,
                     .value2=INT32_MIN,.value3=INT32_MAX,.value4=-7};
    return e;
}
static void seed(unsigned read,unsigned count,unsigned target,unsigned kind) {
    g_input.read=read; g_input.count=count; g_input.write=(read+count)%INPUT_CORE_CAPACITY;
    atomic_store(&g_input.pending,count); atomic_store(&g_input.dropped,0);
    for(unsigned i=0;i<count;++i) {
        input_event_t e=event(i);
        if(i==target) { e.type=kind; e.code=kind==INPUT_EVENT_POINTER ? 0 : INPUT_REL_X; }
        g_input.events[(read+i)%INPUT_CORE_CAPACITY]=e;
    }
    moves=0;
}
static void erase_case(unsigned read,unsigned count,unsigned target,unsigned kind) {
    seed(read,count,target,kind);
    input_lock(); assert(depth==baseline+1);
    assert(input_drop_oldest_motion()); input_unlock();
    assert(depth==baseline && g_input.count==count-1);
    assert(input_core_pending()==count-1 && input_core_dropped()==1);
    assert(g_input.write==(g_input.read+g_input.count)%INPUT_CORE_CAPACITY);
    for(unsigned i=0;i<count;++i) {
        if(i==target)continue;
        input_event_t out, expected=event(i);
        assert(input_core_pop(&out)==K_OK && depth==baseline);
        assert(!memcmp(&out,&expected,sizeof(out)));
    }
    input_event_t sentinel=event(999), before=sentinel;
    assert(input_core_pop(&sentinel)==K_EAGAIN && !memcmp(&sentinel,&before,sizeof(before)));
    unsigned suffix=count-target-1;
    assert(moves==(target<suffix ? target : suffix));
    assert(!input_core_pending() && !atomic_load(&g_input.lock.state));
}
int main(void) {
    assert(input_core_init() && input_core_self_test());
    /* 这是先前的缺陷：read==write 会让旧擦除循环跳过整个满队列，留下最早的运动事件，
     * 丢弃队尾事件。 */
    erase_case(0,INPUT_CORE_CAPACITY,0,INPUT_EVENT_RELATIVE);
    unsigned cases=1;
    for(unsigned read=0;read<INPUT_CORE_CAPACITY;++read) {
        for(unsigned target=0;target<INPUT_CORE_CAPACITY;++target) {
            baseline=depth=(read+target)%32;
            erase_case(read,INPUT_CORE_CAPACITY,target,(target&1)?INPUT_EVENT_POINTER:INPUT_EVENT_RELATIVE);
            ++cases;
        }
    }
    for(unsigned count=1;count<=INPUT_CORE_CAPACITY;++count) for(unsigned target=0;target<count;++target) {
        baseline=depth=target%32;
        erase_case(INPUT_CORE_CAPACITY-1,count,target,INPUT_EVENT_RELATIVE); ++cases;
    }
    baseline=depth=0;
    /* 指针按键状态变化不是可丢弃的运动事件。两个运动记录中只删除第一个，并保留其他
     * 所有字节及顺序。 */
    seed(13,INPUT_CORE_CAPACITY,40,INPUT_EVENT_RELATIVE);
    g_input.events[(13+20)%INPUT_CORE_CAPACITY].type=INPUT_EVENT_POINTER;
    g_input.events[(13+20)%INPUT_CORE_CAPACITY].code=1;
    g_input.events[(13+80)%INPUT_CORE_CAPACITY].type=INPUT_EVENT_POINTER;
    g_input.events[(13+80)%INPUT_CORE_CAPACITY].code=0;
    input_event_t expected_ring[INPUT_CORE_CAPACITY];
    for(unsigned i=0;i<INPUT_CORE_CAPACITY;++i) expected_ring[i]=g_input.events[(13+i)%INPUT_CORE_CAPACITY];
    input_lock(); assert(input_drop_oldest_motion()); input_unlock();
    assert(moves==40 && input_core_dropped()==1);
    for(unsigned i=0;i<INPUT_CORE_CAPACITY;++i) {
        if(i==40)continue;
        input_event_t out; assert(input_core_pop(&out)==K_OK);
        assert(!memcmp(&out,&expected_ring[i],sizeof(out)));
    }
    input_lock(); assert(!input_drop_oldest_motion()); input_unlock();
    assert(!input_core_pending() && input_core_dropped()==1);
    seed(9,INPUT_CORE_CAPACITY,UINT32_MAX,INPUT_EVENT_RELATIVE);
    input_lock(); assert(!input_drop_oldest_motion()); input_unlock();
    assert(!moves && input_core_pending()==INPUT_CORE_CAPACITY && !input_core_dropped());
    input_event_t incoming=event(999); incoming.type=INPUT_EVENT_RELATIVE;
    assert(input_core_push(&incoming)==K_EAGAIN && input_core_dropped()==1);
    /* 向满队列写入按键时应丢弃运动事件，不能丢弃其他按键。 */
    seed(127,INPUT_CORE_CAPACITY,0,INPUT_EVENT_RELATIVE); incoming=event(999);
    assert(input_core_push(&incoming)==K_OK && input_core_pending()==INPUT_CORE_CAPACITY);
    for(unsigned i=1;i<INPUT_CORE_CAPACITY;++i) {
        input_event_t out, expected=event(i); assert(input_core_pop(&out)==K_OK);
        assert(!memcmp(&out,&expected,sizeof(out)));
    }
    input_event_t out; assert(input_core_pop(&out)==K_OK && !memcmp(&out,&incoming,sizeof(out)));
    assert(!depth && !input_core_pending() && !atomic_load(&g_input.lock.state));
    printf("Input eviction: %u full/wrapped/partial-ring stable-order cases, 32 nested depths, exact shortest-side moves, no-motion pressure and full-ring key insertion passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-input-eviction-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
