"""Production FIFO-prefix extraction and nonrecursive GOP motion service."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
core = (root / 'kernel/drivers/input/core.c').read_text()
pump = (root / 'kernel/graphics/input_pump.c').read_text()
start = pump.index('void window_server_pump_publication_motion(')
body = pump[start:pump.index('\nvoid window_server_pump_input(void)', start)]
assert 'desktop_launch_program(' not in pump
program = r'''
#include <assert.h>
#include <string.h>
#include <kernel/input.h>
static unsigned depth,lock_depth,wakes,overlays,routed;
void sched_preempt_disable(void) {++depth;}
void sched_preempt_enable(void) {assert(depth);--depth;}
void wait_queue_init(wait_queue_t *q) {memset(q,0,sizeof(*q));}
uint32_t wake_one(wait_queue_t *q) {(void)q;return 0;}
uint32_t wake_all(wait_queue_t *q) {(void)q;assert(!depth && !lock_depth);++wakes;return 0;}
kstatus_t wait_on_queue(wait_queue_t *q,bool (*ready)(void*),void *c,uint64_t n) {
 (void)q;(void)ready;(void)c;(void)n;return K_ETIMEDOUT;
}
''' + core + r'''
static struct {unsigned dragging_identifier;wait_queue_t event_waitq;} g_window_server;
static struct {unsigned dragging_identifier;} g_compositor_snapshot;
static void window_lock(void) {assert(!depth && !lock_depth);++lock_depth;}
static void window_unlock(void) {assert(lock_depth==1);--lock_depth;}
static void route_input_locked(const input_event_t *e) {
 assert(lock_depth==1 && e->type==INPUT_EVENT_RELATIVE);++routed;
}
static void window_route_pointer_transaction_locked(const input_event_t *e) {
 assert(lock_depth==1 && e->type==INPUT_EVENT_POINTER && !e->code && !e->value4);++routed;
}
static bool window_flush_event_wakes(void) {assert(!lock_depth);return true;}
static void compositor_present_cursor_direct(bool force) {
 assert(!depth && !lock_depth && !force);++overlays;
}
''' + body + r'''
static input_event_t motion(unsigned n) {
 return (input_event_t){.timestamp=n+1,.type=INPUT_EVENT_RELATIVE,.code=INPUT_REL_X,.value=1};
}
int main(void) {
 input_event_t out[16];
 assert(!input_core_pop_cursor_motion(out,16));
 for(unsigned head=0;head<INPUT_CORE_CAPACITY;++head)
 for(unsigned barrier=0;barrier<33;++barrier) {
  input_core_init();g_input.read=g_input.write=head;
  for(unsigned n=0;n<33;++n) {
   input_event_t e=motion(n);
   if(n==barrier)e.type=INPUT_EVENT_KEY;
   assert(input_core_push(&e)==K_OK);
  }
  size_t total=0,count;
  while((count=input_core_pop_cursor_motion(out,16))!=0) {
   for(size_t n=0;n<count;++n)assert(out[n].timestamp==total+n+1);
   total+=count;
  }
  assert(total==barrier && input_core_pending()==33-barrier);
  input_event_t e;assert(input_core_pop(&e)==K_OK && e.type==INPUT_EVENT_KEY);
 }
 const input_event_t barriers[]={
  {.type=INPUT_EVENT_BUTTON},{.type=INPUT_EVENT_ABSOLUTE},
  {.type=INPUT_EVENT_RELATIVE,.code=INPUT_REL_WHEEL},
  {.type=INPUT_EVENT_POINTER,.code=1},
  {.type=INPUT_EVENT_POINTER,.value4=1}};
 for(unsigned i=0;i<sizeof(barriers)/sizeof(barriers[0]);++i) {
  input_core_init();input_core_push(&barriers[i]);
  input_event_t e=motion(1);input_core_push(&e);
  window_server_pump_publication_motion();assert(input_core_pending()==2);
 }
 input_core_init();
 for(unsigned n=0;n<32;++n){input_event_t e=motion(n);input_core_push(&e);}
 g_window_server.dragging_identifier=1;window_server_pump_publication_motion();
 assert(input_core_pending()==32 && !routed);
 g_window_server.dragging_identifier=0;g_compositor_snapshot.dragging_identifier=1;
 window_server_pump_publication_motion();assert(input_core_pending()==32 && !routed);
 g_compositor_snapshot.dragging_identifier=0;
 window_server_pump_publication_motion();assert(input_core_pending()==16 && routed==16);
 window_server_pump_publication_motion();assert(!input_core_pending() && routed==32);
 assert(wakes==2 && overlays==2 && !depth && !lock_depth);
}
'''
with tempfile.TemporaryDirectory(prefix='publication-motion-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-I',str(root / 'include'),
                    str(path / 'test.c'),'-o',str(path / 'test')],check=True)
    subprocess.run([str(path / 'test')],check=True)
print('Publication motion: 8448 wrapped FIFO barriers, 16-event budget, drag exclusion, unlocked cursor/wakes and no synchronous launch PASS')

# Compile the actual post-routing launch dispatch, including queue failure.
start = pump.index('    for (;;) {', pump.index('void window_server_pump_input_mode('))
end = pump.index('\n    if (!compose_now) return;', start)
dispatch = pump[start:end]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define DESKTOP_APP_NONE 0U
static unsigned pending,queued,restored,cycles,held;
static bool available,minimized;
static void window_lock(void) {assert(!held);held=1;}
static void window_unlock(void) {assert(held);held=0;}
static uint32_t window_input_take_desktop_launch_locked(void) {
 unsigned p=pending;pending=0;return p;
}
static bool window_input_take_focus_cycle_request_locked(void) {return false;}
static void desktop_cycle_window_focus(void) {++cycles;}
static bool desktop_launch_enqueue(unsigned app) {
 assert(!held && app);if(available)++queued;return available;
}
static bool desktop_restore_minimized_app(unsigned app) {
 assert(!held && app);if(minimized)++restored;return minimized;
}
static void dispatch(void) {
''' + dispatch + r'''
}
int main(void) {
 /* Worker absent/full: a busy click never loads an ELF synchronously. */
 for(unsigned i=0;i<1000;++i){pending=1;dispatch();}
 assert(!queued && !restored && !pending && !held);
 minimized=true;pending=1;dispatch();assert(restored==1);
 available=true;pending=1;dispatch();assert(queued==1 && restored==1 && !cycles);
}
'''
with tempfile.TemporaryDirectory(prefix='launch-nonblocking-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    str(path / 'test.c'),'-o',str(path / 'test')],check=True)
    subprocess.run([str(path / 'test')],check=True)
print('Actual launch dispatch: 1000 busy/unavailable requests, minimized restore and successful enqueue PASS')
