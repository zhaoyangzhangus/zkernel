"""Run production present/update with a compositor snapshot at every unlock."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/window.c').read_text()
damage_source = (root/'kernel/graphics/damage.c').read_text()
damage_start = damage_source.index('void window_mark_window_locked(')
mark_window = damage_source[damage_start:damage_source.index('\n}', damage_start)+2]

def function(signature):
    start = source.rindex(signature)
    end = source.index('{',start)+1
    depth = 1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end += 1
    return source[start:end]

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <kernel/caption_text.h>
#include <kernel/window_geometry.h>
#define OS_ENABLE_FRAME_TRACE 0
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define K_EIO -5
#define K_ECANCELED -125
#define K_ENOMEM -12
#define K_EAGAIN -11
typedef int kstatus_t;
typedef struct {int id;} process_t;
typedef struct {
 process_t *owner;
 uint32_t identifier,width,height,flags;
 int32_t x,y;
 uint64_t owner_address,buffer_size,present_sequence;
 atomic_uint_fast64_t compositor_presented_sequence;
 atomic_bool registry_removed;
 bool dirty,resize_pending,surface_content_ready;
 bool surface_exchange_enabled;
 void *controls;
 char title[32];
 caption_text_t *caption;
 int event_waitq;
} window_server_window_t;
typedef struct {
 window_server_window_t *window;
 uint64_t sequence;
} window_present_wait_context_t;
static window_server_window_t window;
static bool ready=true,inject_snapshot=true;
static int locked,marks,premature,notifies,waits,shows;
static int last_x,last_y;
static uint32_t last_width,last_height;
static uint64_t last_observed_sequence;
static int last_observed_marks;
static bool track_caption_damage;
static unsigned caption_marks,caption_damage[4];
static bool track_pixels;
static unsigned char dirty_pixels[64*400];
static unsigned caption_live,caption_creates;
static int caption_fault;
/* 字形栅格化另有覆盖；这里验证锁外创建、引用交接与重入后的事务检查。 */
caption_text_t *caption_text_create(const char *title,uint32_t height) {
 assert(!locked && title && height && height<=96U);++caption_creates;
 if(caption_fault==1)return 0;
 caption_text_t *text=malloc(sizeof(*text));assert(text);
 atomic_init(&text->references,1);text->height=height;text->width=0;++caption_live;
 if(caption_fault==2)window.title[0]='!';
 if(caption_fault==3)window.identifier=0;
 if(caption_fault==4)window.height=39;
 return text;
}
void caption_text_put(caption_text_t *text) {
 assert(!locked);
 if(text && atomic_fetch_sub(&text->references,1)==1){assert(caption_live);--caption_live;free(text);}
}
static void snapshot(void) {
 assert(!locked);
 if(window.present_sequence!=last_observed_sequence) {
  if(marks==last_observed_marks)++premature;
  last_observed_sequence=window.present_sequence;
  last_observed_marks=marks;
  atomic_store(&window.compositor_presented_sequence,window.present_sequence);
 }
}
static void window_lock(void){assert(!locked);locked=1;}
static void window_unlock(void){assert(locked);locked=0;if(inject_snapshot)snapshot();}
static bool window_server_kernel_ready(void){return ready;}
static window_server_window_t *window_scene_find_locked(uint32_t id){assert(locked);return id==window.identifier?&window:0;}
static bool window_server_owner_locked(process_t *p,window_server_window_t *w){assert(locked);return w==&window && p==w->owner && w->identifier;}
static void window_mark_rect_locked(int32_t x,int32_t y,uint32_t width,uint32_t height){
 assert(locked);++marks;
 if(track_caption_damage){assert(caption_marks<4);caption_damage[caption_marks++]=(window.flags>>16)&255U;}
 if(track_pixels)for(int py=0;py<400;++py)for(int px=0;px<64;++px)
  if((int64_t)px>=x&&(int64_t)py>=y&&(int64_t)px<(int64_t)x+width&&(int64_t)py<(int64_t)y+height)
   dirty_pixels[py*64+px]=1;
}
static void window_mark_surface_locked(window_server_window_t *w,int x,int y,uint32_t width,uint32_t height){
 assert(locked && w==&window);++marks;last_x=x;last_y=y;last_width=width;last_height=height;
}
static void window_server_notify_worker(void){assert(!locked);++notifies;}
static unsigned control_marks;
static void window_control_surface_damage_locked(window_server_window_t *w,int x,int y,uint32_t width,uint32_t height){
 assert(locked&&w->controls);++control_marks;
 window_mark_surface_locked(w,x+10,y+20,width,height);
}
static kstatus_t window_server_set(window_server_window_t *w,int x,int y,uint32_t visible){
 assert(!locked && w==&window && x==w->x && y==w->y && visible==1);++shows;w->flags|=OS_WINDOW_VISIBLE;return 0;
}
static kstatus_t wait_on_queue(int *queue,bool (*predicate)(void*),void *raw,uint64_t timeout){
 assert(!locked && queue==&window.event_waitq && timeout==5000000000ULL);++waits;
 assert(predicate(raw));return 0;
}
''' + '\n'.join((
    mark_window,
    function('static bool window_present_sequence_ready('),
    function('static kstatus_t window_present_wait_result('),
    function('static kstatus_t window_server_update_internal('),
    function('kstatus_t window_server_update('),
    function('kstatus_t window_server_present('),
)) + r'''
int main(void) {
 process_t owner={1},other={2};
 window=(window_server_window_t){.owner=&owner,.identifier=7,.width=100,.height=100,
  .flags=OS_WINDOW_VISIBLE,.x=10,.y=10,.owner_address=4096,.buffer_size=40000};
 // Reproduce the old two-lock publication. A snapshot can acknowledge the
 // new sequence at the first unlock, before any submitted damage exists.
 window_lock();window.present_sequence=1;window_unlock();
 assert(premature==1 && atomic_load(&window.compositor_presented_sequence)==1);
 window_lock();window_mark_surface_locked(&window,1,2,3,4);window_unlock();
 window.present_sequence=0;atomic_store(&window.compositor_presented_sequence,0);
 marks=premature=last_observed_marks=0;last_observed_sequence=0;
 for(uint64_t sequence=1;sequence<=128;++sequence) {
  assert(window_server_present(&owner,&window,4096,40000,400,1,2,3,4)==0);
  assert(premature==0 && window.present_sequence==sequence);
  assert(atomic_load(&window.compositor_presented_sequence)==sequence);
  assert(last_x==1 && last_y==2 && last_width==3 && last_height==4);
 }
 assert(waits==128 && notifies==128 && marks==128);
 const uint64_t sequence=window.present_sequence;
 window.surface_content_ready=false;
 assert(window_server_update(&owner,7,0,0,0,0,0)==0);
 assert(window.present_sequence==sequence && marks==129 && window.surface_content_ready);
 window.resize_pending=true;window.flags=0;
 assert(window_server_present(&owner,&window,4096,40000,400,0,0,100,100)==0);
 assert(!window.resize_pending && shows==1 && window.flags==OS_WINDOW_VISIBLE);
 const int saved_marks=marks,saved_waits=waits;
 const uint64_t saved_sequence=window.present_sequence;
 assert(window_server_present(&other,&window,4096,40000,400,0,0,100,100)==K_EPERM);
 assert(window_server_present(&owner,&window,4097,40000,400,0,0,100,100)==K_EINVAL);
 assert(window_server_present(&owner,&window,4096,39999,400,0,0,100,100)==K_EINVAL);
 assert(window_server_present(&owner,&window,4096,40000,399,0,0,100,100)==K_EINVAL);
 assert(window_server_present(&owner,&window,4096,40000,400,99,0,2,1)==K_EINVAL);
 ready=false;
 assert(window_server_present(&owner,&window,4096,40000,400,0,0,100,100)==K_EIO);
 ready=true;
 assert(window.present_sequence==saved_sequence && marks==saved_marks && waits==saved_waits);
 assert(window_server_update(&other,7,0,0,3,4,0)==K_EPERM);
 assert(window_server_update(&owner,7,0,0,3,0,0)==K_EINVAL);
 assert(window_server_update(&owner,7,0,0,3,4,1)==K_EINVAL);
 window.flags=21U;
 // 分配失败和锁外创建期间的窗口变化不能发布尺寸、损坏或栅栏。
 for(caption_fault=1;caption_fault<=4;++caption_fault){
  int before_marks=marks;uint64_t before_sequence=window.present_sequence;
  uint32_t before_flags=window.flags;caption_text_t *before_caption=window.caption;
  unsigned before_live=caption_live;
  int expected=caption_fault==1?K_ENOMEM:caption_fault==3?K_EPERM:K_EAGAIN;
  assert(window_server_update(&owner,7,0,0,3,4,76U<<16U)==expected);
  assert(marks==before_marks && window.present_sequence==before_sequence);
  assert(window.flags==before_flags && window.caption==before_caption && caption_live==before_live);
  window.title[0]=0;window.identifier=7;window.height=100;
 }
 caption_fault=0;
 assert(window_server_update(&owner,7,0,0,3,4,76U<<16U)==0);
 assert(caption_live==1 && window.caption->height==caption_text_height(window.flags));
 unsigned before_noop=caption_creates;
 assert(window_server_update(&owner,7,0,0,3,4,76U<<16U)==0);
 assert(caption_creates==before_noop && caption_live==1);
 assert((window.flags&OS_WINDOW_CAPTION_HEIGHT_MASK)==(76U<<16U));
 assert(window_server_update(&owner,7,0,0,3,4,39U<<16U)==K_EINVAL);
 assert(window_server_update(&owner,7,0,0,3,4,129U<<16U)==K_EINVAL);
 assert(window_server_update(&owner,7,0,0,3,4,101U<<16U)==K_EINVAL);
 // A font-dependent caption changes the outer height, not client height.
 // Damage must include the OLD extent before flags change, or shrinking
 // leaves successive white bottom borders on the retained desktop.
 track_caption_damage=true;
 for(unsigned old=40;old<=100;++old)for(unsigned next=40;next<=100;++next){
  window.flags=21U|(old<<16);caption_marks=0;
  assert(window_server_update(&owner,7,0,0,3,4,next<<16)==K_OK);
  if(old==next)assert(!caption_marks);
  else assert(caption_marks==2 && caption_damage[0]==old && caption_damage[1]==next);
 }
 track_caption_damage=false;
 // Use real frame geometry and actual window damage production code. Model
 // repaint only inside submitted damage; every old white pixel outside the
 // new footprint must become wallpaper, including clipped screen edges.
 const unsigned captions[]={40,41,48,56,76,100,128};
 const int positions[][2]={{8,8},{-8,8},{8,-8},{50,8},{8,220},{-8,-8}};
 unsigned pixel_cases=0;
 track_pixels=true;window.width=32;window.height=160;
 for(unsigned client=0;client<2;++client)
 for(unsigned p=0;p<6;++p)
 for(unsigned a=0;a<7;++a)for(unsigned b=0;b<7;++b){
  window.x=positions[p][0];window.y=positions[p][1];
  window.flags=OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_VISIBLE|
   (client?OS_WINDOW_CLIENT_DECORATIONS:0)|OS_WINDOW_CAPTION_HEIGHT(captions[a]);
  unsigned old_w=window_outer_width(window.width,window.flags);
  unsigned old_h=window_outer_height(window.height,window.flags);
  memset(dirty_pixels,0,sizeof(dirty_pixels));
  assert(window_server_update(&owner,7,0,0,0,0,OS_WINDOW_CAPTION_HEIGHT(captions[b]))==K_OK);
  unsigned new_w=window_outer_width(window.width,window.flags);
  unsigned new_h=window_outer_height(window.height,window.flags);
  for(int py=0;py<400;++py)for(int px=0;px<64;++px){
   bool old_white=px>=window.x&&py>=window.y&&(int64_t)px<(int64_t)window.x+old_w&&(int64_t)py<(int64_t)window.y+old_h;
   bool new_white=px>=window.x&&py>=window.y&&(int64_t)px<(int64_t)window.x+new_w&&(int64_t)py<(int64_t)window.y+new_h;
   bool final_white=dirty_pixels[py*64+px]?new_white:old_white;
   assert(final_white==new_white);
  }
  ++pixel_cases;
 }
 assert(pixel_cases==588);
 track_pixels=false;window.controls=&window;window.width=100;window.height=100;
 unsigned previous_controls=control_marks;
 assert(window_server_present(&owner,&window,4096,40000,400,1,2,3,4)==K_OK);
 assert(control_marks==previous_controls+1&&last_x==11&&last_y==22&&last_width==3&&last_height==4);
 assert(atomic_load(&window.compositor_presented_sequence)==window.present_sequence);
 assert(premature==0 && !locked);
 caption_text_put(window.caption);window.caption=0;assert(!caption_live);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-present-transaction-') as directory:
    path=Path(directory)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    def compile_program(actual):
        (path/'test.c').write_text(actual)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),
                    str(root/'kernel/graphics/window_geometry.c'),'-o',str(path/'test')],check=True)
    compile_program(program)
    subprocess.run([str(path/'test')],check=True)
    old_mark='window_mark_window_locked(window);\n            window->flags = next_flags;'
    mark_assert='else assert(caption_marks==2 && caption_damage[0]==old && caption_damage[1]==next);'
    assert old_mark in program and mark_assert in program
    # Disable only the mark-count assertion so the independent pixel oracle,
    # not the earlier structural check, must detect the original bottom trail.
    negative=program.replace(old_mark,'window->flags = next_flags;').replace(mark_assert,'else (void)0;')
    compile_program(negative)
    failed=subprocess.run([str(path/'test')],capture_output=True,text=True,timeout=10)
    assert failed.returncode!=0 and 'final_white==new_white' in failed.stderr,failed
print('present transaction: old premature ACK reproduced; production damage/fence atomic, ownership/validation/resize/visibility passed (ASan/UBSan)')
print('Caption damage: 3721 grow/shrink/no-op transitions retain old/new footprints in one transaction')
print('Caption pixels: 588 native/client grow/shrink/no-op and clipped-edge repaint oracles passed')
print('Caption transaction: allocation failure, title/removal/height races, no-op reuse and reference cleanup passed')
print('Negative control rejected by pixel oracle: omitting old-footprint damage leaves the old border')
