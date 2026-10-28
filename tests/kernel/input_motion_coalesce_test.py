"""真实输入合并：方向屏障、无损加法、队列顺序和边界覆盖。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/input/core.c').read_text()
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned depth,baseline;
void sched_preempt_disable(void) { ++depth; }
void sched_preempt_enable(void) { assert(depth); --depth; }
''' + source + r'''
void wait_queue_init(wait_queue_t *queue) { memset(queue,0,sizeof(*queue)); }
uint32_t wake_one(wait_queue_t *queue) { assert(queue && depth==baseline); return 0; }
kstatus_t wait_on_queue(wait_queue_t *q,bool (*ready)(void *),void *context,uint64_t timeout) {
    (void)q; (void)ready; (void)context; (void)timeout; return K_ETIMEDOUT;
}
static int32_t add(int32_t a,int32_t b) {
    __int128 sum=(__int128)a+b;
    return sum>INT32_MAX ? INT32_MAX : sum<INT32_MIN ? INT32_MIN : (int32_t)sum;
}
static bool lossless(int32_t a,int32_t b) {
    if(a && b && ((a<0)!=(b<0)))return false;
    __int128 total=(__int128)a+b;
    return total>=INT32_MIN && total<=INT32_MAX;
}
static bool oracle(input_event_t *events,unsigned write,unsigned count,const input_event_t *in) {
    if(!in || !count) return false;
    if(in->type==INPUT_EVENT_RELATIVE) {
        for(unsigned offset=0;offset<count;++offset) {
            input_event_t *q=&events[(write+INPUT_CORE_CAPACITY-1-offset)%INPUT_CORE_CAPACITY];
            if(q->type!=INPUT_EVENT_RELATIVE)break;
            if(q->device_id==in->device_id && q->flags==in->flags && q->code==in->code) {
                if(!lossless(q->value,in->value))return false;
                q->value=add(q->value,in->value); q->timestamp=in->timestamp; return true;
            }
        }
    } else if(in->type==INPUT_EVENT_POINTER && !in->code) {
        for(unsigned offset=0;offset<count;++offset) {
            input_event_t *q=&events[(write+INPUT_CORE_CAPACITY-1-offset)%INPUT_CORE_CAPACITY];
            if(q->type!=INPUT_EVENT_POINTER || q->code)break;
            if(q->device_id!=in->device_id || q->flags!=in->flags)continue;
            if(!lossless(q->value2,in->value2) || !lossless(q->value3,in->value3) ||
               !lossless(q->value4,in->value4))return false;
            q->value2=add(q->value2,in->value2); q->value3=add(q->value3,in->value3);
            q->value4=add(q->value4,in->value4); q->timestamp=in->timestamp; return true;
        }
    }
    return false;
}
static const int32_t deltas[]={INT32_MIN,INT32_MAX,-1,0,1,1000000};
static input_event_t event(unsigned id,unsigned scenario) {
    input_event_t e={.timestamp=id+1,.type=INPUT_EVENT_RELATIVE,.code=id%2,
                     .value=deltas[id%6],.value2=deltas[(id+1)%6],
                     .value3=deltas[(id+2)%6],.value4=deltas[(id+3)%6]};
    if(scenario==1 || scenario==3 || scenario==5) { e.type=INPUT_EVENT_POINTER; e.code=0; e.value=id&7; }
    if(scenario==2 || scenario==3) { e.device_id=id%5; e.flags=id%3; }
    if(scenario==4 && id%7==0) e.type=INPUT_EVENT_KEY;
    if(scenario==5 && id%7==0) e.code=1;
    if(scenario==6 && id%2) { e.type=INPUT_EVENT_POINTER; e.code=0; }
    if(scenario==7) e.type=INPUT_EVENT_KEY;
    return e;
}
int main(void) {
    assert(input_core_init()); input_event_t original[INPUT_CORE_CAPACITY],expected[INPUT_CORE_CAPACITY];
    /* 按钮是强制屏障，即使两侧移动方向完全一致也不能跨越。 */
    g_input.count=2;g_input.write=2;
    g_input.events[0]=(input_event_t){.type=INPUT_EVENT_POINTER,.value2=1};
    g_input.events[1]=(input_event_t){.type=INPUT_EVENT_POINTER,.code=1};
    input_event_t boundary={.type=INPUT_EVENT_POINTER,.value2=1};
    bool wanted=false,result=input_coalesce_motion(&boundary);
    assert(result==wanted);
    const unsigned counts[]={0,1,INPUT_COALESCE_THRESHOLD-1,INPUT_COALESCE_THRESHOLD,
                             INPUT_COALESCE_THRESHOLD+1,INPUT_CORE_CAPACITY-1,INPUT_CORE_CAPACITY};
    unsigned cases=0;
    for(unsigned read=0;read<INPUT_CORE_CAPACITY;++read) for(unsigned scenario=0;scenario<8;++scenario)
    for(unsigned ci=0;ci<7;++ci) {
        unsigned count=counts[ci],write=(read+count)%INPUT_CORE_CAPACITY;
        memset(original,0x5a,sizeof(original));
        for(unsigned i=0;i<count;++i) original[(read+i)%INPUT_CORE_CAPACITY]=event(i,scenario);
        for(unsigned mode=0;mode<24;++mode) {
            input_event_t in=event(mode,mode<8?0:1);
            if(mode==16)in.code=1;
            if(mode==17)in.type=INPUT_EVENT_KEY;
            if(mode>=18 && count)in=original[(read+(mode%3==0?0:mode%3==1?count/2:count-1))%INPUT_CORE_CAPACITY];
            in.timestamp=1000000+mode;
            in.value=deltas[mode%6]; in.value2=deltas[(mode+1)%6];
            in.value3=deltas[(mode+2)%6]; in.value4=deltas[(mode+3)%6];
            memcpy(g_input.events,original,sizeof(original)); memcpy(expected,original,sizeof(expected));
            g_input.read=read; g_input.write=write; g_input.count=count;
            atomic_store(&g_input.pending,count); atomic_store(&g_input.dropped,0);
            baseline=depth=(read+mode)%32;
            bool wanted=oracle(expected,write,count,&in);
            input_lock(); bool result=input_coalesce_motion(&in); input_unlock();
            assert(result==wanted && !memcmp(g_input.events,expected,sizeof(expected)));
            assert(depth==baseline && g_input.read==read && g_input.write==write && g_input.count==count);
            assert(input_core_pending()==count && !input_core_dropped() && !atomic_load(&g_input.lock.state));
            ++cases;
        }
    }
    baseline=depth=0; input_lock(); assert(!input_coalesce_motion(0)); input_unlock();
    /* Exercise the real public pressure path, including merged motion at a
     * full ring: it must not invoke eviction or change the count. */
    input_core_init(); input_event_t in,out;
    for(unsigned i=0;i<INPUT_CORE_CAPACITY;++i) {
        in=event(i,1); in.device_id=i; assert(input_core_push(&in)==K_OK);
    }
    assert(input_core_pending()==INPUT_CORE_CAPACITY);
    in=event(INPUT_CORE_CAPACITY-1,1); in.device_id=INPUT_CORE_CAPACITY-1; in.timestamp=999999;
    in.value2=1;in.value3=in.value4=0;
    assert(input_core_push(&in)==K_OK && input_core_pending()==INPUT_CORE_CAPACITY);
    assert(!input_core_dropped());
    for(unsigned i=0;i<INPUT_CORE_CAPACITY;++i) {
        input_event_t wanted=event(i,1); wanted.device_id=i;
        if(i==INPUT_CORE_CAPACITY-1) {
            wanted.value2=add(wanted.value2,in.value2); wanted.value3=add(wanted.value3,in.value3);
            wanted.value4=add(wanted.value4,in.value4); wanted.timestamp=in.timestamp;
        }
        assert(input_core_pop(&out)==K_OK && !memcmp(&out,&wanted,sizeof(out)));
    }
    assert(!input_core_pending() && !depth);
    printf("Input merge: %u independent-oracle cases, all ring offsets, mixed devices/flags/axes, direction/button/type boundaries, lossless deltas and unchanged count/order passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-input-merge-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    negative = code.replace('if (queued->type != incoming->type ||\n            (!relative && queued->code != 0U)) break;',
                            'if (queued->type != incoming->type) break;')
    assert negative != code
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=negative,text=True,check=True)
    failure = subprocess.run([binary],capture_output=True,text=True,timeout=30)
    assert failure.returncode < 0 and 'result==wanted' in failure.stderr
    print('Regression control: removing the pointer button boundary fails the original oracle')
