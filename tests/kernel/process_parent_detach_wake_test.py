"""Actual parent detach and child wait: notification ordering and liveness."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/process.c').read_text()
start = source.index('static process_t *process_parent_reference(')
reference = source[start:source.index('static void process_notify_parent(', start)]
start = source.index('static void process_detach_from_parent(process_t *process) {')
detach = source[start:source.index('static bool process_child_matches(', start)]
start = source.index('static bool process_child_matches(')
predicate = source[start:source.index('/* Called only by the owner of the locked transition', start)]
wait = source[source.index('kstatus_t process_wait_child('):
              source.index('void process_release_runtime_resources(')]
fixture = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#define K_OK 0
#define K_EINVAL -22
#define K_ECHILD -10
#define K_ETIMEDOUT -110
#define PROCESS_RUNNING 1U
#define PROCESS_DEAD 3U
#define OS_PROCESS_WAIT_NOHANG 1U
#define OS_WAIT_INFINITE UINT64_MAX
typedef int kstatus_t;
typedef struct node {struct node *next,*prev;} list_head_t;
#define list_entry(n,t,m) ((t *)((char *)(n)-offsetof(t,m)))
typedef struct wait_queue {pthread_mutex_t gate;pthread_cond_t cond;unsigned waiting,wakes;} wait_queue_t;
typedef struct process {
 list_head_t children,child_node;struct process *parent;
 pthread_mutex_t children_lock,parent_lock;
 uint64_t pid;atomic_uint state,refs;int64_t exit_code;uint32_t exit_signal;
 wait_queue_t child_waitq;
} process_t;
static _Thread_local bool children_held,relation_held;
static atomic_uint destroyed;
static void process_children_lock(process_t *p) {assert(!children_held && !relation_held);pthread_mutex_lock(&p->children_lock);children_held=true;}
static void process_children_unlock(process_t *p) {assert(children_held && !relation_held);children_held=false;pthread_mutex_unlock(&p->children_lock);}
static void process_parent_lock(process_t *p) {assert(!relation_held);pthread_mutex_lock(&p->parent_lock);relation_held=true;}
static void process_parent_unlock(process_t *p) {assert(relation_held);relation_held=false;pthread_mutex_unlock(&p->parent_lock);}
static void list_del(list_head_t *n) {assert(children_held);n->prev->next=n->next;n->next->prev=n->prev;n->next=n->prev=n;}
static void object_get(process_t *p) {assert(p && atomic_fetch_add(&p->refs,1)>0);}
static void object_put(process_t *p) {
 assert(!children_held && !relation_held && atomic_load(&p->refs));
 if(atomic_fetch_sub(&p->refs,1)==1) {
  pthread_mutex_destroy(&p->children_lock);pthread_mutex_destroy(&p->parent_lock);
  pthread_mutex_destroy(&p->child_waitq.gate);pthread_cond_destroy(&p->child_waitq.cond);
  ++destroyed;free(p);
 }
}
static unsigned wake_all(wait_queue_t *q) {
 assert(!children_held && !relation_held);
 pthread_mutex_lock(&q->gate);++q->wakes;pthread_cond_broadcast(&q->cond);pthread_mutex_unlock(&q->gate);return 0;
}
@REFERENCE@
@DETACH@
@PREDICATE@
static kstatus_t wait_on_queue(wait_queue_t *q,bool (*pred)(void *),void *arg,uint64_t timeout) {
 pthread_mutex_lock(&q->gate);
 if(pred(arg)){pthread_mutex_unlock(&q->gate);return K_OK;}
 if(timeout==0){pthread_mutex_unlock(&q->gate);return K_ETIMEDOUT;}
 // 有界看门狗会让旧的漏唤醒问题直接失败，而不是让 QA 挂起。
 struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=2;
 ++q->waiting;
 while(!pred(arg)) {
  int result=pthread_cond_timedwait(&q->cond,&q->gate,&until);
  if(result==ETIMEDOUT){--q->waiting;pthread_mutex_unlock(&q->gate);return K_ETIMEDOUT;}
  assert(result==0);
 }
 --q->waiting;pthread_mutex_unlock(&q->gate);return K_OK;
}
@WAIT@
static process_t *create(uint64_t pid) {
 process_t *p=calloc(1,sizeof(*p));assert(p);p->pid=pid;atomic_init(&p->state,PROCESS_RUNNING);atomic_init(&p->refs,1);
 p->children.next=p->children.prev=&p->children;p->child_node.next=p->child_node.prev=&p->child_node;
 pthread_mutex_init(&p->children_lock,NULL);pthread_mutex_init(&p->parent_lock,NULL);
 pthread_mutex_init(&p->child_waitq.gate,NULL);pthread_cond_init(&p->child_waitq.cond,NULL);return p;
}
static void attach(process_t *p,process_t *c) {
 process_children_lock(p);process_parent_lock(c);
 c->parent=p;list_head_t *n=&c->child_node;n->next=&p->children;n->prev=p->children.prev;p->children.prev->next=n;p->children.prev=n;
 object_get(p);object_get(c);process_parent_unlock(c);process_children_unlock(p);
}
typedef struct {process_t *p;int64_t pid;int result;} job_t;
static void *waiter(void *arg) {
 job_t *j=arg;int64_t pid,code;uint32_t signal;
 j->result=process_wait_child(j->p,j->pid,0,&pid,&code,&signal);return NULL;
}
static void *detacher(void *arg) {process_detach_from_parent(arg);return NULL;}
int main(void) {
 process_detach_from_parent(NULL);
 // 构造失败时，可能只安装了父子关系，还未插入链表。
 process_t *unlisted_parent=create(10),*unlisted_child=create(11);
 unlisted_child->parent=unlisted_parent;object_get(unlisted_parent);
 process_detach_from_parent(unlisted_child);
 assert(!unlisted_child->parent && unlisted_parent->refs==1 && unlisted_child->refs==1 && !unlisted_parent->child_waitq.wakes);
 object_put(unlisted_child);object_put(unlisted_parent);destroyed=0;
 for(unsigned round=0;round<100;++round) {
  process_t *p=create(1),*c=create(2);attach(p,c);
  job_t jobs[2]={{.p=p,.pid=2},{.p=p,.pid=-1}};pthread_t waiters[2],removers[8];
  for(unsigned i=0;i<2;++i)assert(!pthread_create(&waiters[i],NULL,waiter,&jobs[i]));
  for(;;) {
   pthread_mutex_lock(&p->child_waitq.gate);bool ready=p->child_waitq.waiting==2;pthread_mutex_unlock(&p->child_waitq.gate);
   if(ready)break;struct timespec tiny={.tv_nsec=100000};nanosleep(&tiny,NULL);
  }
  for(unsigned i=0;i<8;++i)assert(!pthread_create(&removers[i],NULL,detacher,c));
  for(unsigned i=0;i<8;++i)assert(!pthread_join(removers[i],NULL));
  for(unsigned i=0;i<2;++i){assert(!pthread_join(waiters[i],NULL));assert(jobs[i].result==K_ECHILD);}
  assert(p->child_waitq.wakes==1 && !c->parent && c->refs==1 && p->refs==1);
  process_detach_from_parent(c);assert(p->child_waitq.wakes==1);
  object_put(c);object_put(p);
 }
 assert(destroyed==200);
 // 即使该关系是最后一个持久引用，在 wake_all 访问队列期间也要保留查找得到的所有者。
 process_t *p=create(3),*c=create(4);attach(p,c);object_put(p);
 process_detach_from_parent(c);assert(destroyed==201 && c->refs==1 && !c->parent);object_put(c);
 assert(destroyed==202);return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, body):
        path = directory / (name + '.c')
        path.write_text(fixture.replace('@REFERENCE@', reference).replace('@DETACH@', body)
                        .replace('@PREDICATE@', predicate).replace('@WAIT@', wait))
        binary = directory / name
        subprocess.run(['cc', '-std=c11', '-pthread', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    result = run('wake', detach)
    assert result.returncode == 0, result.stderr
    result = run('no_wake', detach.replace('if (detached) (void)wake_all(&parent->child_waitq);', ''))
    assert result.returncode != 0 and 'jobs[i].result==K_ECHILD' in result.stderr, result.stderr
    put = 'object_put(parent);                /* temporary lookup reference */'
    notify = 'if (detached) (void)wake_all(&parent->child_waitq);'
    early = detach.replace(put, '').replace(notify, put + '\n    ' + notify)
    result = run('early_put', early)
    assert result.returncode != 0 and 'heap-use-after-free' in result.stderr, result.stderr
print('parent detach: 100 rounds with two blocked waiters/eight competing detachers, one unlocked notification and parent lifetime passed; missing wake rejected')
