"""Production resize: small client popups and native decoration constraints."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/window.c').read_text()
start = source.index('kstatus_t window_server_resize(')
end = source.index('\nkstatus_t window_server_close_owned(', start)
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "uapi/window.h"
typedef int kstatus_t;
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define WINDOW_MIN_WIDTH 160U
#define WINDOW_MIN_HEIGHT 96U
typedef struct {int id;} process_t;
typedef struct {
 process_t *owner; uint32_t flags,width,height; int32_t x,y;
 bool dirty,resize_pending,fullscreen;
} window_server_window_t;
static struct {uint32_t display_width,display_height;} g_window_server={1920,1080};
static bool ready=true;
static int locked,events,notifies,marks;
static bool window_server_kernel_ready(void){return ready;}
static void window_lock(void){assert(!locked);locked=1;}
static void window_unlock(void){assert(locked);locked=0;}
static bool window_server_owner_locked(process_t *p,window_server_window_t *w){assert(locked);return p==w->owner;}
static uint32_t window_outer_width(uint32_t w,uint32_t f){return w+((f&OS_WINDOW_CLIENT_DECORATIONS)?0:2);}
static uint32_t window_outer_height(uint32_t h,uint32_t f){return h+((f&OS_WINDOW_CLIENT_DECORATIONS)?0:24);}
static void window_mark_rect_locked(int32_t x,int32_t y,uint32_t w,uint32_t h){assert(locked);(void)x;(void)y;(void)w;(void)h;++marks;}
static void window_mark_window_locked(window_server_window_t *w){assert(locked&&w);++marks;}
static void window_enqueue_resize_event_locked(window_server_window_t *w){assert(locked&&w);++events;}
static void window_server_notify_worker(void){assert(!locked);++notifies;}
''' + source[start:end] + r'''
int main(void){
 process_t owner={1},other={2};
 window_server_window_t popup={.owner=&owner,.flags=OS_WINDOW_RESIZABLE|OS_WINDOW_CLIENT_DECORATIONS,.width=448,.height=100,.x=10,.y=10};
 // 在规范客户机中捕获到的 Chrome 实际失败请求：448x50。
 assert(window_server_resize(&owner,&popup,448,50)==K_OK);
 assert(popup.width==448&&popup.height==50&&popup.dirty&&popup.resize_pending);
 popup.fullscreen=true;
 assert(window_server_resize(&owner,&popup,449,50)==K_EINVAL&&popup.width==448&&popup.height==50);
 assert(window_server_resize(&owner,&popup,448,50)==K_OK);
 popup.fullscreen=false;
 assert(events==1&&notifies==1&&marks==2);
 assert(window_server_resize(&owner,&popup,448,50)==K_OK&&events==1);
 assert(window_server_resize(&owner,&popup,1,1)==K_OK);
 assert(window_server_resize(&other,&popup,448,50)==K_EPERM);
 assert(window_server_resize(&owner,&popup,0,50)==K_EINVAL);
 assert(window_server_resize(&owner,&popup,448,0)==K_EINVAL);
 assert(window_server_resize(&owner,&popup,1920,50)==K_EINVAL);
 assert(window_server_resize(&owner,&popup,448,1080)==K_EINVAL);
 assert(window_server_resize(&owner,&popup,UINT32_MAX,50)==K_EINVAL);
 popup.flags=OS_WINDOW_CLIENT_DECORATIONS;
 assert(window_server_resize(&owner,&popup,448,50)==K_EINVAL);
 popup.flags=OS_WINDOW_RESIZABLE;
 assert(window_server_resize(&owner,&popup,448,50)==K_EINVAL);
 assert(window_server_resize(&owner,&popup,159,96)==K_EINVAL);
 assert(window_server_resize(&owner,&popup,160,96)==K_OK);
 assert(window_server_resize(&owner,&popup,1910,96)==K_EINVAL);
 ready=false;assert(window_server_resize(&owner,&popup,160,96)==K_EINVAL);
 assert(window_server_resize(0,&popup,160,96)==K_EINVAL);
 assert(window_server_resize(&owner,0,160,96)==K_EINVAL);
 assert(locked==0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-popup-resize-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root/'include'), str(path/'test.c'),
                    '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('Popup resize: actual 448x50, native minima, ownership, screen bounds and events passed')
