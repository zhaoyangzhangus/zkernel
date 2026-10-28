"""Production FIFO batch extraction and real desktop pump ordering oracle."""
from pathlib import Path
import subprocess
import tempfile
import resource
import signal

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/input/core.c').read_text()
source = source.replace('atomic_store_explicit(&g_input.pending,',
                        'checked_publish(&g_input.pending,')
source = source.replace('memcpy(events', 'checked_copy(events')
pump = (root / 'kernel/graphics/input_pump.c').read_text()
pump = pump[pump.index('void window_server_pump_input_mode('):]
pump = pump[:pump.index('\n/* 单一合成器')]
old = pump.replace('void window_server_pump_input_mode(', 'void old_pump(')
old = old.replace('    input_event_t events[16];\n    size_t batch_count = 0U;\n    size_t batch_index = 0U;\n', '')
start = old.index('    while (consumed < input_budget) {')
end = old.index('        bool is_motion', start)
old = old[:start] + '''    while (consumed < input_budget && input_core_pop(&event) == K_OK) {
''' + old[end:]
program = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <kernel/input.h>
#include <uapi/input.h>
static _Thread_local unsigned depth, baseline;
static atomic_uint locks, publications, done;
static atomic_uint copies, copied_bytes;
static void checked_publish(atomic_uint *,unsigned,memory_order);
static void *checked_copy(void *,const void *,size_t);
void sched_preempt_disable(void) { ++depth; ++locks; }
void sched_preempt_enable(void) { assert(depth); --depth; }
''' + source + r'''
static void *checked_copy(void *out,const void *in,size_t bytes) {
    assert(depth==baseline+1 && atomic_load(&g_input.lock.state));
    ++copies;copied_bytes+=(unsigned)bytes;return memcpy(out,in,bytes);
}
static void checked_publish(atomic_uint *target,unsigned count,memory_order order) {
    assert(depth==baseline+1 && atomic_load(&g_input.lock.state));
    assert(target==&g_input.pending && count==g_input.count && order==memory_order_release);
    atomic_store_explicit(target,count,order);++publications;
}
void wait_queue_init(wait_queue_t *q) { memset(q,0,sizeof(*q)); }
uint32_t wake_one(wait_queue_t *q) { assert(q && depth==baseline); return 0; }
kstatus_t wait_on_queue(wait_queue_t *q,bool (*ready)(void *),void *ctx,uint64_t ns) {
    (void)q;(void)ready;(void)ctx;(void)ns;return K_ETIMEDOUT;
}
static input_event_t event_for(unsigned i) {
    input_event_t e={.timestamp=i+1,.device_id=i%3,.type=INPUT_EVENT_KEY,
                    .flags=i%2,.code=i,.value=(int)i,.value2=-7,.value3=11};
    return e;
}
static void seed(unsigned head,unsigned count) {
    input_core_init();g_input.read=head;g_input.count=count;
    g_input.write=(head+count)%INPUT_CORE_CAPACITY;
    atomic_store(&g_input.pending,count);
    for(unsigned i=0;i<count;++i) g_input.events[(head+i)%INPUT_CORE_CAPACITY]=event_for(i);
}
static void *produce(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<10000;++i) {
        input_event_t e=event_for(id*10000+i);e.device_id=id;
        assert(input_core_push(&e)==K_OK && !depth);
    }
    ++done;return 0;
}
#define WINDOW_EVENT_CAPACITY 64U
#define WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS 256U
typedef struct { bool active; unsigned device_id,flags; uint64_t timestamp;
                 int64_t delta_x,delta_y; } window_motion_batch_t;
