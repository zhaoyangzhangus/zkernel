"""Exercise the production atomic fullscreen transition and exact restoration."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/window.c').read_text()
begin = source.index('kstatus_t window_server_set_fullscreen_owned(')
# Extract this top-level function, not every function inserted before the next
# historical neighbour. Adjacent drag APIs have their own state and fixtures.
body = source[begin:source.index('\n}', begin) + 2]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <uapi/window.h>
typedef int process_t,kstatus_t;
enum {K_OK=0,K_EINVAL=-22,K_EPERM=-1};
typedef struct {
 process_t *owner;unsigned identifier,flags,width,height;int32_t x,y;
 uint64_t buffer_size,surface_span;bool surface_exchange_enabled,maximized,fullscreen,dirty,resize_pending;
 int32_t fullscreen_restore_x,fullscreen_restore_y;
 unsigned fullscreen_restore_width,fullscreen_restore_height,fullscreen_restore_flags;
 int32_t restore_x,restore_y;unsigned restore_width,restore_height;
} window_server_window_t;
static struct {unsigned display_width,display_height,title_pressed_identifier,dragging_identifier;} g_window_server={1920,1080,0,0};
static bool ready=true,registered=true,locked;
static unsigned marks,events,wakes,rehits;
static void window_input_refresh_pointer_locked(void){assert(locked);++rehits;}
static bool window_server_kernel_ready(void){return ready;}
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static bool window_server_owner_locked(process_t *p,window_server_window_t *w){assert(locked);return registered&&p==w->owner;}
static void window_mark_window_locked(window_server_window_t *w){assert(locked&&w);++marks;}
static void window_enqueue_resize_event_locked(window_server_window_t *w){assert(locked&&w->resize_pending);++events;}
static void window_clear_title_capture_locked(void){assert(locked);g_window_server.title_pressed_identifier=0;}
static void window_input_clear_drag_locked(void){assert(locked);g_window_server.dragging_identifier=0;}
static void window_server_notify_worker(void){assert(!locked);++wakes;}
''' + body + r'''
int main(void){
 process_t owner,other;unsigned cases=0;
 for(unsigned client=0;client<2;client++)for(unsigned max=0;max<2;max++)
 for(unsigned caption=40;caption<=128;caption++){
  unsigned flags=OS_WINDOW_VISIBLE|OS_WINDOW_RESIZABLE|OS_WINDOW_PREMULTIPLIED_ALPHA|
   OS_WINDOW_CAPTION_HEIGHT(caption)|(client?OS_WINDOW_CLIENT_DECORATIONS:OS_WINDOW_STANDARD_CAPTION_CONTROLS);
  window_server_window_t w={.owner=&owner,.identifier=7,.flags=flags,.width=640,.height=360,
   .x=123,.y=234,.buffer_size=32*1024*1024,.maximized=max,.restore_x=12,.restore_y=34,.restore_width=320,.restore_height=240};
  for(unsigned i=0;i<20;i++){
   unsigned n=wakes;g_window_server.title_pressed_identifier=g_window_server.dragging_identifier=7;
   assert(!window_server_set_fullscreen_owned(&owner,&w,1));
   assert(w.fullscreen&&!w.x&&!w.y&&w.width==1920&&w.height==1080&&w.maximized==max);
   assert((w.flags&OS_WINDOW_CLIENT_DECORATIONS)&&!(w.flags&OS_WINDOW_STANDARD_CAPTION_CONTROLS));
   assert(!g_window_server.title_pressed_identifier&&!g_window_server.dragging_identifier);
   assert(!window_server_set_fullscreen_owned(&owner,&w,1)&&wakes==n+1);
   assert(!window_server_set_fullscreen_owned(&owner,&w,0));
   assert(!w.fullscreen&&w.x==123&&w.y==234&&w.width==640&&w.height==360&&w.flags==flags&&w.maximized==max);
   assert(w.restore_x==12&&w.restore_y==34&&w.restore_width==320&&w.restore_height==240);
   assert(!window_server_set_fullscreen_owned(&owner,&w,0)&&wakes==n+2);++cases;
  }
  w.buffer_size=4096;window_server_window_t saved=w;unsigned n=wakes;
  assert(window_server_set_fullscreen_owned(&owner,&w,1)==-22&&!memcmp(&saved,&w,sizeof(w))&&wakes==n);
  assert(window_server_set_fullscreen_owned(&other,&w,1)==-1&&!locked);
  registered=false;assert(window_server_set_fullscreen_owned(&owner,&w,1)==-1);registered=true;
  ready=false;assert(window_server_set_fullscreen_owned(&owner,&w,1)==-22);ready=true;
  assert(window_server_set_fullscreen_owned(&owner,&w,2)==-22);
 }
 assert(marks==cases*4&&events==cases*2&&wakes==cases*2&&rehits==events&&!locked);
 assert(window_server_set_fullscreen_owned(0,0,1)==-22);
 window_server_window_t hidden={.owner=&owner,.identifier=7,.flags=OS_WINDOW_VISIBLE|OS_WINDOW_STANDARD_CAPTION_CONTROLS,
  .width=640,.height=360,.buffer_size=32*1024*1024};
 assert(!window_server_set_fullscreen_owned(&owner,&hidden,1));hidden.flags&=~OS_WINDOW_VISIBLE;
 assert(!window_server_set_fullscreen_owned(&owner,&hidden,0)&&!(hidden.flags&OS_WINDOW_VISIBLE));
 // The allocation covers two frames, but neither slot can hold a full screen.
 hidden.surface_exchange_enabled=true;
 hidden.surface_span=1920ULL*1080*4/2;
 hidden.buffer_size=hidden.surface_span*2;
 window_server_window_t saved=hidden;unsigned n=wakes;
 assert(window_server_set_fullscreen_owned(&owner,&hidden,1)==K_EINVAL);
 assert(!memcmp(&saved,&hidden,sizeof(hidden))&&wakes==n);
 hidden.surface_span=1920ULL*1080*4;hidden.buffer_size=hidden.surface_span*2;
 assert(!window_server_set_fullscreen_owned(&owner,&hidden,1));
 assert(!window_server_set_fullscreen_owned(&owner,&hidden,0));
}
'''
with tempfile.TemporaryDirectory(prefix='window-fullscreen-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-I'+str(root/'include'),str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Fullscreen: 7120 atomic round trips, both frame styles, maximized restore preservation, owner isolation and capacity failure rollback passed')
