"""Actual scheduler init and process trap-frame setup with independent layouts."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
runqueue=(root/'kernel/sched/runqueue.c').read_text()
initializer=runqueue[runqueue.index('void initialize_thread('):
                    runqueue.index('void sched_initialize_new_thread(')]
new_initializer=runqueue[runqueue.index('void sched_initialize_new_thread('):
                        runqueue.index('void sched_initialize_test_thread(')]
thread=(root/'kernel/process/thread.c').read_text()
frame=thread[thread.index('    uintptr_t frame_address = '):
             thread.index('    process_thread_lock(process);',thread.index('    uintptr_t frame_address = '))]
worker_sources=['kernel/irq/deferred.c','kernel/graphics/assets.c',
                'kernel/graphics/launcher.c',
                'kernel/graphics/server.c']
worker_clears=[]
for index,filename in enumerate(worker_sources):
    source=(root/filename).read_text()
    start=source.index('    memset(worker, 0, sizeof(*worker));')
    end=source.index('    refcount_init(&worker->object.refs',start)
    worker_clears.append('static void clear_worker_'+str(index)+'(thread_t *worker) {\n'+
                         source[start:end].replace('memset(', 'liteos_memset(')+'}\n')
code=r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <kernel/sched.h>
#include <arch/x86_64/context.h>
void *liteos_memset(void *,int,size_t);
void x86_user_thread_start(void) { abort(); }
static unsigned placements;
void sched_mark_initial_placement(thread_t *thread) {
    assert(atomic_load(&thread->state)==THREAD_NEW);
    ++placements;
}
''' + ''.join(worker_clears) + initializer.replace('memset(', 'liteos_memset(') + new_initializer + r'''
static void setup_frame(thread_t *thread,const arch_trap_frame_t *initial_frame,uint64_t fs_base) {
''' + frame + r'''
}
int main(void) {
    _Static_assert(sizeof(arch_trap_frame_t)==22*sizeof(uint64_t),"Trap frame must have no uninitialized padding");
    _Alignas(thread_t) unsigned char storage[sizeof(thread_t)+128];
    void (*clear_workers[])(thread_t *)={clear_worker_0,clear_worker_1,
                                        clear_worker_2,clear_worker_3};
    unsigned worker_cases=0;
    for(unsigned fn=0;fn<sizeof(clear_workers)/sizeof(clear_workers[0]);++fn)
    for(unsigned offset=16;offset<=64;offset+=_Alignof(thread_t)) {
        memset(storage,0x75,sizeof(storage));thread_t *worker=(void *)(storage+offset);
        clear_workers[fn](worker);
        for(unsigned i=0;i<sizeof(storage);++i)
            assert(storage[i]==((i>=offset && i<offset+sizeof(*worker))?0:0x75));
        ++worker_cases;
    }
    const unsigned priorities[]={0,1,31,255};unsigned cases=0;
    for(unsigned offset=16;offset<=64;offset+=_Alignof(thread_t))for(unsigned cls=0;cls<3;++cls)
    for(unsigned p=0;p<sizeof(priorities)/sizeof(priorities[0]);++p) {
        memset(storage,0x75,sizeof(storage));thread_t *actual=(void *)(storage+offset),expected;
        memset(&expected,0,sizeof(expected));uint64_t tid=UINT64_MAX-cases;
        initialize_thread(actual,tid,(uint8_t)cls,(uint8_t)priorities[p]);
        refcount_init(&expected.object.refs,1);atomic_init(&expected.state,THREAD_READY);
        atomic_init(&expected.remote_wake_queued,false);atomic_init(&expected.remote_wake_lock,0);
        atomic_init(&expected.block_epoch,0);atomic_init(&expected.start_pending,false);
        atomic_init(&expected.blocked_waiter,0);atomic_init(&expected.command_ack,0);
        expected.tid=tid;expected.sched_class=expected.base_sched_class=(uint8_t)cls;
        expected.rt_priority=expected.base_rt_priority=(uint8_t)priorities[p];expected.sched.weight=1024;
        expected.sched.rt_node.next=expected.sched.rt_node.prev=&actual->sched.rt_node;
        expected.process_node.next=expected.process_node.prev=&actual->process_node;
        expected.owned_mutexes.next=expected.owned_mutexes.prev=&actual->owned_mutexes;
        for(unsigned word=0;word<MAX_CPUS/64;++word)expected.affinity.bits[word]=UINT64_MAX;
        assert(!memcmp(actual,&expected,sizeof(expected)));
        for(unsigned i=0;i<offset;++i)assert(storage[i]==0x75);
        for(unsigned i=offset+sizeof(*actual);i<sizeof(storage);++i)assert(storage[i]==0x75);
        ++cases;
    }
    unsigned new_cases=0;
    sched_initialize_new_thread(0,0,true);assert(!placements);
    for(unsigned cpu=0;cpu<MAX_CPUS+3;++cpu)for(unsigned pin=0;pin<2;++pin) {
        thread_t actual,expected;memset(&actual,0x75,sizeof(actual));expected=actual;
        atomic_init(&expected.state,THREAD_NEW);
        atomic_init(&expected.priority_queued,false);expected.priority_next=0;
        atomic_init(&expected.priority_request,0);atomic_init(&expected.exit_defer_count,0);
        atomic_init(&expected.remote_wake_queued,false);atomic_init(&expected.remote_wake_lock,0);
        atomic_init(&expected.block_epoch,0);atomic_init(&expected.start_pending,false);
        atomic_init(&expected.blocked_waiter,0);atomic_init(&expected.command_ack,0);
        expected.sched_class=expected.base_sched_class=SCHED_CLASS_FAIR;
        expected.rt_priority=expected.base_rt_priority=0;
        expected.sched.runtime_ns=expected.sched.vruntime=expected.sched.exec_start_ns=0;
        expected.sched.slice_runtime_ns=0;expected.sched.weight=1024;
        expected.sched.nice=expected.sched.latency_hint=expected.sched.flags=0;
        expected.sched.rt_node.next=expected.sched.rt_node.prev=&actual.sched.rt_node;
        expected.current_cpu=expected.owner_cpu=(uint16_t)(cpu<MAX_CPUS?cpu:0);
        expected.migration_target_cpu=UINT16_MAX;expected.migration_pending=false;
        expected.command_release_next=0;expected.command_release_count=0;
        for(unsigned word=0;word<MAX_CPUS/64;++word) {
            uint64_t bits=UINT64_MAX;
            if(pin && cpu<MAX_CPUS) bits=word==cpu/64 ? UINT64_C(1)<<(cpu%64) : 0;
            expected.affinity.bits[word]=bits;
        }
        sched_initialize_new_thread(&actual,cpu,pin!=0);
        assert(!memcmp(&actual,&expected,sizeof(actual)));++new_cases;
    }
    assert(placements==new_cases);
    _Alignas(16) unsigned char stack[4096],expected_stack[4096];
    unsigned frame_cases=0;
    for(unsigned offset=0;offset<32;++offset)for(unsigned pattern=0;pattern<32;++pattern) {
        memset(stack,0x59,sizeof(stack));memset(expected_stack,0x59,sizeof(expected_stack));
        arch_trap_frame_t input;uint64_t registers[22];
        for(unsigned r=0;r<22;++r)registers[r]=(UINT64_MAX-pattern*17)^(r*UINT64_C(0x102030405060708));
        memcpy(&input,registers,sizeof(input));arch_trap_frame_t saved=input;
        thread_t actual,expected;memset(&actual,0x75,sizeof(actual));
        actual.kernel_stack_top=(vaddr_t)(uintptr_t)(stack+sizeof(stack)-offset);expected=actual;
        uint64_t fs=UINT64_MAX-pattern;
        size_t frame_offset=((uintptr_t)actual.kernel_stack_top-sizeof(input))&~(uintptr_t)15;
        frame_offset-=(uintptr_t)stack;
        uint64_t entry=(uint64_t)(uintptr_t)&x86_user_thread_start;
        memcpy(expected_stack+frame_offset,&input,sizeof(input));
        memcpy(expected_stack+frame_offset-sizeof(entry),&entry,sizeof(entry));
        expected.arch.switch_ctx.rsp=(uintptr_t)stack+frame_offset-sizeof(entry);
        expected.arch.switch_ctx.r12=(uintptr_t)stack+frame_offset;
        expected.arch.switch_ctx.r13=fs;expected.arch.switch_ctx.r14=actual.kernel_stack_top;
        setup_frame(&actual,&input,fs);
        assert(!memcmp(stack,expected_stack,sizeof(stack)) && !memcmp(&actual,&expected,sizeof(actual)));
        assert(!memcmp(&input,&saved,sizeof(input)));++frame_cases;
    }
    printf("Thread initialization: %u worker-clear canary cases, %u field cases and %u exact register/trampoline/aligned-stack layouts passed\n",worker_cases,cases,frame_cases);
    printf("New thread affinity: %u exact layouts covering every CPU, pin/unpin, invalid CPU fallback and one placement per thread passed\n",new_cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-thread-init-') as directory:
    path=Path(directory);obj=str(path/'string.o');binary=str(path/'test')
    subprocess.run(['cc','-std=c11','-O2','-fno-builtin','-mno-sse','-mno-mmx',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie',
                    '-Dmemcpy=liteos_memcpy','-Dmemmove=liteos_memmove',
                    '-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp',
                    '-c',str(root/'kernel/string.c'),'-o',obj],check=True)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-x','none',obj,'-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
