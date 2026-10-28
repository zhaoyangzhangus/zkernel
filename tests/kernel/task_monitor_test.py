"""Run production monitor/termination with held-reference and lock assertions."""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/process/process.c').read_text()
# Linux TID translation has separate fixtures and depends on the full process
# registry. Extract only the actual monitor/termination implementation here.
functions = (source[source.index('static bool process_monitor_protected('):
                    source.index('uint64_t linux_thread_visible_tid(')] +
             source[source.index('kstatus_t process_task_query('):
                    source.index('static uint64_t read_tsc(')])
runtime_read = 'uint64_t runtime = __atomic_load_n(&thread->sched.runtime_ns, __ATOMIC_RELAXED);'
assert functions.count(runtime_read) == 1
functions = functions.replace(runtime_read, 'assert(held); ++runtime_reads; '+runtime_read)
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <uapi/all.h>
#define K_EINVAL -22
#define K_ENOENT -2
#define K_EPERM -1
#define K_OK 0
#define PAGE_SIZE 4096U
#define PROCESS_RUNNING OS_PROCESS_RUNNING
#define THREAD_DEAD OS_THREAD_DEAD
#define PROCESS_FLAG_INIT_CPU_PINNED 4U
#define THREAD_FLAG_TERMINATE_REQUEST 4U
typedef int kstatus_t;
static kstatus_t sched_monitor_query(uint64_t tid,os_task_info_t *info) {(void)tid;(void)info;return K_ENOENT;}
typedef struct list_head {struct list_head *next,*prev;} list_head_t;
#define list_entry(node,type,member) ((type *)((char *)(node)-offsetof(type,member)))
typedef struct {atomic_uint_fast64_t rss_pages,commit_pages;} vm_space_t;
typedef struct {uint64_t pid,create_time_ns,retired_cpu_ns;atomic_uint state;uint32_t thread_count,flags;vm_space_t *vm;list_head_t threads;list_head_t *monitor_thread_cursor;} process_t;
typedef struct {uint64_t tid,signal_pending;uint32_t flags;atomic_uint state;struct {uint64_t runtime_ns;} sched;list_head_t process_node;} thread_t;
static process_t process,caller={.pid=100};
static thread_t a,b;
static vm_space_t vm;
static unsigned refs,held,terminated,started_cpus=4,lock_calls,allocations,runtime_reads;
static bool thread_only,large_case,allocation_fail,shrink_snapshot,exit_race;
static bool sibling_case;
static int64_t expected_status=-9;
static thread_t many[64];
static bool seen[64];
static void object_get(void *p){assert(p);++refs;}
static void object_put(void *p){assert(p && refs);--refs;}
static process_t *process_monitor_find(uint64_t pid){if(pid!=process.pid)return 0;object_get(&process);return &process;}
static unsigned x86_smp_started_count(void){return started_cpus;}
static void process_thread_lock(process_t *p){assert(p==&process && !held);held=1;++lock_calls;}
static void process_thread_unlock(process_t *p){assert(p==&process && held);held=0;}
static bool process_begin_exit(process_t *p,int64_t status,uint32_t signal){assert(!held && p==&process && status==-9 && signal==9);atomic_store(&p->state,OS_PROCESS_EXITING);return !exit_race;}
static void *kmalloc(size_t size,unsigned flags){
 assert(!held && flags==0 && atomic_load(&process.state)==OS_PROCESS_EXITING);
 ++allocations;
 if(shrink_snapshot){process.threads.next=&many[31].process_node;process.thread_count=33;}
 return allocation_fail?0:malloc(size);
}
static void kfree(void *p){assert(!held && refs==1 && p);free(p);}
static kstatus_t thread_terminate(thread_t *t,int64_t status){
 assert(!held && refs>=2 && status==expected_status);
 if(sibling_case){assert(t==&b && (b.signal_pending&256) && !a.signal_pending);++terminated;return 0;}
 if(thread_only){assert(t==&a && (a.flags&THREAD_FLAG_TERMINATE_REQUEST));++terminated;return 0;}
 if(large_case){
  size_t i=(size_t)(t-many);assert(i<64 && !seen[i]);seen[i]=true;
  for(unsigned j=shrink_snapshot?31:0;j<64;++j)assert(many[j].signal_pending&256);
  ++terminated;
  /* Concurrent detach must not invalidate any held snapshot reference. */
  process.threads.next=&process.threads;process.thread_count=0;
  if(allocation_fail){process.threads.next=&many[i+1<64?i+1:i].process_node;
   if(i==63)process.threads.next=&process.threads;}
  return -16; // running threads leave at the already-published user boundary
 }
 assert((a.signal_pending&256) && (b.signal_pending&256)); // all published before any exit
 assert(t==(terminated?&b:&a));++terminated;return 0;
}
''' + functions + r'''
int main(void){
 process.pid=10;process.create_time_ns=123;process.vm=&vm;process.retired_cpu_ns=40;process.thread_count=2;
 atomic_init(&process.state,OS_PROCESS_RUNNING);atomic_init(&vm.rss_pages,3);atomic_init(&vm.commit_pages,5);
 a.tid=12;a.sched.runtime_ns=100;atomic_init(&a.state,OS_THREAD_RUNNING);
 b.tid=11;b.sched.runtime_ns=20;atomic_init(&b.state,OS_THREAD_BLOCKED);
 process.threads.next=&a.process_node;process.threads.prev=&b.process_node;
 a.process_node.next=&b.process_node;b.process_node.next=&process.threads;
 os_task_info_t info;
 assert(process_task_query(10,0,&info)==0 && info.runtime_ns==160 && info.rss_bytes==12288 && info.committed_bytes==20480 && info.cpu_count==4);
 started_cpus=2;
 assert(process_task_query(10,0,&info)==0 && info.cpu_count==2);
 started_cpus=0;
 assert(process_task_query(10,0,&info)==0 && info.cpu_count==1);
 started_cpus=4;
 /* A single-thread query must not walk unrelated successors after its match. */
 a.process_node.next=0;
 assert(process_task_query(10,12,&info)==0 && info.runtime_ns==100);
 a.process_node.next=&b.process_node;
 assert(process_task_query(10,11,&info)==0 && info.runtime_ns==20 && info.state==OS_THREAD_BLOCKED);
 assert(process_task_query(10,99,&info)==K_ENOENT && !refs && !held);
 assert(process_task_query(99,0,&info)==K_ENOENT && !refs);
 /* Independent saturating total and exact individual identities, including
  * empty lists and a retired total that already reached the ceiling. */
 const uint64_t retired[]={0,40,UINT64_MAX-1,UINT64_MAX};
 for(unsigned count=0;count<=64;++count)for(unsigned r=0;r<4;++r)
 for(unsigned mode=0;mode<3;++mode){
  process.thread_count=count;process.retired_cpu_ns=retired[r];
  process.threads.next=count?&many[0].process_node:&process.threads;
  uint64_t expected=retired[r];unsigned reads=0;
  for(unsigned i=0;i<count;++i){
   many[i].tid=200+i;atomic_store(&many[i].state,i&1?OS_THREAD_RUNNING:OS_THREAD_BLOCKED);
   many[i].sched.runtime_ns=mode==0?i+1:mode==1?UINT64_MAX:i&1?UINT64_MAX:0;
   many[i].process_node.next=i+1==count?&process.threads:&many[i+1].process_node;
   if(expected!=UINT64_MAX){++reads;uint64_t value=many[i].sched.runtime_ns;
    expected=value>UINT64_MAX-expected?UINT64_MAX:expected+value;}
  }
  runtime_reads=0;
  assert(process_task_query(10,0,&info)==K_OK && info.runtime_ns==expected);
  assert(info.thread_count==count && runtime_reads==reads && !refs && !held);
  for(unsigned i=0;i<count;++i){
   runtime_reads=0;
   assert(process_task_query(10,200+i,&info)==K_OK && info.runtime_ns==many[i].sched.runtime_ns);
   assert(info.tid==200+i && info.state==atomic_load(&many[i].state) && !runtime_reads && !refs && !held);
  }
  assert(process_task_query(10,999,&info)==K_ENOENT && !refs && !held);
 }
 process.thread_count=2;process.retired_cpu_ns=40;
 process.threads.next=&a.process_node;a.process_node.next=&b.process_node;b.process_node.next=&process.threads;
 assert(process_terminate_pid(&caller,1,123)==K_EPERM);
 assert(process_terminate_pid(&process,10,123)==K_EPERM);
 assert(process_terminate_pid(&caller,10,999)==K_EPERM && !refs);
 process.flags=PROCESS_FLAG_INIT_CPU_PINNED;
 assert(process_task_query(10,0,&info)==0 && (info.flags&OS_TASK_PROTECTED));
 assert(process_terminate_pid(&caller,10,123)==K_EPERM && !refs);
 assert(process_terminate_tid(&caller,10,12,123)==K_EPERM && !refs);
 process.flags=0;process.vm=0;
 assert(process_task_query(10,0,&info)==0 && (info.flags&OS_TASK_PROTECTED));
 assert(process_terminate_pid(&caller,10,123)==K_EPERM && !refs);
 assert(process_terminate_tid(&caller,10,12,123)==K_EPERM && !refs);
 process.vm=&vm;
 assert(process_terminate_pid(&caller,10,123)==0 && terminated==2 && !refs && !held);
 assert(allocations==0);
 assert(process_terminate_pid(&caller,10,123)==K_EPERM && !refs);
 assert(process_terminate_tid(&caller,10,12,123)==K_EPERM && !refs);
 process.retired_cpu_ns=UINT64_MAX-1;
 assert(process_task_query(10,0,&info)==0 && info.runtime_ns==UINT64_MAX);
 atomic_store(&process.state,OS_PROCESS_RUNNING);thread_only=true;
 assert(process_terminate_tid(&process,10,12,123)==K_EPERM);
 assert(process_terminate_tid(&caller,10,12,999)==K_EPERM && !refs);
 assert(process_terminate_tid(&caller,10,99,123)==K_ENOENT && !refs);
 assert(process_terminate_tid(&caller,10,12,123)==0 && terminated==3 && !refs);
 assert((a.flags&THREAD_FLAG_TERMINATE_REQUEST) && !b.flags);
 assert(atomic_load(&process.state)==OS_PROCESS_RUNNING);
 thread_only=false;large_case=true;
 for(unsigned scenario=0;scenario<3;++scenario){
  allocation_fail=scenario==1;shrink_snapshot=scenario==2;
  terminated=0;lock_calls=0;allocations=0;
  process.thread_count=64;atomic_store(&process.state,OS_PROCESS_RUNNING);
  for(unsigned i=0;i<64;++i){
   many[i].tid=i+200;many[i].signal_pending=0;seen[i]=false;
   many[i].process_node.next=i==63?&process.threads:&many[i+1].process_node;
  }
  process.threads.next=&many[0].process_node;
  assert(process_terminate_pid(&caller,10,123)==0 && !refs && !held);
  assert(terminated==(shrink_snapshot?33:64) && allocations==1);
  if(!allocation_fail)assert(lock_calls==3); // independent of thread count
  for(unsigned i=shrink_snapshot?31:0;i<64;++i)assert(seen[i]);
 }
 exit_race=true;atomic_store(&process.state,OS_PROCESS_RUNNING);terminated=0;allocations=0;
 assert(process_terminate_pid(&caller,10,123)==0 && !terminated && !allocations && !refs);
 /* Normal exit must preserve the caller's status and never signal or
  * terminate the current thread. Dead siblings are not referenced either. */
 large_case=false;exit_race=false;sibling_case=true;expected_status=42;
 atomic_store(&process.state,OS_PROCESS_EXITING);process.thread_count=2;
 process.threads.next=&a.process_node;a.process_node.next=&b.process_node;
 b.process_node.next=&process.threads;a.signal_pending=0;b.signal_pending=0;
 atomic_store(&b.state,OS_THREAD_BLOCKED);terminated=0;object_get(&process);
 process_terminate_siblings(&process,&a,42);
 assert(terminated==1 && refs==1 && !held && !a.signal_pending);
 atomic_store(&b.state,OS_THREAD_DEAD);terminated=0;b.signal_pending=0;
 process_terminate_siblings(&process,&a,42);
 assert(!terminated && !b.signal_pending && refs==1);
 /* Also exercise current/dead exclusions in the allocation-free fallback. */
 process_terminate_threads_fallback(&process,&a,42);
 assert(!terminated && refs==1);
 object_put(&process);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-task-monitor-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
    for label, negative in (
        ('continued saturated scan', code.replace(' && info->runtime_ns != UINT64_MAX', '')),
        ('wrong thread identity', code.replace('if (thread->tid == tid) return thread;', '(void)tid; return thread;'))):
        assert negative != code
        (path/'negative.c').write_text(negative)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-I'+str(root/'include'),str(path/'negative.c'),'-o',str(path/'negative')],check=True)
        result=subprocess.run([str(path/'negative')],capture_output=True,text=True,
                              preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert result.returncode != 0 and 'Assertion' in result.stderr,result.stderr
        print('Negative control rejected: '+label)
apic=(root/'kernel/arch/x86_64/irq/apic.c').read_text()
timer=apic[apic.index('UINT64 lapic_timer_interrupt('):]
assert timer.index('lapic_end_of_interrupt();') < timer.index('process_exit_signal(OS_SIGKILL);')
assert '(saved[16] & 3U) == 3U && !sched_preempt_disabled()' in timer
print('Task monitor: CPU/RSS, references, linear termination snapshot, low-memory fallback, concurrent detach/exit, stale identity and protection passed')
