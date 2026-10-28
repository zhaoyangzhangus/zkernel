"""真实线程枚举和摘除路径：索引缓存、成员变化及并发释放。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/process.c').read_text()
enumerate_thread = source[source.index('kstatus_t process_enumerate_thread('):
                          source.index('static process_t *process_monitor_find(')]
detach_start = source.index('void process_detach_thread(')
detach = source[detach_start:source.index('static bool process_is_signaled(', detach_start)]

fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/process.h>
#include <uapi/syscall.h>
#define K_OK 0
#define K_EINVAL -22
#define K_ENOENT -2
#define PROCESS_RUNNING 1U
#define PROCESS_EXITING 2U
#define PROCESS_DEAD 3U
#define PROCESS_FLAG_EVER_HAD_THREAD 1U
typedef int kstatus_t;
typedef struct node {struct node *next,*prev;} list_head_t;
#define list_entry(n,t,m) ((t *)((char *)(n)-offsetof(t,m)))
typedef struct process {
 pthread_mutex_t lock;list_head_t global_node,threads;
 list_head_t *monitor_thread_cursor,*enumeration_thread_cursor;
 uint32_t enumeration_thread_index,thread_count,flags;
 uint64_t pid,retired_cpu_ns;int64_t exit_code;atomic_uint state,refs;
} process_t;
typedef struct thread {
 process_t *process;list_head_t process_node;uint64_t tid;atomic_uint state;
 uint32_t current_cpu;int64_t exit_code;struct {uint64_t runtime_ns;} sched;
} thread_t;
static pthread_mutex_t registry_lock=PTHREAD_MUTEX_INITIALIZER;
static list_head_t g_process_registry,*g_process_monitor_cursor;
static _Thread_local bool registry,locked;
static void process_registry_lock(void) {assert(!registry&&!locked);pthread_mutex_lock(&registry_lock);registry=true;}
static void process_registry_unlock(void) {assert(registry&&!locked);registry=false;pthread_mutex_unlock(&registry_lock);}
static void process_thread_lock(process_t *p) {assert(!locked);pthread_mutex_lock(&p->lock);locked=true;}
static void process_thread_unlock(process_t *p) {assert(locked);locked=false;pthread_mutex_unlock(&p->lock);}
static void object_get(process_t *p) {assert(registry&&locked);atomic_fetch_add(&p->refs,1);}
static void object_put(process_t *p) {assert(!registry&&!locked&&atomic_fetch_sub(&p->refs,1)>1);}
static void process_snapshot_name(process_t *p,char *out,size_t capacity) {
 assert(!registry&&!locked&&atomic_load(&p->refs)>1);strncpy(out,"chrome",capacity);
}
static void list_del(list_head_t *n) {assert(locked);n->prev->next=n->next;n->next->prev=n->prev;n->next=n->prev=n;}
static void process_publish_exit(process_t *p) {(void)p;assert(!"permanent thread must stay alive");}
static kstatus_t sched_monitor_thread(uint32_t i,os_thread_snapshot_t *out) {
 (void)i;(void)out;assert(!registry&&!locked);return K_ENOENT;
}
@FUNCTIONS@
static thread_t *add(process_t *p,uint64_t tid) {
 thread_t *t=calloc(1,sizeof(*t));assert(t);t->process=p;t->tid=tid;
 atomic_init(&t->state,OS_THREAD_RUNNING);t->current_cpu=2;
 process_thread_lock(p);
 /* 与实际创建路径相同：在修改链表之前、持线程锁时失效。 */
 p->enumeration_thread_cursor=0;
 list_head_t *node=&t->process_node;node->prev=p->threads.prev;node->next=&p->threads;
 p->threads.prev->next=node;p->threads.prev=node;++p->thread_count;
 process_thread_unlock(p);return t;
}
static void remove_thread(thread_t *t) {process_detach_thread(t);free(t);}
static uint64_t get(unsigned index) {
 os_thread_snapshot_t s;assert(process_enumerate_thread(index,&s)==K_OK);
 assert(s.process_pid==42&&!strcmp(s.name,"chrome"));return s.tid;
}
static process_t p;static thread_t *slots[16];
static void setup(void) {
 memset(&p,0,sizeof(p));pthread_mutex_init(&p.lock,NULL);p.pid=42;
 p.flags=PROCESS_FLAG_EVER_HAD_THREAD;atomic_init(&p.state,PROCESS_RUNNING);atomic_init(&p.refs,1);
 p.threads.next=p.threads.prev=&p.threads;
 g_process_registry.next=g_process_registry.prev=&p.global_node;
 p.global_node.next=p.global_node.prev=&g_process_registry;
 for(unsigned i=0;i<16;++i)slots[i]=add(&p,i+1);
}
static void *reader(void *argument) {
 uintptr_t seed=(uintptr_t)argument;
 for(unsigned n=0;n<30000;++n) {
  seed=seed*1664525U+1013904223U;
  os_thread_snapshot_t s;int result=process_enumerate_thread((seed>>8)%16,&s);
  if(result==K_OK)assert(s.process_pid==42&&s.tid&&s.state==OS_THREAD_RUNNING&&s.current_cpu==2);
  else assert(result==K_ENOENT);
 }
 return NULL;
}
int main(int argc,char **argv) {
 (void)argv;setup();
 assert(get(7)==8);
 if(argc==1) {
  /* 删除前面的其他线程后，原缓存节点仍活着，但它的索引已变化。 */
  remove_thread(slots[2]);slots[2]=0;
  assert(get(7)==9);
 }
 /* 删除缓存节点并立即释放，接下来的枚举不可访问悬空指针。 */
 thread_t *cached=argc==1?slots[8]:slots[7];
 remove_thread(cached);slots[argc==1?8:7]=0;
 assert(get(7)==(argc==1?10:9));
 if(argc>1)return 0;
 slots[2]=add(&p,101);slots[8]=add(&p,102);
 assert(!p.enumeration_thread_cursor);
 pthread_t readers[4];
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&readers[i],NULL,reader,(void*)(uintptr_t)(i+3)));
 for(unsigned n=0;n<5000;++n) {
  unsigned i=1+n%15;remove_thread(slots[i]);slots[i]=add(&p,1000+n);
 }
 for(unsigned i=0;i<4;++i)assert(!pthread_join(readers[i],NULL));
 assert(atomic_load(&p.refs)==1&&p.thread_count==16);
 for(unsigned i=1;i<16;++i)remove_thread(slots[i]);
 p.flags=0;remove_thread(slots[0]);
 return 0;
}
'''
assert 'process->enumeration_thread_cursor = 0;' in detach

with tempfile.TemporaryDirectory(prefix='thread-enumeration-cursor-') as tmp:
    path = Path(tmp)
    def compile_case(name, body):
        text = fixture.replace('@FUNCTIONS@', body + '\n' + enumerate_thread)
        (path / (name + '.c')).write_text(text)
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I' + str(root / 'include'), str(path / (name + '.c')),
                        '-o', str(path / name)], check=True)
        return path / name
    actual = compile_case('actual', detach)
    subprocess.run([str(actual)], check=True, timeout=30)
    stale = compile_case('stale', detach.replace('process->enumeration_thread_cursor = 0;', '(void)0;'))
    wrong = subprocess.run([str(stale)], capture_output=True, text=True)
    assert wrong.returncode != 0 and 'Assertion' in wrong.stderr, wrong.stderr
    dangling = subprocess.run([str(stale), 'cached'], capture_output=True, text=True)
    assert dangling.returncode != 0 and 'heap-use-after-free' in dangling.stderr, dangling.stderr
print('Thread enumeration cursor: index shift, cached-node free and four concurrent readers with 5000 detach/add cycles passed; stale-cache controls fail')
