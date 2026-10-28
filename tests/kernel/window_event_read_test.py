"""Production window readiness and blocking reads across close/lookup races."""
from pathlib import Path
import argparse
import resource
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-controls', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/window.c').read_text()
signaled = source[source.index('static bool window_is_signaled('):source.index('static uint64_t window_tsc_to_us(')]
read = source[source.index('typedef struct window_event_wait_context'):source.index('static kstatus_t window_server_update_internal(', source.index('typedef struct window_event_wait_context'))]
assert 'g_window_server' not in signaled
assert 'atomic_init(&window->registry_removed, false)' in source
assert 'atomic_store_explicit(&window->registry_removed, true, memory_order_release)' in source
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include "uapi/window.h"
#define WINDOW_EVENT_CAPACITY 64U
enum {K_OK=0,K_EINVAL=-22,K_EAGAIN=-11,K_EPERM=-1,K_ENOENT=-2,K_ETIMEDOUT=-110,K_ECANCELED=-125};
typedef int kstatus_t;
typedef struct {unsigned id;} process_t;
typedef struct {unsigned id;} wait_queue_t;
typedef struct {
 process_t *owner;uint32_t identifier,event_count,event_read;
 os_window_event_t events[64];atomic_bool registry_removed;wait_queue_t event_waitq;
 unsigned refs;
} window_server_window_t;
static struct {struct {uint32_t count;window_server_window_t *children[64];} scene;wait_queue_t event_waitq;} g_window_server;
static bool initialized=true,lookup_closes=false;
static unsigned locked,waits,gets,released_refs,mode;
static window_server_window_t *target;
static void window_lock(void){assert(!locked);locked=1;}
static void window_unlock(void){assert(locked);locked=0;}
static bool window_server_init(void){return initialized;}
static kstatus_t window_server_lookup(uint32_t id,window_server_window_t **out){
 assert(!locked);
 if(!target || target->identifier!=id)return K_ENOENT;
 ++gets;++target->refs;*out=target;
 if(lookup_closes){atomic_store(&target->registry_removed,true);g_window_server.scene.count=0;}
 return K_OK;
}
static void window_server_put(window_server_window_t *w){assert(!locked && w->refs>1);--w->refs;++released_refs;}
static kstatus_t wait_on_queue(wait_queue_t *queue,bool (*predicate)(void*),void *context,uint64_t timeout){
 assert(!locked && timeout);++waits;
 if(mode==4){
  assert(queue==&g_window_server.event_waitq);
  g_window_server.scene.children[0]->event_count=1;
  assert(predicate(context));return K_OK;
 }
 assert(target && queue==&target->event_waitq && target->refs==2);
 if(mode==3)return K_ECANCELED;
 if(atomic_load(&target->registry_removed)){assert(predicate(context));return K_OK;}
 // Empty/spurious checks do not need the registry once a reference is held.
 g_window_server.scene.count=0;
 for(unsigned i=0;i<100;++i)assert(!predicate(context));
 if(mode==0)return K_ETIMEDOUT;
 if(mode==1){target->event_count=1;assert(predicate(context));return K_OK;}
 assert(mode==2);target->event_count=1; // Close must not accidentally consume this stale event.
 atomic_store(&target->registry_removed,true);
 assert(predicate(context));return K_OK;
}
''' + signaled + read + r'''
static void setup(window_server_window_t *w,process_t *owner){
 *w=(window_server_window_t){.owner=owner,.identifier=7,.refs=1};
 atomic_init(&w->registry_removed,false);
 w->events[0]=(os_window_event_t){.identifier=7,.type=OS_WINDOW_EVENT_INPUT,.input.timestamp=123};
 w->events[63]=(os_window_event_t){.identifier=7,.type=OS_WINDOW_EVENT_INPUT,.input.timestamp=456};
 target=w;g_window_server.scene.children[0]=w;g_window_server.scene.count=1;
 lookup_closes=false;mode=waits=gets=released_refs=0;
}
int main(void){
 process_t owner={1},foreign={2};window_server_window_t w;os_window_event_t out,unchanged;
 setup(&w,&owner);
 assert(!window_is_signaled(NULL));assert(!window_is_signaled(&w));
 w.event_count=1;assert(window_is_signaled(&w));w.event_count=0;
 atomic_store(&w.registry_removed,true);assert(window_is_signaled(&w));
 // Readiness uses the removal marker even with an unrelated/absent registry.
 g_window_server.scene.count=64;memset(g_window_server.scene.children,0,sizeof(g_window_server.scene.children));
 for(unsigned i=0;i<100000;++i)assert(window_is_signaled(&w));
 initialized=false;assert(!window_is_signaled(&w));initialized=true;
 setup(&w,&owner);memset(&out,0xa5,sizeof(out));unchanged=out;
 assert(window_server_event_read(NULL,7,&out,1)==K_EINVAL);
 assert(window_server_event_read(&owner,7,NULL,1)==K_EINVAL);
 assert(window_server_event_read(&owner,7,&out,0)==K_EAGAIN && !waits && !gets);
 assert(window_server_event_read(&foreign,7,&out,0)==K_EAGAIN);
 assert(window_server_event_read(&foreign,7,&out,1)==K_EPERM && gets==released_refs && w.refs==1);
 assert(window_server_event_read(&owner,999,&out,1)==K_EPERM);
 for(unsigned m=0;m<4;++m){
  setup(&w,&owner);out=unchanged;mode=m;
  int expected=m==0?K_ETIMEDOUT:m==1?K_OK:K_ECANCELED;
  assert(window_server_event_read(&owner,7,&out,1000)==expected);
  assert(w.refs==1 && gets==1 && released_refs==1 && waits==1);
  if(m==1)assert(out.input.timestamp==123 && w.event_count==0);
  else assert(!memcmp(&out,&unchanged,sizeof(out)));
  if(m==2)assert(w.event_count==1);
 }
 setup(&w,&owner);lookup_closes=true;out=unchanged;
 assert(window_server_event_read(&owner,7,&out,1000)==K_ECANCELED && gets==released_refs);
 assert(!memcmp(&out,&unchanged,sizeof(out)));
 // Immediate reads retain existing FIFO, ownership filtering and wrap behavior.
 setup(&w,&owner);w.event_read=63;w.event_count=2;
 assert(window_server_event_read(&owner,7,&out,0)==K_OK && out.input.timestamp==456);
 assert(window_server_event_read(&owner,7,&out,0)==K_OK && out.input.timestamp==123);
 assert(!waits && !gets && w.event_read==1 && !w.event_count);
 // Identifier zero still waits across all windows of the calling process.
 setup(&w,&owner);mode=4;
 assert(window_server_event_read(&owner,0,&out,1000)==K_OK && !gets && !released_refs && waits==1);
 puts("Window reads: constant-time readiness, 100000 HUP checks, spurious wakes, close races, ownership, timeout, FIFO wrap and any-window reads passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-window-read-') as directory:
    path = Path(directory)
    def run(name, code, fail=False):
        (path/(name+'.c')).write_text(code)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I'+str(root/'include'), str(path/(name+'.c')), '-o', str(path/name)], check=True)
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
        run('missing-close-cancellation', program.replace('available = context->cancelled ||', 'available = false ||'), True)
        run('missing-cancel-result', program.replace('if (status == K_OK && context.cancelled) return K_ECANCELED;', ''), True)
