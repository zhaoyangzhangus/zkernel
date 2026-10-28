"""Run production surface BEGIN/COMMIT/CANCEL with injected wait/close/resize races."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/window.c').read_text()
start = source.index('typedef struct window_surface_wait_context {')
end = source.index('kstatus_t window_server_geometry_owned(', start)
production = source[start:end]
damage_source = (root / 'kernel/graphics/damage.c').read_text()
def damage_function(signature):
    begin = damage_source.index(signature)
    return damage_source[begin:damage_source.index('\n}', begin) + 2]
damage_production = '\n'.join(damage_function(signature) for signature in (
    'static bool window_damage_try_merge(',
    'void window_mark_rect_locked(',
    'void window_mark_surface_locked(',
)).replace('void window_mark_surface_locked(', 'void production_mark_surface_locked(')
program = r'''
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include "kernel/rect.h"
#include "kernel/window_surface_exchange.h"
#include "uapi/window_surface.h"
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define K_ECANCELED -125
#define K_EAGAIN -11
#define K_ETIMEDOUT -110
#define OS_WINDOW_VISIBLE 1
typedef int kstatus_t;
typedef struct {int id;} process_t;
typedef struct {
 process_t *owner;
 window_surface_exchange_t surface_exchange;
 bool surface_exchange_enabled,resize_pending,dirty;
 uint64_t owner_address,surface_span,buffer_size,present_sequence;
 uint32_t width,height,surface_writer_width,surface_writer_height,flags;
    uint32_t surface_front_width,surface_front_height;
    void *controls;
 int32_t x,y;
 atomic_bool registry_removed;
 unsigned surface_waitq;
} window_server_window_t;
static window_server_window_t w;
#define WINDOW_DAMAGE_MAX_RECTS 16U
static struct {
 uint32_t display_width,display_height,damage_count;
 bool dirty,damage_full,damage_tiles_active;
 Rect damage_bounds,damage_rects[WINDOW_DAMAGE_MAX_RECTS];
} g_window_server={.display_width=64,.display_height=64};
static bool window_dirty_edge_locked(void){g_window_server.dirty=true;return true;}
/* 本用例最多提交三个分离区域，不应进入瓦片路径。 */
static void window_damage_enable_tiles_locked(void){assert(false);}
static void window_damage_mark_tiles_locked(const Rect *r){(void)r;assert(false);}
static uint32_t window_client_offset_x(uint32_t flags){(void)flags;return 1;}
static uint32_t window_client_offset_y(uint32_t flags){(void)flags;return 2;}
''' + damage_production + r'''
static void clear_damage(void){
 g_window_server.dirty=false;g_window_server.damage_count=0;
}
static bool covered(int x,int y){
 for(unsigned i=0;i<g_window_server.damage_count;++i){
  Rect r=g_window_server.damage_rects[i];
  if(x>=(int)r.x0&&x<(int)r.x1&&y>=(int)r.y0&&y<(int)r.y1)return true;
 }
 return false;
}
static bool locked;
static unsigned mode,wakes,marks,notifies,shows,projected;
static int last_x,last_y;static unsigned last_width,last_height;
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static bool window_server_owner_locked(process_t *p,window_server_window_t *v){
 assert(locked);return p==v->owner&&!atomic_load(&v->registry_removed);
}
static unsigned wake_all(unsigned *q){assert(!locked&&q==&w.surface_waitq);return ++wakes;}
static void window_mark_surface_locked(window_server_window_t *v,int x,int y,unsigned width,unsigned height){
 assert(locked&&v==&w&&x>=0&&y>=0&&width&&height&&
  (uint64_t)x+width<=w.width&&(uint64_t)y+height<=w.height);
 last_x=x;last_y=y;last_width=width;last_height=height;
 production_mark_surface_locked(v,x,y,width,height);
 assert(w.present_sequence&& !w.resize_pending && w.dirty);++marks;
}
static void window_control_surface_damage_locked(window_server_window_t *v,int x,int y,unsigned width,unsigned height){
 assert(v->controls);++projected;window_mark_surface_locked(v,x,y,width,height);
}
static void window_server_notify_worker(void){assert(!locked);++notifies;}
static kstatus_t window_server_set(window_server_window_t *v,int x,int y,unsigned visible){
 assert(!locked&&v==&w&&x==v->x&&y==v->y&&visible==1);++shows;v->flags|=visible;return 0;
}
static kstatus_t wait_on_queue(unsigned *q,bool (*predicate)(void *),void *context,uint64_t timeout){
 assert(!locked&&q==&w.surface_waitq&&timeout==5000000000ULL);
 if(mode==1){assert(!predicate(context));return K_ETIMEDOUT;}
 if(mode==2){assert(!predicate(context));assert(window_surface_read_end(&w.surface_exchange,0));}
 assert(predicate(context));
 uint64_t serial=w.surface_exchange.serial;
 assert(predicate(context)&&w.surface_exchange.serial==serial); // Idempotent predicate.
 if(mode==3)return K_ECANCELED; // Cancellation after acquisition must abort it.
 if(mode==4){atomic_store(&w.registry_removed,true);window_surface_exchange_close(&w.surface_exchange);}
 return K_OK;
}
''' + production + r'''
int main(void){
 process_t owner={1},other={2};
 w=(window_server_window_t){.owner=&owner,.surface_exchange_enabled=true,
  .surface_span=4096,.buffer_size=8192,.owner_address=0x20000,.width=16,.height=16};
 os_surface_frame_t frame;
 assert(window_server_surface_begin(&other,&w,&frame)==K_EPERM&&!frame.token);
 assert(window_server_surface_begin(&owner,&w,&frame)==K_OK);
 assert(frame.address==0x21000&&frame.capacity==4096&&frame.stride==64);
 os_surface_frame_t bad=frame;bad.address++;
 assert(window_server_surface_end(&owner,&w,&bad,true)==K_EINVAL&&w.surface_exchange.writing);
 bad=frame;bad.token++;
 assert(window_server_surface_end(&owner,&w,&bad,false)==K_EINVAL&&w.surface_exchange.writing);
 frame.damage_x=2;frame.damage_y=3;frame.damage_width=4;frame.damage_height=5;
 assert(window_server_surface_end(&owner,&w,&frame,true)==K_OK);
 assert(last_x==0&&last_y==0&&last_width==16&&last_height==16);
 assert(w.surface_exchange.front==1&&marks==1&&notifies==1&&shows==1);
 assert(w.surface_front_width==16&&w.surface_front_height==16);
 w.controls=&owner;
 assert(window_server_surface_end(&owner,&w,&frame,true)==K_EINVAL);
 w.surface_exchange.readers[0]=1;mode=1;
 assert(window_server_surface_begin(&owner,&w,&frame)==K_ETIMEDOUT&&!frame.token);
 mode=2;assert(window_server_surface_begin(&owner,&w,&frame)==K_OK);
 assert(frame.address==0x20000);w.width=8;w.resize_pending=true;
 assert(window_server_surface_end(&owner,&w,&frame,true)==K_EAGAIN);
 assert(w.surface_exchange.front==1&&!w.surface_exchange.writing&&w.resize_pending&&marks==1);
 assert(w.surface_front_width==16&&w.surface_front_height==16);
 mode=0;assert(window_server_surface_begin(&owner,&w,&frame)==K_OK);
 assert(frame.width==8&&frame.stride==32);
 assert(window_server_surface_end(&owner,&w,&frame,false)==K_OK&&w.surface_exchange.front==1);
 assert(window_server_surface_begin(&owner,&w,&frame)==K_OK);
 frame.damage_x=1;frame.damage_y=1;frame.damage_width=1;frame.damage_height=1;
 assert(window_server_surface_end(&owner,&w,&frame,true)==K_OK);
 assert(last_x==0&&last_y==0&&last_width==8&&last_height==16);
 assert(w.surface_front_width==8&&w.surface_front_height==16&&projected==1&&marks==2);
 /* 同尺寸的稳定帧只标记变化区域，保留完整帧租约和交换语义。 */
 assert(window_server_surface_begin(&owner,&w,&frame)==K_OK);
 frame.damage_x=2;frame.damage_y=3;frame.damage_width=4;frame.damage_height=5;
 assert(window_server_surface_end(&owner,&w,&frame,true)==K_OK);
 assert(last_x==2&&last_y==3&&last_width==4&&last_height==5&&projected==2&&marks==3);
 assert(window_server_surface_begin(&owner,&w,&frame)==K_OK);
 os_surface_frame_t valid=frame;
 for(unsigned i=0;i<6;++i){
  bad=valid;
  if(i==0)bad.damage_x=-1;
  if(i==1)bad.damage_y=-1;
  if(i==2)bad.damage_width=0;
  if(i==3)bad.damage_height=0;
  if(i==4)bad.damage_width=UINT32_MAX;
  if(i==5)bad.damage_height=UINT32_MAX;
  assert(window_server_surface_end(&owner,&w,&bad,true)==K_EINVAL&&w.surface_exchange.writing);
  assert(marks==3);
 }
 assert(window_server_surface_end(&owner,&w,&frame,false)==K_OK&&marks==3);
 /* 合成器尚未捕获时，连续提交只读取最新前台槽，但必须保留全部变化区域。 */
 w.controls=0;w.x=10;w.y=11;clear_damage();
 for(unsigned i=0;i<3;++i){
  assert(window_server_surface_begin(&owner,&w,&frame)==K_OK);
  frame.damage_x=(int32_t)(i*3);frame.damage_y=(int32_t)(i*5);
  frame.damage_width=1;frame.damage_height=2;
  assert(window_server_surface_end(&owner,&w,&frame,true)==K_OK);
 }
 for(int y=0;y<64;++y)for(int x=0;x<64;++x){
  bool expected=false;
  for(int i=0;i<3;++i)if(x==11+i*3&&y>=13+i*5&&y<15+i*5)expected=true;
  assert(covered(x,y)==expected);
 }
 /* 取消的更新不得污染下一帧的损坏区域。 */
 clear_damage();assert(window_server_surface_begin(&owner,&w,&frame)==K_OK);
 frame.damage_x=7;frame.damage_y=15;frame.damage_width=1;frame.damage_height=1;
 assert(window_server_surface_end(&owner,&w,&frame,false)==K_OK);
 assert(!g_window_server.dirty&&!g_window_server.damage_count);
 mode=3;assert(window_server_surface_begin(&owner,&w,&frame)==K_ECANCELED);
 assert(!frame.token&&!w.surface_exchange.writing);
 mode=0;w.surface_exchange.serial=UINT64_MAX;
 assert(window_server_surface_begin(&owner,&w,&frame)==K_EINVAL);
 w.surface_exchange.serial=10;w.width=2048;
 assert(window_server_surface_begin(&owner,&w,&frame)==K_EINVAL);
 w.width=8;mode=4;
 assert(window_server_surface_begin(&owner,&w,&frame)==K_ECANCELED);
 assert(!frame.token&&!w.surface_exchange.writing);
 mode=0;assert(window_server_surface_begin(&owner,&w,&frame)==K_ECANCELED);
 assert(!locked&&wakes);
}
'''
with tempfile.TemporaryDirectory(prefix='window-surface-api-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(root / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=20)
print('Surface API: ownership, slot/stride bounds, stale token, atomic full commit, '
      'partial damage accumulation, cancellation isolation, invalid rectangles, first-frame and resize full damage, timeout, reader retry, resize abort, cancellation after acquire and close passed')
