"""执行生产帧提交和扫描线发布，验证边缘裁剪、拖动包围盒及帧缓冲守卫。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
commit = (root / 'kernel/graphics/compositor.c').read_text()
start = commit.index('void compositor_commit_snapshot(void)')
commit = commit[start:commit.index('\n}', start) + 2]
policy = (root / 'kernel/graphics/publication_policy.c').read_text()
start = policy.index('uint64_t compositor_publication_damage_pixels(void)')
end = policy.index('\n/*\n * 从已完成的保留 WB 场景', start)
policy = policy[start:end]
damage = (root / 'kernel/graphics/damage_plan.c').read_text()
def function(name):
    result = 'bool ' if name == 'compositor_snapshot_damage_bounds' else 'void '
    start = damage.index(result + name + '(')
    return damage[start:damage.index('\n}', start) + 2]
bounds = function('compositor_snapshot_damage_bounds')
collapse = function('compositor_collapse_drag_damage')
fence = '__asm__ volatile ("sfence" : : : "memory");'
commit = commit.replace('__asm__ volatile (\n        "sfence"\n        :\n        :\n        : "memory");', 'fence();')
policy = policy.replace(fence, 'fence();')
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <kernel/rect.h>
typedef int64_t kstatus_t;
#define K_OK 0
#define WINDOW_COMPOSITOR_ATOMIC_PIXELS (128U * 1024U)
#define WINDOW_COMPOSITOR_PUBLICATION_YIELD_PIXELS (128U * 1024U)
static struct { uint32_t *framebuffer, *composite_framebuffer;
 uint32_t display_width,display_height,display_stride; } g_window_server;
typedef struct { unsigned dragging_identifier,damage_count; Rect damage_rects[4]; } compositor_snapshot_t;
static compositor_snapshot_t g_compositor_snapshot;
static kstatus_t g_compositor_publication_status;
static atomic_uint_fast64_t g_compositor_native_retry_deadline;
static unsigned preempt,copied,prepares,exposures;
static uint64_t x86_read_tsc(void){return 0;}
static uint64_t x86_timeout_ns_to_tsc(uint64_t ns){return ns;}
static bool display_core_has_native_scanout(unsigned display){assert(!display);return false;}
static void display_core_poll_scanout(void){assert(!"GOP must not poll native scanout");}
static kstatus_t compositor_submit_native_snapshot(void){assert(!"GOP must not submit native scanout");return 0;}
static void sched_preempt_disable(void){assert(!preempt);preempt=1;}
static void sched_preempt_enable(void){assert(preempt);preempt=0;}
static void fence(void){assert(preempt);}
static bool sched_tick_should_preempt(void){return false;}
static unsigned input_core_pending(void){return 0;}
static void schedule(void){assert(!preempt);}
static void window_server_pump_publication_motion(void){assert(!preempt);}
static void compositor_publication_drag_old_exposure(void){assert(preempt);++exposures;}
static void window_present_cursor_prepare_rect(const Rect *r){
 assert(preempt&&r->x0>=0&&r->y0>=0&&r->x1<=5&&r->y1<=4);++prepares;
}
static bool window_present_cursor_publish_span(volatile uint32_t *d,const uint32_t *s,
 uint32_t x,uint32_t y,uint32_t n){(void)d;(void)s;(void)x;(void)y;(void)n;assert(preempt);return false;}
static void compositor_copy_wc_scanline(volatile uint32_t *d,const uint32_t *s,uint32_t n){
 assert(preempt);copied+=n;for(unsigned i=0;i<n;++i)d[i]=s[i];
}
''' + bounds + '\n' + collapse + '\n' + policy + '\n' + commit + r'''
int main(void){
 uint32_t front[48],back[48];
 g_window_server.framebuffer=front;g_window_server.composite_framebuffer=back;
 g_window_server.display_width=5;g_window_server.display_height=4;g_window_server.display_stride=8;
 const Rect cases[]={ {-3,-2,2,2},{0,0,5,4},{3,2,20,20},{-9,1,-1,3},
  {7,1,9,3},{1,1,1,3},{3,3,1,1},{INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX},
  {INT32_MIN,1,-1,3},{1,1,3,3} };
 unsigned count=0;
 for(unsigned drag=0;drag<2;++drag)
 for(unsigned a=0;a<sizeof(cases)/sizeof(*cases);++a)
 for(unsigned b=0;b<sizeof(cases)/sizeof(*cases);++b){
  for(unsigned i=0;i<48;++i){front[i]=0xdead;back[i]=1000+i;}
  g_compositor_snapshot.dragging_identifier=drag;
  g_compositor_snapshot.damage_count=2;
  g_compositor_snapshot.damage_rects[0]=cases[a];g_compositor_snapshot.damage_rects[1]=cases[b];
  copied=prepares=exposures=0;
  compositor_commit_snapshot();assert(!preempt&&exposures==drag);
  /* 独立的像素判定不使用生产裁剪函数，退化矩形不参与拖动包围盒。 */
  bool valid=false;int64_t left=0,top=0,right=0,bottom=0;
  if(drag)for(unsigned r=0;r<2;++r){
   Rect c=g_compositor_snapshot.damage_rects[r];
   if(c.x0>=c.x1||c.y0>=c.y1)continue;
   if(!valid){left=c.x0;top=c.y0;right=c.x1;bottom=c.y1;valid=true;}
   else {if(c.x0<left)left=c.x0;if(c.y0<top)top=c.y0;
    if(c.x1>right)right=c.x1;if(c.y1>bottom)bottom=c.y1;}
  }
  unsigned expected_pixels=0;
  for(unsigned y=0;y<6;++y)for(unsigned x=0;x<8;++x){
   bool visible=false;
   if(y<4&&x<5){
    if(drag)visible=valid&&(int64_t)x>=left&&(int64_t)x<right&&(int64_t)y>=top&&(int64_t)y<bottom;
    else for(unsigned r=0;r<2;++r){Rect c=g_compositor_snapshot.damage_rects[r];
     visible|=(int64_t)x>=c.x0&&(int64_t)x<c.x1&&(int64_t)y>=c.y0&&(int64_t)y<c.y1;
     if((int64_t)x>=c.x0&&(int64_t)x<c.x1&&(int64_t)y>=c.y0&&(int64_t)y<c.y1)++expected_pixels;
    }
    if(drag&&visible)++expected_pixels;
   }
   assert(front[y*8+x]==(visible?back[y*8+x]:0xdead));
  }
  assert(copied==expected_pixels);++count;
  if(drag){
   /* 规划阶段折叠与最终提交必须采用相同的非空有符号包围盒。 */
   compositor_collapse_drag_damage(&g_compositor_snapshot);
   if(valid){
    assert(g_compositor_snapshot.damage_count==1);
    Rect collapsed=g_compositor_snapshot.damage_rects[0];
    assert(collapsed.x0==left&&collapsed.y0==top&&collapsed.x1==right&&collapsed.y1==bottom);
   }
   for(unsigned i=0;i<48;++i)front[i]=0xdead;
   copied=prepares=exposures=0;
   compositor_commit_snapshot();
   assert(copied==expected_pixels&&!preempt);
   for(unsigned y=0;y<6;++y)for(unsigned x=0;x<8;++x){
    bool visible=y<4&&x<5&&valid&&(int64_t)x>=left&&(int64_t)x<right&&(int64_t)y>=top&&(int64_t)y<bottom;
    assert(front[y*8+x]==(visible?back[y*8+x]:0xdead));
   }
  }
 }
 puts("Frame commit: 200 signed/extreme/empty rectangle pairs, exact ordinary and drag pixels, padded-stride guards and balanced preemption PASS");
 assert(count==200);
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-commit-') as directory:
    binary = Path(directory) / 'test'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', str(binary)],
                   input=program, text=True, check=True)
    subprocess.run([str(binary)], check=True)
