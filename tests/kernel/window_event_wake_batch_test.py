"""Production window wake batches: overflow, lifetime, requeue and concurrency."""
from pathlib import Path
import argparse
import resource
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-controls', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/input_events.c').read_text()
begin = source.index('void window_event_reset_ready_locked(')
end = source.index('static bool window_event_discard_motion_locked(', begin)
production = source[begin:end]
production = re.sub(r'\bready\[index\]', '(*(++ready_accesses, &ready[index]))', production)
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#define WINDOW_SERVER_MAX_WINDOWS 64U
typedef struct window window_server_window_t;
typedef struct {window_server_window_t *owner;} wait_queue_t;
struct window {
 bool event_wake_pending;uint32_t event_count;
 wait_queue_t event_waitq;
 atomic_uint refs,wakes,notifications;
};
static struct {
 struct {uint32_t count;window_server_window_t *children[64];} scene;
 window_server_window_t *event_ready_windows[64];
 uint32_t event_ready_count;bool event_ready_overflow;
} g_window_server;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned locked;
static void window_lock(void){assert(!locked);assert(!pthread_mutex_lock(&mutex));locked=1;}
static void window_unlock(void){assert(locked);locked=0;assert(!pthread_mutex_unlock(&mutex));}
static void window_event_schedule_wake_locked(window_server_window_t *);
static window_server_window_t *requeue,*close_during_wake;
static unsigned ready_accesses;
static void object_get(window_server_window_t *w){assert(locked);assert(atomic_fetch_add(&w->refs,1)>0);}
static void object_put(window_server_window_t *w){assert(!locked);assert(atomic_fetch_sub(&w->refs,1)>0);}
static unsigned wake_one(wait_queue_t *q){
 assert(!locked);window_server_window_t *w=q->owner;
 assert(atomic_load(&w->refs)>0);atomic_fetch_add(&w->wakes,1);
 if(requeue==w){requeue=NULL;window_lock();window_event_schedule_wake_locked(w);window_unlock();}
 if(close_during_wake==w){
  close_during_wake=NULL;window_lock();g_window_server.scene.count=0;window_unlock();
  assert(atomic_fetch_sub(&w->refs,1)>1); // Remove the owner reference, not the batch reference.
 }
 return 1;
}
static void object_notify_signaled(window_server_window_t *w){
 assert(!locked && atomic_load(&w->refs)>0);atomic_fetch_add(&w->notifications,1);
}
''' + production + r'''
static void init(window_server_window_t *w){
 *w=(window_server_window_t){0};w->event_waitq.owner=w;
 atomic_init(&w->refs,1);atomic_init(&w->wakes,0);atomic_init(&w->notifications,0);
 w->event_count=1;
}
static void reset(void){
 assert(!g_window_server.event_ready_count);
 window_lock();window_event_reset_ready_locked();g_window_server.scene.count=0;window_unlock();
}
static void schedule(window_server_window_t *w){window_lock();window_event_schedule_wake_locked(w);window_unlock();}
static void check(window_server_window_t *w,unsigned count){
 assert(atomic_load(&w->wakes)==count && atomic_load(&w->notifications)==count);
 assert(atomic_load(&w->refs)==1 && !w->event_wake_pending);
}
static atomic_bool producer_done;
static window_server_window_t concurrent[64];
static void *producer(void *unused){
 (void)unused;
 for(unsigned i=0;i<100000;++i)schedule(&concurrent[(i*17U)%64]);
 atomic_store(&producer_done,true);return NULL;
}
int main(void){
 reset();assert(!window_flush_event_wakes());schedule(NULL);assert(!window_flush_event_wakes());
 window_server_window_t normal;init(&normal);
 for(unsigned i=0;i<1000;++i)schedule(&normal);
 assert(atomic_load(&normal.refs)==2 && g_window_server.event_ready_count==1);
 unsigned accesses_before=ready_accesses;
 assert(window_flush_event_wakes());check(&normal,1);assert(!window_flush_event_wakes());
 // One ready-array write while locked, one publication read after unlock.
 // A second unconditional membership-clearing pass would add two accesses.
 assert(ready_accesses-accesses_before==2);
 // Overflow overlaps the detached ready set: each window must wake exactly once.
 window_server_window_t all[65];
 reset();for(unsigned i=0;i<65;++i)init(&all[i]);
 for(unsigned i=0;i<64;++i){g_window_server.scene.children[i]=&all[i];schedule(&all[i]);}
 g_window_server.scene.count=64;schedule(&all[64]);assert(g_window_server.event_ready_overflow);
 assert(window_flush_event_wakes());for(unsigned i=0;i<64;++i)check(&all[i],1);
 check(&all[64],0); // Not registered, so the fallback must not touch it.
 // Removed ready windows retain their references; all live overflow windows are notified.
 reset();window_server_window_t removed[64],active[64];
 for(unsigned i=0;i<64;++i){init(&removed[i]);init(&active[i]);schedule(&removed[i]);g_window_server.scene.children[i]=&active[i];}
 g_window_server.scene.count=64;
 for(unsigned i=0;i<64;++i)schedule(&active[i]);
 assert(window_flush_event_wakes());
 for(unsigned i=0;i<64;++i){check(&removed[i],1);check(&active[i],1);}
 // Empty registry entries and empty queues don't create overflow references.
 reset();for(unsigned i=0;i<64;++i)schedule(&removed[i]);
 active[0].event_count=0;g_window_server.scene.count=2;g_window_server.scene.children[0]=NULL;g_window_server.scene.children[1]=&active[0];
 schedule(&active[0]);assert(window_flush_event_wakes());check(&active[0],1);
 // Publication occurs outside the lock, after membership is cleared.
 reset();init(&normal);schedule(&normal);requeue=&normal;
 assert(window_flush_event_wakes());
 assert(g_window_server.event_ready_count==1 && normal.event_wake_pending && atomic_load(&normal.refs)==2);
 assert(window_flush_event_wakes());check(&normal,2);
 // Teardown before flushing also leaves the queued reference alive.
 reset();init(&normal);schedule(&normal);
 assert(atomic_fetch_sub(&normal.refs,1)==2);
 assert(window_flush_event_wakes());
 assert(!atomic_load(&normal.refs) && atomic_load(&normal.wakes)==1 && atomic_load(&normal.notifications)==1);
 // Concurrent teardown during wake leaves the batch reference valid for object notification.
 reset();init(&normal);g_window_server.scene.children[0]=&normal;g_window_server.scene.count=1;
 schedule(&normal);close_during_wake=&normal;assert(window_flush_event_wakes());
 assert(!atomic_load(&normal.refs) && atomic_load(&normal.notifications)==1);
 // Real producer/consumer interleavings exercise pending-bit and reference ownership.
 reset();for(unsigned i=0;i<64;++i){init(&concurrent[i]);g_window_server.scene.children[i]=&concurrent[i];}
 g_window_server.scene.count=64;atomic_init(&producer_done,false);
 pthread_t thread;assert(!pthread_create(&thread,NULL,producer,NULL));
 while(!atomic_load(&producer_done))window_flush_event_wakes();
 assert(!pthread_join(thread,NULL));while(window_flush_event_wakes()){}
 for(unsigned i=0;i<64;++i){
  assert(atomic_load(&concurrent[i].refs)==1 && !concurrent[i].event_wake_pending);
  assert(atomic_load(&concurrent[i].wakes)==atomic_load(&concurrent[i].notifications));
 }
 puts("Window wake batches: single-pass normal detach, deduplicated overflow, teardown, notification, requeue and 100000 concurrent schedules passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-window-wakes-') as directory:
    path = Path(directory)
    def run(name, code, fail=False):
        (path/(name+'.c')).write_text(code)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path/(name+'.c')), '-o', str(path/name)], check=True)
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE,(0,0))
        result = subprocess.run([str(path/name)], text=True, capture_output=True, preexec_fn=no_core)
        if fail:
            assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
            print('Negative control rejected: '+name)
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
    run('production', program)
    if args.negative_controls:
        run('duplicate-overflow-wake', program.replace('window == 0 || window->event_wake_pending ||',
                                                     'window == 0 ||'), True)
        run('missing-overflow-notification', program.replace('window_event_publish_wake(overflow_ready[index]);',
             '{ (void)wake_one(&overflow_ready[index]->event_waitq); object_put(overflow_ready[index]); }'), True)
        run('early-overflow-membership-clear', program.replace('!overflow && window != 0',
                                                             'window != 0'), True)
