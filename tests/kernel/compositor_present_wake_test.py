"""Execute actual snapshot finish: fence-specific wake, lock order, lifetime."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/graphics/compositor.c').read_text()
start=source.index('void compositor_snapshot_finish(void)')
opening=source.index('{',start)
depth=1;end=opening+1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
finish=source[start:end]
release_start = source.index('static void compositor_release_window_snapshot(')
release = source[release_start:source.index('\n}', release_start) + 2]
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#define WINDOW_SERVER_MAX_WINDOWS 64
#define K_OK 0
static int g_compositor_publication_status;
static unsigned dirty_restored;
typedef struct window {
 int32_t compositor_presented_x,compositor_presented_y;
 uint32_t compositor_presented_width,compositor_presented_height;
 bool compositor_presented_valid;
 atomic_uint_fast64_t compositor_presented_sequence;
 unsigned event_waitq,puts;
 uint64_t expected;
 uint64_t present_sequence;
} window_server_window_t;
typedef struct {
 window_server_window_t *reference;
 void *section;
 unsigned *controls;
 void *caption;
 int32_t x,y;
 uint32_t width,height,flags;
 uint64_t present_sequence;
} compositor_window_view_t;
static struct {
 compositor_window_view_t windows[WINDOW_SERVER_MAX_WINDOWS];
 uint32_t window_count,damage_count;
} g_compositor_snapshot;
static struct {bool composing;} g_window_server;
static bool locked,reset;
static unsigned wakes,puts;
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static void window_animation_presented_locked(const compositor_window_view_t *view){assert(locked&&view->reference);}
static void window_mark_dirty_locked(void){assert(locked);++dirty_restored;}
static void window_surface_snapshot_release(compositor_window_view_t *view){assert(!locked&&view->reference);}
static uint32_t window_outer_width(uint32_t n,uint32_t f){(void)f;return n;}
static uint32_t window_outer_height(uint32_t n,uint32_t f){(void)f;return n;}
static void window_input_set_drag_blit_valid_locked(bool b){assert(locked&&!b);}
static int wake_all(unsigned *queue){
 assert(!locked&&!g_window_server.composing);
 window_server_window_t *found=0;
 for(unsigned i=0;i<g_compositor_snapshot.window_count;i++){
  window_server_window_t *w=g_compositor_snapshot.windows[i].reference;
  if(w && &w->event_waitq==queue){found=w;break;}
 }
 assert(found&&found->puts==0);
 /* 首个唤醒之前，后续窗口的 ACK 也必须已在锁内统一发布。 */
 if(g_compositor_publication_status==K_OK)
  for(unsigned i=0;i<g_compositor_snapshot.window_count;i++){
   window_server_window_t *w=g_compositor_snapshot.windows[i].reference;
   if(w)assert(atomic_load_explicit(&w->compositor_presented_sequence,memory_order_acquire)==w->expected);
  }
 assert(atomic_load_explicit(&found->compositor_presented_sequence,memory_order_acquire)==found->expected);
 ++*queue;++wakes;return 0;
}
static void object_put(window_server_window_t *w){assert(!locked);++w->puts;++puts;}
static void window_control_document_put(unsigned *controls){assert(!locked);if(controls){assert(*controls);--*controls;}}
static void caption_text_put(void *caption){assert(!locked&&!caption);}
static void compositor_render_plan_reset(void){reset=true;}
''' + release + '\n' + finish + r'''
int main(void){
 window_server_window_t windows[WINDOW_SERVER_MAX_WINDOWS]={0};
 unsigned documents[WINDOW_SERVER_MAX_WINDOWS];
 g_compositor_snapshot.window_count=WINDOW_SERVER_MAX_WINDOWS;
 g_compositor_snapshot.damage_count=3;g_window_server.composing=true;
 for(unsigned i=0;i<WINDOW_SERVER_MAX_WINDOWS;i++){
  documents[i]=1;
  atomic_init(&windows[i].compositor_presented_sequence,10);
  windows[i].expected=(i%4==0||i==63)?11:10;
  g_compositor_snapshot.windows[i]=(compositor_window_view_t){
   .reference=&windows[i],.section=&windows[i],.controls=&documents[i],.x=12,.y=20,
   .width=640,.height=480,.present_sequence=windows[i].expected};
 }
 /* A sparse snapshot reference must never be woken or released. */
 g_compositor_snapshot.windows[4].reference=0;
 g_compositor_snapshot.windows[4].controls=0;documents[4]=0;
 compositor_snapshot_finish();
 assert(wakes==16&&puts==63&&reset&&!locked);
 for(unsigned i=0;i<WINDOW_SERVER_MAX_WINDOWS;i++){
  assert(windows[i].event_waitq==(i!=4&&(i%4==0||i==63)));
  assert(g_compositor_snapshot.windows[i].reference==0);
  assert(g_compositor_snapshot.windows[i].section==0);
  assert(!g_compositor_snapshot.windows[i].controls&&!documents[i]);
  if(i!=4)assert(windows[i].compositor_presented_valid);
 }
 assert(!g_compositor_snapshot.window_count&&!g_compositor_snapshot.damage_count);
 /* Cursor-only/unchanged frame has geometry bookkeeping but zero wakes. */
 wakes=puts=0;windows[0].puts=0;reset=false;
 g_compositor_snapshot.window_count=1;
 g_compositor_snapshot.windows[0].reference=&windows[0];
 g_window_server.composing=true;
 compositor_snapshot_finish();
 assert(wakes==0&&puts==1&&reset);
 /* Comparison keeps existing uint64 sequence publication semantics. */
 windows[0].puts=0;windows[0].expected=0;
 atomic_store(&windows[0].compositor_presented_sequence,UINT64_MAX);
 g_compositor_snapshot.windows[0].reference=&windows[0];
 g_compositor_snapshot.windows[0].present_sequence=0;
 g_compositor_snapshot.window_count=1;g_window_server.composing=true;
 compositor_snapshot_finish();assert(wakes==1);
 /* A newer submitted frame is not complete merely because an older
  * snapshot reached scanout. Publish only the sequence actually composed. */
 wakes=puts=0;windows[0].puts=0;windows[0].expected=41;
 windows[0].present_sequence=42;
 atomic_store(&windows[0].compositor_presented_sequence,40);
 g_compositor_snapshot.windows[0].reference=&windows[0];
 g_compositor_snapshot.windows[0].present_sequence=41;
 g_compositor_snapshot.window_count=1;g_window_server.composing=true;
 compositor_snapshot_finish();
 assert(wakes==1&&atomic_load(&windows[0].compositor_presented_sequence)==41);
 assert(atomic_load(&windows[0].compositor_presented_sequence)<windows[0].present_sequence);
 windows[0].puts=0;windows[0].expected=42;
 g_compositor_snapshot.windows[0].reference=&windows[0];
 g_compositor_snapshot.windows[0].present_sequence=42;
 g_compositor_snapshot.window_count=1;g_window_server.composing=true;
 compositor_snapshot_finish();
 assert(wakes==2&&atomic_load(&windows[0].compositor_presented_sequence)==42);
 /* Busy, error and timeout publications preserve old geometry and ACK,
  * restore damage, and still release every snapshot lifetime reference. */
 const int failures[]={-16,-5,-110};
 for(unsigned i=0;i<3;i++){
  windows[0].puts=0;
  g_compositor_publication_status=failures[i];
  g_compositor_snapshot.windows[0].reference=&windows[0];
  g_compositor_snapshot.windows[0].present_sequence=43;
  g_compositor_snapshot.windows[0].x=999;
  g_compositor_snapshot.window_count=1;g_window_server.composing=true;
  compositor_snapshot_finish();
  assert(wakes==2&&atomic_load(&windows[0].compositor_presented_sequence)==42);
  assert(windows[0].compositor_presented_x==12&&windows[0].puts==1);
  assert(dirty_restored==i+1&&!g_window_server.composing);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-compositor-wake-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Actual compositor finish: unchanged fences skipped, changed fences published before wake, unlock before queue lock, references held, sparse/wrap cases passed')
