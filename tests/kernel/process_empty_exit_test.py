"""Actual exit transition, final-thread detach and shared notification path."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/process.c').read_text()
start = source.index('static void process_publish_exit(')
functions = source[start:source.index('static bool process_is_signaled(', start)]
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
#define PROCESS_TEARDOWN_CALLBACK_MAX 4U
typedef struct node {struct node *next,*prev;} list_head_t;
typedef struct {
 pthread_mutex_t lock;atomic_uint state;unsigned thread_count,flags;
 int64_t exit_code;unsigned exit_signal;uint64_t retired_cpu_ns;
 list_head_t *monitor_thread_cursor,*enumeration_thread_cursor;
 atomic_uint removed,orphaned,callbacks,notified,parent_wakes,released;
} process_t;
typedef struct {process_t *process;list_head_t process_node;int64_t exit_code;struct {uint64_t runtime_ns;} sched;} thread_t;
typedef void (*process_teardown_callback_t)(process_t *);
static _Thread_local bool held;
static void process_thread_lock(process_t *p) {assert(!held);pthread_mutex_lock(&p->lock);held=true;}
static void process_thread_unlock(process_t *p) {assert(held);held=false;pthread_mutex_unlock(&p->lock);}
static void check(process_t *p) {assert(!held && atomic_load(&p->state)==PROCESS_DEAD && !p->thread_count);}
static void process_registry_remove(process_t *p) {check(p);assert(atomic_fetch_add(&p->removed,1)==0);}
static void process_orphan_children(process_t *p) {check(p);assert(p->removed==1);++p->orphaned;}
static void callback(process_t *p) {check(p);assert(p->orphaned==1);++p->callbacks;assert(!p->notified);}
static unsigned process_snapshot_teardown_callbacks(process_teardown_callback_t *out) {assert(!held);out[0]=callback;return 1;}
static void object_notify_signaled(process_t *p) {check(p);assert(p->callbacks==1);++p->notified;}
static void process_notify_parent(process_t *p) {check(p);assert(p->notified==1);++p->parent_wakes;}
static void process_release_runtime_resources(process_t *p) {check(p);assert(p->parent_wakes==1);++p->released;}
static void list_del(list_head_t *n) {assert(held);n->prev->next=n->next;n->next->prev=n->prev;n->next=n->prev=n;}
@FUNCTIONS@
static void setup(process_t *p,unsigned count) {
 *p=(process_t){0};pthread_mutex_init(&p->lock,NULL);atomic_init(&p->state,PROCESS_RUNNING);
 p->thread_count=count;p->flags=count?PROCESS_FLAG_EVER_HAD_THREAD:0;
}
static void done(process_t *p) {
 assert(atomic_load(&p->state)==PROCESS_DEAD && !p->thread_count);
 assert(p->removed==1 && p->orphaned==1 && p->callbacks==1 && p->notified==1 && p->parent_wakes==1);
 pthread_mutex_destroy(&p->lock);
}
static void *exit_worker(void *arg) {process_begin_exit(arg,-9,9);return NULL;}
static void *detach_worker(void *arg) {process_detach_thread(arg);return NULL;}
int main(void) {
 assert(!process_begin_exit(NULL,-9,9));process_detach_thread(NULL);
 for(unsigned round=0;round<200;++round) {
  process_t p;setup(&p,0);pthread_t jobs[8];
  for(unsigned i=0;i<8;++i)assert(!pthread_create(&jobs[i],NULL,exit_worker,&p));
  for(unsigned i=0;i<8;++i)assert(!pthread_join(jobs[i],NULL));
  assert(p.exit_code==-9 && p.exit_signal==9 && p.released==1);
  assert(!process_begin_exit(&p,-15,15) && p.exit_code==-9);done(&p);
 }
 for(unsigned round=0;round<400;++round) {
  process_t p;setup(&p,1);list_head_t head;
  thread_t t={.process=&p,.exit_code=7,.sched.runtime_ns=11};
  head.next=head.prev=&t.process_node;t.process_node.next=t.process_node.prev=&head;
  pthread_t a,b;assert(!pthread_create(&a,NULL,exit_worker,&p));assert(!pthread_create(&b,NULL,detach_worker,&t));
  assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
  assert(p.exit_code==7 || p.exit_code==-9);assert(!p.released && p.retired_cpu_ns==11);
  process_detach_thread(&t);assert(p.retired_cpu_ns==11);done(&p);
 }
 process_t p;setup(&p,1);assert(process_begin_exit(&p,-9,9));
 assert(atomic_load(&p.state)==PROCESS_EXITING && !p.notified && !p.released);
 pthread_mutex_destroy(&p.lock);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, body):
        path = directory / (name + '.c')
        path.write_text(fixture.replace('@FUNCTIONS@', body))
        binary = directory / name
        subprocess.run(['cc', '-std=c11', '-pthread', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True)
    result = run('empty_exit', functions)
    assert result.returncode == 0, result.stderr
    result = run('old_transition', functions.replace('empty = process->thread_count == 0U;', 'empty = false;'))
    assert result.returncode != 0, 'Old empty exit must fail the regression'
print('empty process exit: 200 concurrent exit rounds, 400 final-detach races and negative passed')
