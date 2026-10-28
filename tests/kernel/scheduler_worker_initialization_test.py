"""执行四条真实工作线程初始化片段及统一调度初始化，检查完整字段和入口栈。"""
from pathlib import Path
import re
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
queue = (root/'kernel/sched/runqueue.c').read_text()
initialize = queue[queue.index('void sched_initialize_new_thread('):queue.index('void sched_initialize_test_thread(')]
core = (root/'kernel/sched/core.c').read_text()
placement = core[core.index('void sched_mark_initial_placement('):core.index('\n}',core.index('void sched_mark_initial_placement('))+2]
sites = [
    ('kernel/graphics/server.c','bool window_server_start_worker(', 'window_server_worker_main'),
    ('kernel/graphics/assets.c','bool desktop_shell_start_asset_worker(', 'desktop_asset_worker_main'),
    ('kernel/graphics/launcher.c','bool desktop_shell_start_launch_worker(', 'desktop_launch_worker_main'),
    ('kernel/irq/deferred.c','bool deferred_start_worker(', 'deferred_worker_main'),
]
fragments = []
for path, entry, function in sites:
    source = (root/path).read_text()
    start = source.index('memset(worker, 0, sizeof(*worker));',source.index(entry))
    end = source.index(';', source.index('x86_prepare_kernel_thread(', start)) + 1
    fragments.append(source[start:end])
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/process.h>
#include "kernel/sched/internal.h"
static uint8_t g_window_server_worker_stack[32768] __attribute__((aligned(16)));
static uint8_t g_desktop_asset_worker_stack[32768] __attribute__((aligned(16)));
static uint8_t g_desktop_launch_worker_stack[32768] __attribute__((aligned(16)));
static uint8_t g_deferred_worker_stack[32768] __attribute__((aligned(16)));
static uint64_t next_tid=100;
uint64_t process_allocate_task_id(void){return next_tid++;}
void x86_kernel_thread_start(void){assert(!"test must not execute the trampoline");}
'''
for _,_,function in sites:
    prefix += f'static void {function}(void *argument){{(void)argument;}}\n'
wrappers = ''
for index, fragment in enumerate(fragments):
    wrappers += f'static void site_{index}(thread_t *worker,uint32_t cpu,uint32_t index){{\n'
    wrappers += '''uint32_t worker_cpu=cpu,asset_cpu=cpu,launch_cpu=cpu,cpu_id=cpu;
 (void)worker_cpu;(void)asset_cpu;(void)launch_cpu;(void)cpu_id;(void)index;
'''+fragment+'\n}\n'
main = r'''
int main(void){
 void (*sites[])(thread_t *,uint32_t,uint32_t)={site_0,site_1,site_2,site_3};
 void (*entries[])(void *)={window_server_worker_main,desktop_asset_worker_main,
  desktop_launch_worker_main,deferred_worker_main};
 void *stacks[]={g_window_server_worker_stack,g_desktop_asset_worker_stack,
  g_desktop_launch_worker_stack,g_deferred_worker_stack};
 unsigned cases=0;
 for(unsigned site=0;site<sizeof(sites)/sizeof(*sites);++site)for(unsigned cpu=0;cpu<MAX_CPUS;++cpu)
  for(unsigned index=0;index<1U;++index){
   thread_t t;memset(&t,0xa5,sizeof(t));sites[site](&t,cpu,index);++cases;
   assert(atomic_load(&t.state)==THREAD_READY);
   assert(t.object.type==KOBJECT_TYPE_THREAD&&atomic_load(&t.object.refs.value)==1);
   assert(!t.object.flags&&!t.object.ops&&!t.process);
   uint64_t tid=site==0?UINT64_MAX-1:site==1?UINT64_MAX-4:site==2?UINT64_MAX-5:
     next_tid-1;
   assert(t.tid==tid&&t.owner_cpu==cpu&&t.current_cpu==cpu);
   assert(t.migration_target_cpu==UINT16_MAX&&!t.migration_pending);
   assert(t.sched_class==SCHED_CLASS_FAIR&&t.base_sched_class==SCHED_CLASS_FAIR);
   assert(!t.rt_priority&&!t.base_rt_priority&&t.sched.weight==1024&&!t.sched.nice);
   assert(t.sched.flags==SCHED_ENTITY_INITIAL_PLACEMENT&&!t.sched.vruntime);
   assert(!t.sched.runtime_ns&&!t.sched.exec_start_ns&&!t.sched.slice_runtime_ns);
   assert(t.sched.rt_node.next==&t.sched.rt_node&&t.sched.rt_node.prev==&t.sched.rt_node);
   assert(t.process_node.next==&t.process_node&&t.owned_mutexes.next==&t.owned_mutexes);
   for(unsigned word=0;word<MAX_CPUS/64;++word)
    assert(t.affinity.bits[word]==(word==cpu/64?(1ULL<<(cpu%64)):0));
   assert(!atomic_load(&t.remote_wake_queued)&&!atomic_load(&t.priority_queued));
   assert(!atomic_load(&t.priority_request)&&!atomic_load(&t.remote_wake_lock));
   assert(!atomic_load(&t.block_epoch)&&!atomic_load(&t.start_pending));
   assert(!atomic_load(&t.exit_defer_count)&&!atomic_load(&t.blocked_waiter));
   assert(!atomic_load(&t.command_ack)&&!t.command_release_next&&!t.command_release_count);
   assert(!t.priority_next);
   void *stack=stacks[site];
   assert(t.kernel_stack_base==stack&&t.kernel_stack_size==32768);
   assert(t.kernel_stack_top==(uintptr_t)stack+32768);
   assert(t.arch.switch_ctx.rsp==t.kernel_stack_top-sizeof(uint64_t));
   assert(*(uint64_t *)(uintptr_t)t.arch.switch_ctx.rsp==(uintptr_t)&x86_kernel_thread_start);
   assert(t.arch.switch_ctx.r12==(uintptr_t)entries[site]);
   assert(t.arch.switch_ctx.r13==0&&t.arch.switch_ctx.r14==t.kernel_stack_top);
   assert(!t.arch.fs_base&&!t.arch.switch_ctx.rbx&&!t.arch.switch_ctx.rbp&&!t.arch.switch_ctx.r15);
  }
 printf("Kernel worker initialization: %u cases, all 256 CPUs, four active paths; scheduler state, identity, pinning and ABI stacks PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='scheduler-workers-') as directory:
    binary=Path(directory)/'test'
    def run(source,negative=False):
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I'+str(root/'include'),'-I'+str(root),'-x','c','-',
                        '-o',str(binary)],input=source,text=True,check=True)
        result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=30,
                              preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        if negative:
            assert result.returncode and 'migration_target_cpu' in result.stderr,result.stderr
            print('Negative control rejected: manual initialization omits scheduler state')
        else:
            assert result.returncode==0,result.stderr
            print(result.stdout.strip())
    code=prefix+placement+initialize+wrappers+main
    run(code)
    legacy=re.sub(r'sched_initialize_new_thread\(worker, [^;]+;',
        '/* 模拟旧手工路径：调度器迁移和放置元数据未初始化。 */',wrappers)
    assert legacy!=wrappers
    run(prefix+placement+initialize+legacy+main,True)
