"""Execute the actual locked owner-checked native maximize dispatcher."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/graphics/input_router.c').read_text()
start=source.index('kstatus_t window_server_set_maximized_owned(')
body=source[start:source.index('\n}',start)+2]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef int process_t,kstatus_t;
enum {K_OK=0,K_EINVAL=-22,K_EPERM=-1};
typedef struct {process_t *owner;uint32_t identifier;bool maximized;bool fullscreen;} window_server_window_t;
static process_t owner,other;
static window_server_window_t window={&owner,7,false,false};
static bool ready=true,registered=true,locked,allowed=true;
static unsigned toggles,wakes;
static bool window_server_kernel_ready(void){return ready;}
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static window_server_window_t *window_scene_find_locked(unsigned id){assert(locked);return registered&&id==7?&window:0;}
static bool window_toggle_maximize_locked(window_server_window_t *w){assert(locked);++toggles;if(!allowed)return false;w->maximized=!w->maximized;return true;}
static void window_server_notify_worker(void){assert(!locked);++wakes;}
''' + body + r'''
int main(void){
 for(unsigned i=0;i<1000;++i){
  assert(!window_server_set_maximized_owned(&owner,&window,1)&&window.maximized);
  unsigned n=toggles;assert(!window_server_set_maximized_owned(&owner,&window,1)&&toggles==n);
  assert(!window_server_set_maximized_owned(&owner,&window,0)&&!window.maximized);
 }
 assert(toggles==2000&&wakes==2000&&!locked);
 window.fullscreen=true;assert(window_server_set_maximized_owned(&owner,&window,1)==-22&&!window.maximized&&toggles==2000);window.fullscreen=false;
 assert(window_server_set_maximized_owned(&other,&window,1)==-1&&!locked);
 registered=false;assert(window_server_set_maximized_owned(&owner,&window,1)==-1);registered=true;
 allowed=false;assert(window_server_set_maximized_owned(&owner,&window,1)==-22&&!window.maximized&&wakes==2000);
 ready=false;assert(window_server_set_maximized_owned(&owner,&window,1)==-22);ready=true;
 assert(window_server_set_maximized_owned(&owner,&window,2)==-22);
 assert(window_server_set_maximized_owned(0,&window,1)==-22);
 assert(window_server_set_maximized_owned(&owner,0,1)==-22);
}
'''
with tempfile.TemporaryDirectory(prefix='window-maximize-owned-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Native maximize dispatcher: 1000 round trips, no-op suppression, owner isolation and failure guards passed')
