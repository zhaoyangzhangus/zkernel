"""执行真实相交与包含路径，对照宽整数参考并验证空区域和贴边规则。"""
from pathlib import Path
import subprocess
import resource
import tempfile

root = Path(__file__).resolve().parents[2]


def function(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


functions = '\n'.join(function(path, signature) for path, signature in (
    ('kernel/graphics/tile_metadata.c', 'static bool tile_view_bounds('),
    ('kernel/graphics/tile_metadata.c', 'static bool tile_inset_opaque_bounds('),
    ('kernel/graphics/tile_metadata.c', 'bool compositor_view_opaque_bounds('),
    ('kernel/graphics/render_plan.c', 'bool compositor_view_fully_covers_snapshot_bounds('),
    ('kernel/graphics/cursor_occlusion.c', 'bool compositor_damage_inside_surface_interior('),
    ('kernel/graphics/render_plan.c', 'static bool compositor_view_intersects_render_rect('),
    ('kernel/graphics/cursor_occlusion.c', 'bool compositor_window_intersects_damage_locked('),
    ('kernel/graphics/damage_plan.c', 'bool compositor_snapshot_damage_bounds('),
    ('kernel/graphics/damage_plan.c', 'bool compositor_view_intersects_snapshot_damage('),
    ('kernel/graphics/damage_plan.c', 'uint32_t compositor_snapshot_occlusion_floor('),
    ('kernel/graphics/damage_plan.c', 'bool compositor_registry_window_intersects_damage_bounds_locked('),
))
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/graphics/internal.h"
static unsigned intersection_queries;
static bool measured_intersection(Rect r,int64_t l,int64_t t,int64_t right,int64_t b){
 ++intersection_queries;return rect_intersects_bounds(r,l,t,right,b);
}
#define rect_intersects_bounds measured_intersection
''' + functions + r'''
#undef rect_intersects_bounds

static compositor_snapshot_t snapshot;
static uint64_t seed=739;
static uint32_t random_value(void){seed=seed*6364136223846793005ULL+1;return seed>>32;}
/* 用交集宽高而非生产实现的分离轴条件作为参考，不做可能溢出的减法。 */
static bool oracle(Rect r,__int128 left,__int128 top,__int128 right,__int128 bottom){
 if(r.x0>=r.x1||r.y0>=r.y1||left>=right||top>=bottom)return false;
 __int128 x0=left>r.x0?left:r.x0,y0=top>r.y0?top:r.y0;
 __int128 x1=right<r.x1?right:r.x1,y1=bottom<r.y1?bottom:r.y1;
 return x0<x1&&y0<y1;
}
static bool inside(Rect r,__int128 left,__int128 top,__int128 right,__int128 bottom){
 if(r.x0>=r.x1||r.y0>=r.y1||left>=right||top>=bottom)return false;
 /* 非空矩形的交集与自身相等时，才算完全位于容器内。 */
 __int128 x0=left>r.x0?left:r.x0,y0=top>r.y0?top:r.y0;
 __int128 x1=right<r.x1?right:r.x1,y1=bottom<r.y1?bottom:r.y1;
 return x0==r.x0&&y0==r.y0&&x1==r.x1&&y1==r.y1;
}
static void check_window(compositor_window_view_t w,Rect r){
 bool client=(w.flags&OS_WINDOW_CLIENT_DECORATIONS)!=0;
 uint64_t width=(uint64_t)w.width+(client?0:2);
 uint64_t height=(uint64_t)w.height+(client?0:2+WINDOW_TITLEBAR_HEIGHT);
 assert(width<=UINT32_MAX&&height<=UINT32_MAX);
 unsigned inset=client?(w.maximized?0:8):WINDOW_CORNER_RADIUS;
 bool covered=!(w.flags&OS_WINDOW_PREMULTIPLIED_ALPHA)&&width>2*inset&&height>2*inset&&
  inside(r,(__int128)w.x+inset,(__int128)w.y+inset,
   (__int128)w.x+width-inset,(__int128)w.y+height-inset);
 assert(compositor_view_fully_covers_snapshot_bounds(&w,&r)==covered);
 unsigned border=client?0:WINDOW_FRAME_BORDER;
 unsigned offset=client?0:WINDOW_FRAME_BORDER+WINDOW_TITLEBAR_HEIGHT;
 bool interior=w.width>2*WINDOW_CORNER_RADIUS&&w.height>2*WINDOW_CORNER_RADIUS&&
  inside(r,(__int128)w.x+border+WINDOW_CORNER_RADIUS,(__int128)w.y+offset+WINDOW_CORNER_RADIUS,
   (__int128)w.x+border+w.width-WINDOW_CORNER_RADIUS,
   (__int128)w.y+offset+w.height-WINDOW_CORNER_RADIUS);
 assert(compositor_damage_inside_surface_interior(r,&w)==interior);
 bool expected=oracle(r,w.x,w.y,(__int128)w.x+width,(__int128)w.y+height);
 assert(compositor_view_intersects_render_rect(&w,&r)==expected);
 assert(compositor_window_intersects_damage_locked(r,&w)==expected);
 snapshot.damage_count=1;snapshot.damage_rects[0]=r;
 Rect bounds={0};(void)compositor_snapshot_damage_bounds(&snapshot,&bounds);
 assert(compositor_view_intersects_snapshot_damage(&w,&snapshot,&bounds)==expected);
 /* 无效矩形不能使“任意损坏区域相交”的查询提前返回真。 */
 snapshot.damage_count=2;snapshot.damage_rects[0]=(Rect){5,5,5,5};snapshot.damage_rects[1]=r;
 bounds=(Rect){0};(void)compositor_snapshot_damage_bounds(&snapshot,&bounds);
 assert(compositor_view_intersects_snapshot_damage(&w,&snapshot,&bounds)==expected);
 window_server_window_t live={.x=w.x,.y=w.y,.width=w.width,.height=w.height,.flags=w.flags};
 assert(compositor_registry_window_intersects_damage_bounds_locked(&live,&r)==expected);
}
int main(void){
 const Rect rects[]={{0,0,10,10},{5,5,5,5},{6,6,4,4},
  {INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX},{-10,-10,0,0},{INT32_MAX-1,0,INT32_MAX,1}};
 const int64_t values[]={INT64_MIN,-2147483649LL,INT32_MIN,-1,0,1,3,17,INT32_MAX,2147483648LL,INT64_MAX};
 unsigned wide_cases=0;
 for(unsigned r=0;r<sizeof(rects)/sizeof(rects[0]);++r)
 for(unsigned a=0;a<11;++a)for(unsigned b=0;b<11;++b)
 for(unsigned c=0;c<11;++c)for(unsigned d=0;d<11;++d){
  assert(rect_intersects_bounds(rects[r],values[a],values[b],values[c],values[d])==
         oracle(rects[r],values[a],values[b],values[c],values[d]));
  assert(rect_inside_bounds(rects[r],values[a],values[b],values[c],values[d])==
         inside(rects[r],values[a],values[b],values[c],values[d]));++wide_cases;
 }
 for(unsigned a=0;a<sizeof(rects)/sizeof(rects[0]);++a)
 for(unsigned b=0;b<sizeof(rects)/sizeof(rects[0]);++b){
  Rect r=rects[a],s=rects[b];
  assert(rect_intersects(r,s)==oracle(r,s.x0,s.y0,s.x1,s.y1));
  assert(rect_intersects(r,s)==rect_intersects(s,r));
 }
 for(unsigned client=0;client<2;++client){
  compositor_window_view_t w={.x=0,.y=0,.width=10,.height=10,
   .flags=client?OS_WINDOW_CLIENT_DECORATIONS:0};
  check_window(w,(Rect){5,5,5,5});check_window(w,(Rect){6,6,4,4});
  unsigned width=10+(client?0:2),height=10+(client?0:2+WINDOW_TITLEBAR_HEIGHT);
  check_window(w,(Rect){width,0,width+1,1});
  check_window(w,(Rect){0,height,1,height+1});
  check_window(w,(Rect){width-1,height-1,width,height});
  w.width=w.height=0;check_window(w,(Rect){-1,-1,1,1});
  w=(compositor_window_view_t){.x=INT32_MAX-4,.y=-1,.width=16,.height=16,
   .flags=client?OS_WINDOW_CLIENT_DECORATIONS:0};
  check_window(w,(Rect){INT32_MAX-3,0,INT32_MAX,1});
 }
 /* 容器内部、边界、空区域和反向区域必须直接覆盖两条生产包含路径。 */
 const Rect containment_cases[]={{8,8,248,248},{9,40,247,247},{10,10,10,10},
  {14,14,12,12},{0,0,256,256},{-1,10,10,20},{8,8,9,9}};
 for(unsigned flags=0;flags<4;++flags)for(unsigned maximized=0;maximized<2;++maximized){
  compositor_window_view_t w={.width=256,.height=256,.maximized=maximized,
   .flags=(flags&1?OS_WINDOW_CLIENT_DECORATIONS:0)|(flags&2?OS_WINDOW_PREMULTIPLIED_ALPHA:0)};
  for(unsigned i=0;i<sizeof(containment_cases)/sizeof(*containment_cases);++i)
   check_window(w,containment_cases[i]);
 }
 for(unsigned trial=0;trial<50000;++trial){
  compositor_window_view_t w={.x=(int32_t)random_value(),.y=(int32_t)random_value(),
   .width=random_value()%(UINT32_MAX-64U),.height=random_value()%(UINT32_MAX-64U),
   .flags=(trial%2?OS_WINDOW_CLIENT_DECORATIONS:0)|(trial%3?0:OS_WINDOW_PREMULTIPLIED_ALPHA),
   .maximized=trial%5==0};
  Rect r={(int32_t)random_value(),(int32_t)random_value(),
          (int32_t)random_value(),(int32_t)random_value()};
  check_window(w,r);
 }

 /* 多区域的包围盒只作预筛选，不能填平未损坏的间隙。 */
 snapshot.damage_count=128;
 for(unsigned i=0;i<128;++i)snapshot.damage_rects[i]=(Rect){2*i,0,2*i+1,1};
 Rect union_bounds={0};assert(compositor_snapshot_damage_bounds(&snapshot,&union_bounds));
 compositor_window_view_t distant={.x=1000,.width=1,.height=1,.flags=OS_WINDOW_CLIENT_DECORATIONS};
 intersection_queries=0;
 assert(!compositor_view_intersects_snapshot_damage(&distant,&snapshot,&union_bounds));
 assert(intersection_queries==2);
 distant.x=1;intersection_queries=0;
 assert(!compositor_view_intersects_snapshot_damage(&distant,&snapshot,&union_bounds));
 assert(intersection_queries==129);
 distant.x=254;
 assert(compositor_view_intersects_snapshot_damage(&distant,&snapshot,&union_bounds));
 distant.x=0;intersection_queries=0;
 assert(compositor_view_intersects_snapshot_damage(&distant,&snapshot,&union_bounds));
 assert(intersection_queries==1);
 for(unsigned count=1;count<=4;++count){
  snapshot.damage_count=count;distant.x=1000;intersection_queries=0;
  assert(!compositor_view_intersects_snapshot_damage(&distant,&snapshot,&union_bounds));
  assert(intersection_queries==count);
 }

 for(unsigned trial=0;trial<2000;++trial){
  snapshot.damage_count=1+random_value()%128;
  compositor_window_view_t candidate={.x=(int)(random_value()%1500)-750,
   .y=(int)(random_value()%1500)-750,.width=random_value()%400,.height=random_value()%400,
   .flags=trial%2?OS_WINDOW_CLIENT_DECORATIONS:0};
  bool expected=false;
  for(unsigned i=0;i<snapshot.damage_count;++i){
   Rect *d=&snapshot.damage_rects[i];
   *d=(Rect){(int)(random_value()%1000)-500,(int)(random_value()%1000)-500,0,0};
   d->x1=d->x0+(int)(random_value()%200)-50;
   d->y1=d->y0+(int)(random_value()%200)-50;
   expected|=oracle(*d,candidate.x,candidate.y,
    (int64_t)candidate.x+window_outer_width(candidate.width,candidate.flags),
    (int64_t)candidate.y+window_outer_height(candidate.height,candidate.flags));
  }
  union_bounds=(Rect){0};(void)compositor_snapshot_damage_bounds(&snapshot,&union_bounds);
  assert(compositor_view_intersects_snapshot_damage(&candidate,&snapshot,&union_bounds)==expected);
 }

 /* 共用包围盒后遮挡下限与拖动窗口保留规则不变。 */
 union_bounds=(Rect){50,50,60,60};snapshot.window_count=3;
 for(unsigned i=0;i<3;++i)snapshot.windows[i]=(compositor_window_view_t){
  .identifier=10+i,.width=1000,.height=1000,.flags=OS_WINDOW_CLIENT_DECORATIONS};
 assert(compositor_snapshot_occlusion_floor(&snapshot,&union_bounds)==2);
 snapshot.windows[2].flags|=OS_WINDOW_PREMULTIPLIED_ALPHA;
 assert(compositor_snapshot_occlusion_floor(&snapshot,&union_bounds)==1);
 snapshot.dragging_identifier=10;
 assert(compositor_snapshot_occlusion_floor(&snapshot,&union_bounds)==0);
 snapshot.dragging_identifier=0;snapshot.windows[1].x=100;
 assert(compositor_snapshot_occlusion_floor(&snapshot,&union_bounds)==0);
 assert(compositor_snapshot_occlusion_floor(0,&union_bounds)==0);
 assert(compositor_snapshot_occlusion_floor(&snapshot,0)==0);
 assert(compositor_snapshot_occlusion_floor(&snapshot,&(Rect){0})==0);
 puts("Damage prefilter: distant window 128 -> 2 intersection checks; gaps and 2000 mixed-region oracle cases PASS");
 Rect r={0,0,1,1};compositor_window_view_t w={0};
 assert(!compositor_view_fully_covers_snapshot_bounds(0,&r));
 assert(!compositor_view_fully_covers_snapshot_bounds(&w,0));
 assert(!compositor_damage_inside_surface_interior(r,0));
 assert(!compositor_view_intersects_render_rect(0,&r));
 assert(!compositor_view_intersects_render_rect(&w,0));
 assert(!compositor_window_intersects_damage_locked(r,0));
 assert(!compositor_view_intersects_snapshot_damage(0,&snapshot,&r));
 assert(!compositor_view_intersects_snapshot_damage(&w,0,&r));
 assert(!compositor_view_intersects_snapshot_damage(&w,&snapshot,0));
 snapshot.damage_count=0;assert(!compositor_view_intersects_snapshot_damage(&w,&snapshot,&r));
 assert(!compositor_registry_window_intersects_damage_bounds_locked(0,&r));
 window_server_window_t live={0};
 assert(!compositor_registry_window_intersects_damage_bounds_locked(&live,0));
 printf("相交/包含规则：%u 组宽坐标、50000 组真实窗口，六条路径与独立 128 位参考一致\n",wide_cases);
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-intersection-') as directory:
    base = Path(directory)
    header = base / 'include/kernel/rect.h'
    header.parent.mkdir(parents=True)
    original = (root / 'include/kernel/rect.h').read_text()
    header.write_text(original)
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-I'+str(base/'include'), '-I'+str(root), '-I'+str(root/'include'),
               '-x', 'c', '-', str(root/'kernel/graphics/window_geometry.c'),
               '-o', str(base/'test')]
    subprocess.run(command, input=code, text=True, check=True)
    subprocess.run([str(base/'test')], check=True)
    for old, new in (
        ('!rect_is_empty(rect) && left < right && top < bottom &&', ''),
        ('right > rect.x0', '(int32_t)right > rect.x0'),
        ('(int64_t)rect.x1 <= right', '(int64_t)rect.x1 < right'),
    ):
        broken = original.replace(old, new)
        assert broken != original
        header.write_text(broken)
        subprocess.run(command, input=code, text=True, check=True)
        assert subprocess.run([str(base/'test')], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0))).returncode != 0
print('空区域误报、宽坐标截断和包含边界偏差反例均被拒绝')

with tempfile.TemporaryDirectory(prefix='damage-prefilter-negative-') as directory:
    binary=str(Path(directory)/'test')
    for name,old,new in (
        ('missing broad rejection','if (!rect_intersects_bounds(*bounds, left, top, right, bottom))',
         'if (false && !rect_intersects_bounds(*bounds, left, top, right, bottom))'),
        ('bounding box fills gaps','if (!rect_intersects_bounds(*bounds, left, top, right, bottom)) return false;',
         'return true;'),
    ):
        broken=code.replace(old,new)
        assert broken!=code
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                        '-I'+str(root),'-I'+str(root/'include'),'-x','c','-',
                        str(root/'kernel/graphics/window_geometry.c'),'-o',binary],
                       input=broken,text=True,check=True)
        result=subprocess.run([binary],capture_output=True,timeout=30,
            preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert result.returncode!=0 and b'Assertion' in result.stderr
        print('Negative control rejected: '+name)
