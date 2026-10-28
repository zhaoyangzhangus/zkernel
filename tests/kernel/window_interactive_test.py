"""Execute production interactive move authorization and Linux command decoding."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/window.c').read_text()
begin = source.index('kstatus_t window_server_begin_interactive_owned(')
body = source[begin:source.index('kstatus_t window_server_set_decorations_owned(',begin)]
linux = (root/'kernel/syscall/linux.c').read_text()
begin = linux.index('    case LINUX_WINDOW_OPERATION_INTERACTIVE:')
dispatch = linux[begin:linux.index('    case ',begin+10)]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <uapi/window.h>
#include <uapi/window_state.h>
#include <uapi/linux_abi.h>
enum {K_OK=0,K_EINVAL=-22,K_EPERM=-1,K_EBUSY=-16,
 WINDOW_RESIZE_LEFT=1,WINDOW_RESIZE_RIGHT=2,WINDOW_RESIZE_TOP=4,WINDOW_RESIZE_BOTTOM=8};
typedef int process_t,kstatus_t;
typedef struct Window {
 process_t *owner; unsigned identifier,flags; int x,y;
 bool client_button_capture,client_pointer_capture,client_hidden,minimized,maximized,fullscreen;
} window_server_window_t;
static struct {unsigned pointer_x,pointer_y,dragging_identifier;
 struct {unsigned count;window_server_window_t *children[2];} scene;} g_window_server;
static bool ready=true,registered=true,locked;
static unsigned clears,wakes,marks,edges_seen;
static int offset_x,offset_y;
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static bool window_server_kernel_ready(void){return ready;}
static bool window_server_owner_locked(process_t *p,window_server_window_t *w){assert(locked);return registered&&w->owner==p;}
static void window_clear_title_capture_locked(void){assert(locked);++clears;}
static void window_input_begin_drag_locked(unsigned id,int x,int y,unsigned edges){
 assert(locked);g_window_server.dragging_identifier=id;offset_x=x;offset_y=y;edges_seen=edges;
}
static void window_mark_moved_cursor_locked(unsigned x,unsigned y,unsigned a,unsigned b){assert(locked&&x==a&&y==b);++marks;}
static void window_server_notify_worker(void){assert(!locked);++wakes;}
''' + body + r'''
static int invoke(process_t *process,window_server_window_t *window,linux_window_command_t *command){
 switch(command->operation){
''' + dispatch + r'''
 default:return -22;
 }
}
int main(void){
 process_t owner,foreign;
 window_server_window_t w={.owner=&owner,.identifier=7,.x=50,.y=80,
  .flags=OS_WINDOW_VISIBLE|OS_WINDOW_RESIZABLE|OS_WINDOW_EXPLICIT_MOVE_RESIZE,
  .client_button_capture=true,.client_pointer_capture=true};
 window_server_window_t other={.owner=&foreign,.identifier=8};
 g_window_server.scene.count=2;g_window_server.scene.children[0]=&w;g_window_server.scene.children[1]=&other;
 g_window_server.pointer_x=61;g_window_server.pointer_y=99;
 for(unsigned edges=0;edges<32;++edges){
  bool valid=edges<16&&(edges&3)!=3&&(edges&12)!=12;
  window_server_window_t before=w;
  unsigned n=wakes;
  int result=window_server_begin_interactive_owned(&owner,&w,edges);
  if(valid){
   assert(result==0&&wakes==n+1&&marks==wakes&&clears==wakes);
   assert(!w.client_button_capture&&!w.client_pointer_capture);
   assert(g_window_server.dragging_identifier==7&&edges_seen==edges);
   assert(offset_x==(edges?0:11)&&offset_y==(edges?0:19));
   g_window_server.dragging_identifier=0;w=before;
  }else assert(result==K_EINVAL&&wakes==n&&!memcmp(&before,&w,sizeof(w)));
 }
 unsigned n=wakes;
 assert(window_server_begin_interactive_owned(&foreign,&w,0)==K_EPERM);
 registered=false;assert(window_server_begin_interactive_owned(&owner,&w,0)==K_EPERM);registered=true;
 ready=false;assert(window_server_begin_interactive_owned(&owner,&w,0)==K_EINVAL);ready=true;
 assert(window_server_begin_interactive_owned(0,&w,0)==K_EINVAL);
 assert(window_server_begin_interactive_owned(&owner,0,0)==K_EINVAL);
 assert(window_server_begin_interactive_owned(&owner,&w,UINT32_MAX)==K_EINVAL);
 bool *states[]={&w.client_hidden,&w.minimized,&w.maximized,&w.fullscreen};
 for(unsigned i=0;i<4;++i){*states[i]=true;assert(window_server_begin_interactive_owned(&owner,&w,0)==K_EINVAL);*states[i]=false;}
 w.client_button_capture=false;assert(window_server_begin_interactive_owned(&owner,&w,0)==K_EINVAL);w.client_button_capture=true;
 for(unsigned flag=OS_WINDOW_VISIBLE;flag<=OS_WINDOW_EXPLICIT_MOVE_RESIZE;flag<<=1){
  if(flag!=OS_WINDOW_VISIBLE&&flag!=OS_WINDOW_EXPLICIT_MOVE_RESIZE)continue;
  w.flags^=flag;assert(window_server_begin_interactive_owned(&owner,&w,0)==K_EINVAL);w.flags^=flag;
 }
 w.flags^=OS_WINDOW_RESIZABLE;assert(window_server_begin_interactive_owned(&owner,&w,1)==K_EINVAL);w.flags^=OS_WINDOW_RESIZABLE;
 g_window_server.dragging_identifier=8;assert(window_server_begin_interactive_owned(&owner,&w,0)==K_EBUSY);g_window_server.dragging_identifier=0;
 other.client_pointer_capture=true;assert(window_server_begin_interactive_owned(&owner,&w,0)==K_EBUSY);other.client_pointer_capture=false;
 other.client_button_capture=true;assert(window_server_begin_interactive_owned(&owner,&w,0)==K_EBUSY);other.client_button_capture=false;
 assert(wakes==n&&w.client_button_capture&&w.client_pointer_capture);
 linux_window_command_t c={.operation=LINUX_WINDOW_OPERATION_INTERACTIVE,.width=5};
 c.x=1;assert(invoke(&owner,&w,&c)==K_EINVAL);c.x=0;
 c.y=-1;assert(invoke(&owner,&w,&c)==K_EINVAL);c.y=0;
 c.height=1;assert(invoke(&owner,&w,&c)==K_EINVAL);c.height=0;
 c.stride=1;assert(invoke(&owner,&w,&c)==K_EINVAL);c.stride=0;
 c.address=1;assert(invoke(&owner,&w,&c)==K_EINVAL);c.address=0;
 c.length=1;assert(invoke(&owner,&w,&c)==K_EINVAL);c.length=0;
 assert(wakes==n);
 assert(invoke(&owner,&w,&c)==0&&edges_seen==5&&wakes==n+1);
 assert(invoke(&owner,&w,&c)==K_EINVAL); // One press cannot start two operations.
}
'''
with tempfile.TemporaryDirectory(prefix='window-interactive-') as directory:
    base=Path(directory)
    (base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Interactive window operation: edge mapping, press/ownership/state/capture authorization and payload validation passed')
