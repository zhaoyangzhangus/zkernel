"""Actual borrowed TID hint plus detach: search cost and freed-node safety."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/process.c').read_text()
start = source.index('void process_detach_thread(')
detach = source[start:source.index('static bool process_is_signaled(', start)]
lookup = source[source.index('static thread_t *process_monitor_thread_locked('):
                source.index('uint64_t linux_thread_visible_tid(')]
lookup = lookup.replace('thread_t *thread = list_entry(node,',
                        '++visits; thread_t *thread = list_entry(node,')
fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#define PROCESS_RUNNING 1U
#define PROCESS_EXITING 2U
#define PROCESS_DEAD 3U
#define PROCESS_FLAG_EVER_HAD_THREAD 1U
typedef struct node {struct node *next,*prev;} list_head_t;
#define list_entry(n,t,m) ((t *)((char *)(n)-offsetof(t,m)))
typedef struct {
 pthread_mutex_t lock;atomic_uint state;unsigned thread_count,flags;
 int64_t exit_code;uint64_t retired_cpu_ns;
 list_head_t threads,*monitor_thread_cursor,*enumeration_thread_cursor;unsigned notifications;
} process_t;
typedef struct {
 process_t *process;list_head_t process_node;uint64_t tid;
 int64_t exit_code;struct {uint64_t runtime_ns;} sched;
} thread_t;
static _Thread_local bool held;
static unsigned visits;
static void process_thread_lock(process_t *p) {assert(!held);pthread_mutex_lock(&p->lock);held=true;}
static void process_thread_unlock(process_t *p) {assert(held);held=false;pthread_mutex_unlock(&p->lock);}
static void list_del(list_head_t *n) {assert(held);n->prev->next=n->next;n->next->prev=n->prev;n->next=n->prev=n;}
static void process_publish_exit(process_t *p) {assert(!held && !p->thread_count && atomic_load(&p->state)==PROCESS_DEAD);++p->notifications;}
@FUNCTIONS@
static void setup(process_t *p) {
 *p=(process_t){0};pthread_mutex_init(&p->lock,NULL);atomic_init(&p->state,PROCESS_RUNNING);
 p->flags=PROCESS_FLAG_EVER_HAD_THREAD;p->threads.next=p->threads.prev=&p->threads;visits=0;
}
static thread_t *add(process_t *p,uint64_t tid) {
 thread_t *t=calloc(1,sizeof(*t));assert(t);t->process=p;t->tid=tid;t->sched.runtime_ns=3;
 process_thread_lock(p);list_head_t *n=&t->process_node;
 n->next=&p->threads;n->prev=p->threads.prev;p->threads.prev->next=n;p->threads.prev=n;++p->thread_count;
 process_thread_unlock(p);return t;
}
static thread_t *find(process_t *p,uint64_t tid) {
 process_thread_lock(p);thread_t *t=process_monitor_thread_locked(p,tid);
 if(t)assert(t->tid==tid);process_thread_unlock(p);return t;
}
static void remove_thread(thread_t *t) {process_detach_thread(t);process_detach_thread(t);free(t);}
static void finish(process_t *p) {
 assert(!p->thread_count && p->notifications==1 && !p->monitor_thread_cursor);
 pthread_mutex_destroy(&p->lock);
}
static void *reader(void *arg) {
 process_t *p=arg;
 for(unsigned i=0;i<5000;++i) {
  process_thread_lock(p);thread_t *t=process_monitor_thread_locked(p,1+i%16);
  if(t)assert(t->tid==1+i%16);process_thread_unlock(p);
 }
 return NULL;
}
int main(void) {
 process_t p;setup(&p);thread_t *items[256];
 for(unsigned i=0;i<256;++i)items[i]=add(&p,i+1);
 assert(find(&p,256)==items[255] && visits==256);
 for(unsigned i=0;i<10000;++i)assert(find(&p,256)==items[255]);
 assert(visits==256); // one initial scan, repeated tail TID is constant-time
 assert(!find(&p,999) && visits==512);assert(find(&p,256)==items[255] && visits==512);
 p.enumeration_thread_cursor=&items[255]->process_node;
 remove_thread(items[0]);assert(!p.enumeration_thread_cursor);
 assert(find(&p,256)==items[255] && visits==512);
 p.enumeration_thread_cursor=&items[255]->process_node;
 remove_thread(items[255]);
 assert(!p.enumeration_thread_cursor);
 // No early cursor assertion: a broken invalidation must reach the real UAF.
 assert(!find(&p,256));
 for(unsigned i=1;i<255;++i)remove_thread(items[i]);
 assert(p.retired_cpu_ns==768);finish(&p);
 for(unsigned round=0;round<100;++round) {
  setup(&p);for(unsigned i=0;i<16;++i)items[i]=add(&p,i+1);
  pthread_t worker;assert(!pthread_create(&worker,NULL,reader,&p));
  for(unsigned i=0;i<16;++i)remove_thread(items[i]);
  assert(!pthread_join(worker,NULL));assert(p.retired_cpu_ns==48);finish(&p);
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, body):
        path = directory / (name + '.c')
        path.write_text(fixture.replace('@FUNCTIONS@', body + lookup))
        binary = directory / name
        subprocess.run(['cc', '-std=c11', '-pthread', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True)
    result = run('monitor', detach)
    assert result.returncode == 0, result.stderr
    stale = detach.replace('process->monitor_thread_cursor = 0;', '(void)0;')
    result = run('stale', stale)
    assert result.returncode != 0 and 'heap-use-after-free' in result.stderr, result.stderr
print('TID hint: 10000 tail queries reuse one scan, cached/noncached removal and 100 concurrent query/detach rounds passed; stale hint reproduces ASAN UAF')
