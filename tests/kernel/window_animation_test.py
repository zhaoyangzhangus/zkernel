"""真实动画所有者：时间推进、圆角像素、前台槽位租约、退出和快速恢复，不启动 QEMU。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
body = (root/'kernel/graphics/animation.c').read_text().replace('#include "internal.h"', '').replace('#include <arch/x86_64/cpu.h>', '')
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <kernel/control_raster.h>
#include <kernel/control_tree.h>
#define WINDOW_SERVER_MAX_WINDOWS 64U
#define OS_WINDOW_VISIBLE 1U
typedef struct {unsigned identifier,width,height,flags;int x,y;bool minimized,client_show_inactive,compositor_presented_valid,animation_seen,surface_content_ready;
 uint64_t animation_start,present_sequence;bool animation_restore;int animation_dx,animation_dy;void *controls;unsigned refs,desktop_app;} window_server_window_t;
typedef struct {window_server_window_t *reference;void *controls,*caption;unsigned identifier,width,height,flags,animation_progress,animation_kind;int x,y,animation_dx,animation_dy;} compositor_window_view_t;
static struct {unsigned focused_identifier,display_width,display_height,display_stride;struct {unsigned count;window_server_window_t *children[64];} scene;
 bool composing;uint32_t *framebuffer,*composite_framebuffer;} g_window_server;
static struct {uint64_t animation_layers,animation_time;bool animations,drag_blit_valid;Rect damage_bounds;} g_compositor_snapshot;
static uint64_t now=100;
static unsigned lock_depth,marks,leases,documents,releases,draws;
static unsigned pixels[128*96];
static uint32_t scene[128*96];
static bool desktop_icon_box_locked(unsigned app,control_tree_box_t *box){
 (void)app;box->bounds=(Rect){100,80,120,96};return true;
}
static uint64_t x86_read_tsc(void){return now;}
static uint64_t x86_tsc_to_ns(uint64_t tsc){return tsc;}
static uint64_t x86_timeout_ns_to_tsc(uint64_t ns){return ns;}
static void window_lock(void){assert(!lock_depth);lock_depth=1;}
static void window_unlock(void){assert(lock_depth==1);lock_depth=0;}
static unsigned window_outer_width(unsigned w,unsigned flags){(void)flags;return w;}
static unsigned window_outer_height(unsigned h,unsigned flags){(void)flags;return h;}
static void window_mark_rect_locked(int x,int y,unsigned w,unsigned h){(void)x;(void)y;assert(lock_depth&&w&&h);++marks;}
static void window_mark_window_locked(window_server_window_t *w){assert(lock_depth&&w);++marks;}
static window_server_window_t *window_scene_find_locked(unsigned id){
 for(unsigned i=0;i<g_window_server.scene.count;++i)if(g_window_server.scene.children[i]->identifier==id)return g_window_server.scene.children[i];return 0;
}
static void compositor_capture_window_locked(window_server_window_t *w,compositor_window_view_t *v){
 assert(lock_depth&&w->refs);++w->refs;++leases;++documents;
 *v=(compositor_window_view_t){.reference=w,.controls=w->controls,.identifier=w->identifier,
  .width=w->width,.height=w->height,.flags=w->flags,.x=w->x,.y=w->y};
}
static void window_surface_snapshot_release(compositor_window_view_t *v){assert(!lock_depth&&v->reference&&leases);--leases;}
static void window_control_document_put(void *p){assert(!lock_depth&&p&&documents);--documents;}
static void caption_text_put(void *p){assert(!lock_depth&&!p);}
static void object_put(window_server_window_t *w){assert(!lock_depth&&w->refs);--w->refs;++releases;}
static void compositor_draw_window_locked(const compositor_window_view_t *v){
 assert(v&&g_window_server.composing);++draws;Rect r=g_compositor_snapshot.damage_bounds;
 assert(r.x0>=0&&r.y0>=0&&r.x1<=128&&r.y1<=96);
 for(int y=r.y0;y<r.y1;++y)for(int x=r.x0;x<r.x1;++x)if(x>=v->x&&x<v->x+(int)v->width&&y>=v->y&&y<v->y+(int)v->height){++pixels[y*128+x];scene[y*128+x]=0xffffff;}
}
''' + body + r'''
static void depart(window_server_window_t *w){window_lock();window_animation_depart_locked(w,true);window_unlock();}
static void snapshot(void){window_lock();window_animation_snapshot_locked();g_window_server.composing=true;window_unlock();}
static void done(void){g_window_server.composing=false;}
int main(void){
 g_window_server.display_width=128;g_window_server.display_height=96;
 g_window_server.composite_framebuffer=scene;g_window_server.framebuffer=(void*)2;g_window_server.display_stride=128;
 g_compositor_snapshot.damage_bounds=(Rect){0,0,128,96};
 assert(!window_animation_ready()&&window_animation_wait_ns()==UINT64_MAX);
 uint32_t previous=0;
 for(uint64_t n=0;n<=400000000ULL;n+=100000ULL){
  unsigned p=window_animation_progress(1,n+1);assert(p>=previous&&p<=10000);previous=p;
 }assert(previous==10000);
 /* 多尺寸/屏外位置/损坏矩形下，每个有效像素只绘制一次，裁剪之外不落笔。 */
 for(unsigned p=0;p<=10000;p+=137)for(int off=-20;off<=20;off+=10){
  compositor_window_view_t v={.x=off+25,.y=off+20,.width=80,.height=60,.animation_progress=p};
  g_compositor_snapshot.damage_bounds=(Rect){13,11,111,83};memset(pixels,0,sizeof(pixels));draws=0;
  g_window_server.composing=true;compositor_animation_window(&v);g_window_server.composing=false;
  unsigned w=80*p/10000,h=60*p/10000;int x0=v.x+(80-(int)w)/2,y0=v.y+(60-(int)h)/2;
  unsigned corner=control_round_radius(8,w,h);
  for(int y=0;y<96;++y)for(int x=0;x<128;++x){
   bool inside=w&&h&&x>=x0&&x<x0+(int)w&&y>=y0&&y<y0+(int)h;
   if(inside){unsigned inset=control_round_inset((unsigned)(y-y0),w,h,corner);inside=x>=x0+(int)inset&&x<x0+(int)w-(int)inset;}
   inside=inside&&x>=13&&x<111&&y>=11&&y<83;
   assert(pixels[y*128+x]==(unsigned)inside);
  }
  assert(draws<=17);assert(g_compositor_snapshot.damage_bounds.x0==13&&g_compositor_snapshot.damage_bounds.y1==83);
 }
 /* 关闭只淡出颜色，不能复用最小化的缩小裁剪。 */
 for(unsigned p=0;p<=10000;p+=1000){
  compositor_window_view_t v={.x=20,.y=20,.width=80,.height=60,.animation_progress=p,.animation_kind=WINDOW_ANIMATION_CLOSE};
  for(unsigned i=0;i<128*96;++i)scene[i]=0x224466;
  g_compositor_snapshot.damage_bounds=(Rect){0,0,128,96};g_window_server.composing=true;
  compositor_animation_window(&v);g_window_server.composing=false;
  uint32_t expected=control_blend_argb(0x224466,(p*255U/10000U<<24)|0xffffff);
  for(int y=0;y<96;++y)for(int x=0;x<128;++x)
   assert(scene[y*128+x]==(x>=20&&x<100&&y>=20&&y<80?expected:0x224466));
 }
 window_server_window_t window={.identifier=7,.width=80,.height=60,.x=20,.y=20,.flags=1,
  .refs=1,.controls=(void*)1,.compositor_presented_valid=true};
 g_window_server.scene.children[0]=&window;g_window_server.scene.count=1;g_window_server.focused_identifier=7;
 compositor_window_view_t view={.reference=&window};
 /* 普通 UPDATE 没有栅栏序号，也没有控件文档；必须由真实内容就绪状态触发。 */
 window.controls=0;window.present_sequence=0;
 snapshot();window_animation_capture_locked(&view);done();assert(!window.animation_seen);
 window.surface_content_ready=true;
 snapshot();window_animation_capture_locked(&view);done();
 assert(view.animation_progress==0&&window_animation_ready()&&g_compositor_snapshot.animations);
 /* 首帧再慢也从实际发布时计时，不得在渲染期间把动画耗尽。 */
 now+=500000000;window_lock();window_animation_presented_locked(&view);window_unlock();
 assert(window.animation_start==now);
 window.controls=(void*)1;
 now+=150000000;window_animation_tick();snapshot();window_animation_capture_locked(&view);done();
 assert(view.animation_progress>8000&&view.animation_progress<10000);
 now+=200000000;window_animation_tick();assert(!window.animation_start&&window_animation_wait_ns()==UINT64_MAX);
 /* 最小化不延迟逻辑状态；保留的引用在动画结束后准确释放。 */
 depart(&window);window.minimized=true;assert(window.refs==2&&leases==1&&documents==1);
 assert(g_window_departures[0].view.animation_kind==WINDOW_ANIMATION_MINIMIZE);
 assert(g_window_departures[0].target_dx==50&&g_window_departures[0].target_dy==38);
 snapshot();assert(g_compositor_snapshot.animation_layers&&g_compositor_snapshot.animations);
 g_compositor_snapshot.damage_bounds=(Rect){0,0,128,96};compositor_animation_layers();
 done();now+=150000000;window_animation_tick();snapshot();
 memset(pixels,0,sizeof(pixels));compositor_animation_layers();
 assert(pixels[83*128+102]&&!pixels[40*128+40]);
 now+=400000000;window_animation_tick();assert(window.refs==2);done();
 window_animation_tick();assert(window.refs==1&&!leases&&!documents&&releases==1);
 assert(window_animation_wait_ns()==UINT64_MAX);
 /* 恢复逐像素反向重走最小化轨迹，且不得改变实际几何或快照坐标。 */
 window_lock();window_animation_restore_locked(&window);window_unlock();
 assert(window.animation_restore&&window.animation_dx==50&&window.animation_dy==38);
 window.minimized=false;
 for(unsigned p=0;p<=10000;p+=125){
  compositor_window_view_t restore={.reference=&window,.x=20,.y=20,.width=80,.height=60,
   .animation_kind=WINDOW_ANIMATION_RESTORE,.animation_progress=p,.animation_dx=50,.animation_dy=38};
  compositor_window_view_t reverse=restore;
  reverse.animation_kind=WINDOW_ANIMATION_MINIMIZE;
  reverse.x+=(int)(50LL*(10000-p)/10000);reverse.y+=(int)(38LL*(10000-p)/10000);
  memset(pixels,0,sizeof(pixels));g_window_server.composing=true;
  compositor_animation_window(&restore);unsigned expected_pixels[128*96];memcpy(expected_pixels,pixels,sizeof(pixels));
  memset(pixels,0,sizeof(pixels));compositor_animation_window(&reverse);g_window_server.composing=false;
  assert(!memcmp(expected_pixels,pixels,sizeof(pixels))&&restore.x==20&&restore.y==20&&window.x==20&&window.y==20);
 }
 view=(compositor_window_view_t){.reference=&window,.x=20,.y=20,.width=80,.height=60};
 snapshot();window_animation_capture_locked(&view);done();
 assert(view.animation_kind==WINDOW_ANIMATION_RESTORE&&view.animation_progress==0);
 now+=400000000;window_animation_tick();assert(!window.animation_restore&&!window.animation_start);
 /* 关闭后注册表已移除，也能完成退场，而不是提前释放表面。 */
 window.minimized=false;window_lock();window_animation_depart_locked(&window,false);window_unlock();
 assert(g_window_departures[0].view.animation_kind==WINDOW_ANIMATION_CLOSE);
 g_window_server.scene.count=0;assert(window.refs==2);
 now+=400000000;window_animation_tick();assert(window.refs==1&&!leases&&!documents);
 /* 快速最小化再恢复，下一帧前取消旧退场层。 */
 g_window_server.scene.count=1;depart(&window);window.minimized=true;window.minimized=false;
 window_animation_tick();assert(window.refs==1&&!leases&&!documents);
 /* 槽位满时不阻塞；清理全部槽位，无悬挂引用。 */
 for(unsigned i=0;i<65;++i)depart(&window);
 assert(leases==64&&documents==64&&window.refs==65);g_window_server.scene.count=0;
 now+=400000000;window_animation_tick();assert(window.refs==1&&!leases&&!documents);
 assert(!window_animation_ready()&&window_animation_wait_ns()==UINT64_MAX&&!lock_depth&&marks);
 puts("Window animation: easing, 4.5M clipped rounded pixels, frame lifetime, minimize/close/restore/pool cleanup PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='window-animation-') as directory:
    work = Path(directory)
    (work/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root/'include'), str(work/'test.c'), '-o', str(work/'test')], check=True)
    subprocess.run([str(work/'test')], check=True)