static struct { unsigned dragging_identifier; struct {unsigned power_request;} desktop; wait_queue_t event_waitq; } g_window_server;
static input_event_t routed[512];static unsigned routed_count;
static bool window_server_kernel_ready(void) { return true; }
static _Thread_local bool window_locked;
static void window_lock(void) { assert(depth==baseline&&!window_locked);window_locked=true; }
static void window_unlock(void) {assert(window_locked);window_locked=false;}
typedef struct {unsigned token;} window_control_copy_request_t;
static window_control_copy_request_t window_control_copy_selection_locked(const os_input_event_t *e){
 assert(window_locked);return (window_control_copy_request_t){e->code};
}
static void window_control_copy_complete(window_control_copy_request_t *request){
 assert(!window_locked&&request);request->token=0;
}
static bool window_input_router_is_relative_motion(const input_event_t *e) {
    return e->type==INPUT_EVENT_RELATIVE && (e->code==INPUT_REL_X || e->code==INPUT_REL_Y);
}
static void window_flush_motion_batch_locked(window_motion_batch_t *m) {
    if(!m->active)return;
    input_event_t e={.timestamp=m->timestamp,.device_id=m->device_id,
        .type=INPUT_EVENT_POINTER,.flags=m->flags,.value2=(int32_t)m->delta_x,
        .value3=(int32_t)m->delta_y};routed[routed_count++]=e;m->active=false;
}
static void route_input_locked(const input_event_t *e) {
    routed[routed_count++]=*e;
    if(e->code==777)g_window_server.dragging_identifier=1;
}
static void window_route_pointer_transaction_locked(const input_event_t *e) {
    route_input_locked(e);
}
#define DESKTOP_APP_NONE 0
static unsigned window_input_take_desktop_launch_locked(void) {assert(window_locked);return 0;}
static bool window_input_take_focus_cycle_request_locked(void) {assert(window_locked);return false;}
static void desktop_cycle_window_focus(void) {assert(0);}
static bool desktop_launch_enqueue(unsigned app) {(void)app;assert(0);return false;}
static bool desktop_restore_minimized_app(unsigned app) {(void)app;assert(0);return false;}
static void compositor_frame_run(void) {assert(0);}
static unsigned power_calls, last_power;
static int power_system_request(unsigned action) {
 assert(!window_locked && !g_window_server.desktop.power_request);
 ++power_calls;last_power=action;return K_EIO;
}
static bool window_flush_event_wakes(void) { return true; }
uint32_t wake_all(wait_queue_t *q) { assert(q && depth==baseline);return 0; }
static int clipped(int value,int delta) {
    int64_t next=(int64_t)value+delta;
    return next<0?0:next>100?100:(int)next;
}
''' + pump + old + r'''
int main(void) {
    input_event_t out[INPUT_CORE_CAPACITY+2], expected[INPUT_CORE_CAPACITY+2];
    assert(!input_core_pop_batch(out,16) && !locks);
    const size_t capacities[]={0,1,2,15,16,17,63,64,255,256,257,SIZE_MAX};
    unsigned cases=0;
    for(unsigned head=0;head<INPUT_CORE_CAPACITY;++head)
      for(unsigned count=0;count<=INPUT_CORE_CAPACITY;++count)
        for(unsigned c=0;c<sizeof(capacities)/sizeof(*capacities);++c) {
            seed(head,count);memset(out,0x5a,sizeof(out));memcpy(expected,out,sizeof(out));
            size_t n=capacities[c]<count?capacities[c]:count;
            for(size_t i=0;i<n;++i)
                memcpy(&expected[i],&g_input.events[(head+i)%256],sizeof(*expected));
            unsigned before=locks;
            unsigned published=publications;
            unsigned copy_before=copies,bytes_before=copied_bytes;
            assert(input_core_pop_batch(out,capacities[c])==n);
            assert(locks-before==(capacities[c]!=0));
            assert(publications-published==(n!=0));
            assert(copies-copy_before==(n==0?0:n<=256-head?1:2));
            assert(copied_bytes-bytes_before==n*sizeof(*out));
            assert(!memcmp(out,expected,sizeof(out)) && !depth);
            assert(g_input.read==(head+n)%INPUT_CORE_CAPACITY);
            assert(g_input.count==count-n && input_core_pending()==count-n);
            input_event_t e;
            for(unsigned i=(unsigned)n;i<count;++i) {
                expected[0]=event_for(i);assert(input_core_pop(&e)==K_OK);
                assert(e.timestamp==expected[0].timestamp && e.device_id==expected[0].device_id);
                assert(e.type==expected[0].type && e.flags==expected[0].flags && e.code==expected[0].code);
                assert(e.value==expected[0].value && e.value2==expected[0].value2 && e.value3==expected[0].value3);
            }
            ++cases;
        }
    for(baseline=0;baseline<32;++baseline) {
        seed(253,16);depth=baseline;assert(input_core_pop_batch(out,SIZE_MAX)==16);
        assert(depth==baseline);assert(!input_core_pop_batch(0,16));
        assert(!input_core_pop_batch(out,0));assert(depth==baseline);depth=0;
    }
    baseline=0;
    /* 用逐次边缘截断作独立参考；合并后的最终坐标不能随批次划分改变。 */
    const int starts[]={0,1,50,99,100};
    const int dx[]={-80,20,80,-20},dy[]={-40,10,40,-10};
    for(unsigned sx=0;sx<5;++sx)for(unsigned sy=0;sy<5;++sy)
    for(unsigned paired=0;paired<2;++paired) {
        seed(253,paired?4:8);
        int expected_x=starts[sx],expected_y=starts[sy];
        for(unsigned i=0;i<4;++i) {
            expected_x=clipped(expected_x,dx[i]);expected_y=clipped(expected_y,dy[i]);
            input_event_t e={.timestamp=i+1,.type=paired?INPUT_EVENT_POINTER:INPUT_EVENT_RELATIVE};
            if(paired) {
                e.value2=dx[i];e.value3=dy[i];g_input.events[(253+i)%256]=e;
            } else {
                e.code=INPUT_REL_X;e.value=dx[i];g_input.events[(253+2*i)%256]=e;
                e.code=INPUT_REL_Y;e.value=dy[i];g_input.events[(254+2*i)%256]=e;
            }
        }
        routed_count=0;g_window_server.dragging_identifier=0;
        window_server_pump_input_mode(false);
        int actual_x=starts[sx],actual_y=starts[sy];
        for(unsigned i=0;i<routed_count;++i) {
            assert(routed[i].type==INPUT_EVENT_POINTER);
            actual_x=clipped(actual_x,routed[i].value2);
            actual_y=clipped(actual_y,routed[i].value3);
        }
        assert(actual_x==expected_x && actual_y==expected_y && !input_core_pending());
    }
    puts("Real input pump: 50 paired/separate-axis edge reversals preserve sequential clipped coordinates");
    /* Compare the actual new pump with its old single-pop loop. Include every
     * position of a drag-start barrier, wrapped rings and mixed motion types. */
    unsigned pumps=0;
    for(unsigned head=0;head<256;head+=17)for(unsigned barrier=0;barrier<256;++barrier) {
        input_event_t original[256],reference[512];
        for(unsigned i=0;i<256;++i) {
            original[i]=event_for(i);
            if(i%7<4) {original[i].type=INPUT_EVENT_RELATIVE;original[i].code=i%2?INPUT_REL_X:INPUT_REL_Y;}
            else if(i%7==4) {original[i].type=INPUT_EVENT_POINTER;original[i].code=0;}
            if(i==barrier) {original[i].type=INPUT_EVENT_KEY;original[i].code=777;}
        }
        unsigned expected_count,expected_pending,old_locks;
        for(unsigned mode=0;mode<2;++mode) {
            seed(head,256);for(unsigned i=0;i<256;++i)g_input.events[(head+i)%256]=original[i];
            routed_count=0;g_window_server.dragging_identifier=0;unsigned before=locks;
            if(!mode) {
                old_pump(false);expected_count=routed_count;expected_pending=input_core_pending();
                memcpy(reference,routed,routed_count*sizeof(*routed));old_locks=locks-before;
            } else {
                window_server_pump_input_mode(false);
                assert(routed_count==expected_count && input_core_pending()==expected_pending);
                assert(!memcmp(reference,routed,routed_count*sizeof(*routed)));
                assert(locks-before < old_locks && !depth);
            }
        }
        ++pumps;
    }
    /* 两种电源请求都须在窗口锁外执行，且错误返回后不重复派发。 */
    for(unsigned action=0;action<2;++action) {
        seed(0,0);g_window_server.desktop.power_request=action+1;
        window_server_pump_input_mode(false);
        assert(power_calls==action+1 && last_power==action);
        window_server_pump_input_mode(false);assert(power_calls==action+1);
    }
    seed(0,0);pthread_t workers[4];unsigned received=0,last[4]={0};bool seen[4]={0};
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&workers[i],0,produce,(void *)(uintptr_t)i));
    while(atomic_load(&done)<4 || input_core_pending()) {
        size_t count=input_core_pop_batch(out,16);
        if(!count) {sched_yield();continue;}
        for(size_t i=0;i<count;++i) {
            unsigned id=out[i].device_id,seq=out[i].code%10000;
            assert(id<4 && out[i].code/10000==id);
            assert(!seen[id] || seq>last[id]);seen[id]=true;last[id]=seq;
            assert(out[i].type==INPUT_EVENT_KEY && out[i].timestamp==out[i].code+1);
            assert(out[i].value==(int32_t)out[i].code && out[i].value2==-7 && out[i].value3==11);
            ++received;
        }
    }
    for(unsigned i=0;i<4;++i)assert(!pthread_join(workers[i],0));
    assert(received && received+input_core_dropped()==40000 && !input_core_pending() && !depth);
    printf("Input batch: %u FIFO/canary/wrap/capacity cases, 32 nested depths, %u real-pump ordering/drag-budget comparisons and 40000 concurrent events passed\n",cases,pumps);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-input-batch-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-Wno-unused-parameter','-I'+str(root/'include'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary]
    subprocess.run(command,input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
    broken = program
    for expression in (
        '(motion.delta_x < 0 && event.value2 > 0)',
        '(motion.delta_x > 0 && event.value2 < 0)',
        '(motion.delta_y < 0 && event.value3 > 0)',
        '(motion.delta_y > 0 && event.value3 < 0)',
    ):
        assert expression in broken
        broken = broken.replace(expression, 'false')
    for expression in ('(previous < 0 && event.value > 0)',
                       '(previous > 0 && event.value < 0)'):
        assert expression in broken
        broken = broken.replace(expression, '((void)previous, false)')
    subprocess.run(command,input=broken,text=True,check=True)
    failed = subprocess.run([binary],capture_output=True,text=True,timeout=60,
        preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert failed.returncode == -signal.SIGABRT and 'actual_x==expected_x' in failed.stderr
    print('Negative control rejected: merging reversed motion changes clipped pointer coordinates')
    locked_copy = program.replace('window_unlock();\n        window_control_copy_complete(&copy);',
        'window_control_copy_complete(&copy);\n        window_unlock();')
    assert locked_copy != program
    subprocess.run(command,input=locked_copy,text=True,check=True)
    failed = subprocess.run([binary],capture_output=True,text=True,timeout=60,
        preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert failed.returncode == -signal.SIGABRT and '!window_locked' in failed.stderr
    print('Negative control rejected: clipboard publication while window lock is held')
