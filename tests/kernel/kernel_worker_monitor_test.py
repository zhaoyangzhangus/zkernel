"""Actual worker registry: accounting, lifetime, bounded snapshots and races."""
from pathlib import Path
import subprocess
import tempfile
import resource

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/sched/monitor.c').read_text()
actual = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <uapi/process.h>
#include <uapi/syscall.h>
typedef int kstatus_t;
#define K_OK 0
#define K_ENOENT -2
#define K_EINVAL -22
typedef unsigned spinlock_t;
typedef struct thread {
 uint64_t tid;void *process;atomic_uint state;uint16_t current_cpu;
 struct {uint64_t runtime_ns;} sched;
 struct thread *monitor_next;const char *monitor_name;uint64_t monitor_created_ns,monitor_retired_ns;
} thread_t;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool held;
static uint32_t cpus=4;
static uint64_t scheduler_lock(spinlock_t *lock){(void)lock;assert(!held);pthread_mutex_lock(&mutex);held=true;return 123;}
static void scheduler_unlock(spinlock_t *lock,uint64_t flags){(void)lock;assert(held&&flags==123);held=false;pthread_mutex_unlock(&mutex);}
static uint64_t x86_read_tsc(void){assert(held);static uint64_t ticks;return ++ticks;}
static uint64_t x86_tsc_to_ns(uint64_t t){return t;}
static uint32_t x86_smp_started_count(void){assert(held);return cpus;}
''' + actual + r'''
static void *reader(void *unused){
 (void)unused;os_task_info_t info;os_thread_snapshot_t thread;
 for(unsigned i=0;i<30000;++i){
  assert(sched_monitor_query(0,&info)==K_OK);
  assert(info.flags==(OS_TASK_KERNEL|OS_TASK_PROTECTED));
  int status=sched_monitor_thread(i%3,&thread);
  assert(status==K_OK||status==K_ENOENT);
  if(status==K_OK)assert(thread.name[sizeof(thread.name)-1]=='\0');
 }
 return NULL;
}
int main(void){
 os_process_snapshot_t process;os_thread_snapshot_t snapshot;os_task_info_t info;
 assert(sched_monitor_process(&process)==K_ENOENT);
 assert(sched_monitor_process(NULL)==K_EINVAL);
 assert(sched_monitor_thread(0,NULL)==K_EINVAL);
 assert(sched_monitor_query(0,NULL)==K_EINVAL);
 thread_t a={.tid=7,.state=OS_THREAD_RUNNING,.current_cpu=2,.sched.runtime_ns=100};
 thread_t b={.tid=8,.state=OS_THREAD_BLOCKED,.sched.runtime_ns=30};
 thread_t user={.tid=9,.process=&a};
 thread_t zero={0};sched_monitor_register(&zero,"invalid-zero-tid");
 sched_monitor_register(NULL,"invalid");sched_monitor_register(&user,"user");
 sched_monitor_register(&a,NULL);assert(sched_monitor_process(&process)==K_ENOENT);
 sched_monitor_register(&a,"compositor");sched_monitor_register(&b,"irq-deferred");
 sched_monitor_register(&a,"duplicate");
 assert(sched_monitor_process(&process)==K_OK&&process.thread_count==2);
 assert(process.pid==OS_KERNEL_WORKERS_PID&&!strcmp(process.name,"Kernel workers"));
 assert(sched_monitor_thread(0,&snapshot)==K_OK&&snapshot.tid==8);
 assert(snapshot.process_pid==OS_KERNEL_WORKERS_PID&&snapshot.state==OS_THREAD_BLOCKED);
 assert(sched_monitor_thread(1,&snapshot)==K_OK&&snapshot.current_cpu==2&&!strcmp(snapshot.name,"compositor"));
 assert(sched_monitor_thread(2,&snapshot)==K_ENOENT);
 assert(sched_monitor_thread(UINT32_MAX,&snapshot)==K_ENOENT);
 assert(sched_monitor_query(0,&info)==K_OK&&info.runtime_ns==130&&info.cpu_count==4&&info.thread_count==2);
 assert(!info.rss_bytes&&!info.committed_bytes&&info.flags==(OS_TASK_KERNEL|OS_TASK_PROTECTED));
 assert(sched_monitor_query(7,&info)==K_OK&&info.runtime_ns==100&&info.state==OS_THREAD_RUNNING);
 uint64_t identity=info.create_time_ns;assert(identity);
 assert(sched_monitor_query(99,&info)==K_ENOENT);
 cpus=0;assert(sched_monitor_query(0,&info)==K_OK&&info.cpu_count==1);cpus=4;
 sched_monitor_unregister(NULL);sched_monitor_unregister(&user);
 sched_monitor_unregister(&a);sched_monitor_unregister(&a);
 assert(sched_monitor_query(7,&info)==K_ENOENT);
 assert(sched_monitor_query(0,&info)==K_OK&&info.runtime_ns==130&&info.thread_count==1);
 sched_monitor_register(&a,"compositor");
 assert(sched_monitor_query(7,&info)==K_OK&&info.create_time_ns!=identity);
 sched_monitor_unregister(&a);
 pthread_t readers[3];for(unsigned i=0;i<3;++i)assert(!pthread_create(&readers[i],NULL,reader,NULL));
 for(unsigned i=0;i<10000;++i){
  thread_t *t=calloc(1,sizeof(*t));assert(t);t->tid=100+i;t->sched.runtime_ns=1;
  sched_monitor_register(t,"a-name-that-is-longer-than-the-snapshot-name-buffer");
  sched_monitor_unregister(t);free(t);
 }
 for(unsigned i=0;i<3;++i)assert(!pthread_join(readers[i],NULL));
 assert(sched_monitor_query(0,&info)==K_OK&&info.runtime_ns==10130);
 b.sched.runtime_ns=UINT64_MAX;assert(sched_monitor_query(0,&info)==K_OK&&info.runtime_ns==UINT64_MAX);
 sched_monitor_unregister(&b);
 assert(sched_monitor_query(0,&info)==K_OK&&info.runtime_ns==UINT64_MAX&&!info.thread_count);
}
'''
with tempfile.TemporaryDirectory(prefix='kernel-worker-monitor-') as directory:
    binary = str(Path(directory)/'test')
    compiler = ['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                '-fsanitize=address,undefined','-fno-pie','-no-pie',
                '-I'+str(root/'include'),'-x','c','-','-o',binary]
    subprocess.run(compiler,input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    for label, before, after in (
        ('unprotected kernel targets',
         'info->flags = OS_TASK_PROTECTED | OS_TASK_KERNEL;',
         'info->flags = OS_TASK_KERNEL;'),
        ('double-counted retired runtime on re-registration',
         'runtime - thread->monitor_retired_ns', 'runtime'),
    ):
        negative = code.replace(before, after, 1)
        assert negative != code
        subprocess.run(compiler,input=negative,text=True,check=True)
        result = subprocess.run([binary],capture_output=True,text=True,timeout=30)
        assert result.returncode != 0 and 'Assertion' in result.stderr, label
        print('Negative control rejected:',label)
print('Kernel workers: actual registry/query, protected aggregate, identity, retired CPU, overflow and 90000 concurrent snapshots/10000 lifetimes passed')
