"""Execute actual owner-checked compositor title metadata update."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/graphics/window.c').read_text()
begin=source.index('kstatus_t window_server_title_owned(')
body=source[begin:source.index('kstatus_t window_server_update(',begin)]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
typedef int process_t,kstatus_t;
enum {K_OK=0,K_EINVAL=-22,K_EIO=-5,K_EPERM=-1,K_ENOMEM=-12,K_EAGAIN=-11};
typedef struct {unsigned height;} caption_text_t;
typedef struct {process_t *owner;char title[32];int32_t x,y;uint32_t width,flags;caption_text_t *caption;} window_server_window_t;
static process_t owner,other;
static window_server_window_t window={.owner=&owner,.x=10,.y=20,.width=640};
static bool locked,ready=true;
static unsigned marks,wakes,lookups;
static bool allocation_fails;
static unsigned caption_text_height(uint32_t flags){(void)flags;return 16;}
static caption_text_t *caption_text_create(const char *text,unsigned height){
 assert(!locked&&text&&height==16);return allocation_fails?NULL:calloc(1,sizeof(caption_text_t));
}
static void caption_text_put(caption_text_t *caption){assert(!locked);free(caption);}
static bool window_server_kernel_ready(void){return ready;}
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static window_server_window_t *window_scene_find_locked(uint32_t id){assert(locked);++lookups;return id==7?&window:NULL;}
static uint32_t window_outer_width(uint32_t w,uint32_t f){(void)f;return w+2;}
static bool window_client_decorations(uint32_t f){return f&4;}
static uint32_t os_window_caption_height(uint32_t f){(void)f;return 40;}
static uint32_t window_client_offset_y(uint32_t f){(void)f;return 41;}
static void window_mark_rect_locked(int32_t x,int32_t y,uint32_t w,uint32_t h){assert(locked&&x==10&&y==20&&w==642&&h==(window.flags&4?40U:41U));++marks;}
static void window_server_notify_worker(void){assert(!locked);++wakes;}
''' + body + r'''
int main(void){
 char title[32];memset(title,'x',32);
 assert(window_server_title_owned(&owner,7,title)==K_EINVAL&&!lookups);
 strcpy(title,"* /notes.txt");
 assert(window_server_title_owned(&other,7,title)==K_EPERM&&!marks&&!locked);
 assert(window_server_title_owned(&owner,8,title)==K_EPERM&&!marks&&!locked);
 ready=false;assert(window_server_title_owned(&owner,7,title)==K_EIO);ready=true;
 assert(!window_server_title_owned(&owner,7,title)&&marks==1&&wakes==1&&!strcmp(window.title,title));
 for(unsigned i=0;i<1000;++i)assert(!window_server_title_owned(&owner,7,title));
 assert(marks==1&&wakes==1);
 allocation_fails=true;strcpy(title,"failed");
 assert(window_server_title_owned(&owner,7,title)==K_ENOMEM&&marks==1&&!strcmp(window.title,"* /notes.txt"));
 allocation_fails=false;
 window.flags=4;strcpy(title,"saved");
 assert(!window_server_title_owned(&owner,7,title)&&marks==2&&wakes==2);
 for(unsigned i=6;i<32;++i)assert(!window.title[i]);
 title[0]=0;assert(!window_server_title_owned(&owner,7,title)&&marks==3&&!window.title[0]);
 assert(window_server_title_owned(NULL,7,title)==K_EINVAL);
 assert(window_server_title_owned(&owner,0,title)==K_EINVAL);
 assert(window_server_title_owned(&owner,7,NULL)==K_EINVAL);
 caption_text_put(window.caption);
}
'''
with tempfile.TemporaryDirectory(prefix='window-title-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Window title metadata: owner isolation, NUL bounds, trailing cleanup, caption-only damage and 1000 no-op updates passed')
