"""执行真实遮挡与绘制计划，对照强制重建并验证缓存失效和失败恢复。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
tile = (root/'kernel/graphics/tile_metadata.c').read_text().replace(
    '#include "internal.h"', '#include "kernel/graphics/internal.h"')
plan = (root/'kernel/graphics/render_plan.c').read_text().replace(
    '#include "internal.h"', '#include "kernel/graphics/internal.h"')
marker = 'static void compositor_compile_render_plan_commands(\n    const compositor_snapshot_t *snapshot) {'
assert plan.count(marker) == 1
plan = plan.replace(marker, marker+'\n    ++builds;')
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned builds;
''' + tile + '\n' + plan + r'''
window_server_state_t g_window_server;
compositor_snapshot_t g_compositor_snapshot;
bool window_client_decorations(uint32_t f){return (f&OS_WINDOW_CLIENT_DECORATIONS)!=0;}
uint32_t window_outer_width(uint32_t w,uint32_t f){(void)f;return w;}
uint32_t window_outer_height(uint32_t h,uint32_t f){(void)f;return h;}
uint32_t window_damage_tile_columns_locked(void){return (g_window_server.display_width+31U)/32U;}
uint32_t window_damage_tile_rows_locked(void){return (g_window_server.display_height+31U)/32U;}
uint64_t telemetry_timestamp(void){return 0;}
void kernel_perf_emit_scope_at(const char *n,uint64_t s,const char *f,uint32_t l){
 (void)n;(void)s;(void)f;(void)l;
}
static compositor_snapshot_t s;
static compositor_render_span_t expected[WINDOW_RENDER_PLAN_MAX_SPANS];
static void verify(bool reuse){
 assert(compositor_tile_metadata_build(&s));
 compositor_render_plan_reset();
 assert(!g_compositor_render_plan_valid&&!g_compositor_render_plan_count);
 unsigned before=builds;
 assert(compositor_build_render_plan(&s));
 assert(builds-before==(reuse?0U:1U));
 unsigned count=g_compositor_render_plan_count;
 memcpy(expected,g_compositor_render_plan,count*sizeof(expected[0]));
 /* 独立输出数组保留上次结果，强制运行完整生产构建，不用缓存自身充当参考。 */
 g_compositor_plan_cache.valid=false;
 assert(compositor_build_render_plan(&s));
 assert(g_compositor_render_plan_valid&&g_compositor_render_plan_count==count);
 for(unsigned i=0;i<count;++i){
  assert(!memcmp(&expected[i].rect,&g_compositor_render_plan[i].rect,sizeof(Rect)));
  assert(expected[i].first_window==g_compositor_render_plan[i].first_window);
  assert(expected[i].window_mask==g_compositor_render_plan[i].window_mask);
 }
}
int main(void){
 g_window_server.display_width=512;g_window_server.display_height=512;
 s.window_count=2;s.damage_count=1;s.damage_rects[0]=(Rect){0,0,256,256};
 s.windows[0]=(compositor_window_view_t){.width=512,.height=512,.flags=OS_WINDOW_VISIBLE};
 s.windows[1]=(compositor_window_view_t){.x=32,.y=32,.width=160,.height=160,
  .flags=OS_WINDOW_VISIBLE};
 verify(false);
 unsigned before=builds;
 for(unsigned i=0;i<10000;++i){
  s.windows[1].surface_offset=(i&1U)*4096U;
  s.windows[1].present_sequence=i;
  compositor_render_plan_reset();
  assert(compositor_tile_metadata_build(&s)&&compositor_build_render_plan(&s));
 }
 assert(builds==before);
 verify(true);
 /* 每项几何键独立变化，仍须与强制重建逐字段相同。 */
 for(unsigned f=0;f<10;++f){
  if(f==0)++s.windows[1].x;
  if(f==1)++s.windows[1].y;
  if(f==2)++s.windows[1].width;
  if(f==3)++s.windows[1].height;
  if(f==4)s.windows[1].flags^=OS_WINDOW_PREMULTIPLIED_ALPHA;
  if(f==5)s.windows[1].maximized=!s.windows[1].maximized;
  if(f==6)++g_window_server.display_width;
  if(f==7)++g_window_server.display_height;
  if(f==8){s.windows[2]=s.windows[1];s.windows[2].x+=64;++s.window_count;}
  if(f==9){compositor_window_view_t t=s.windows[0];s.windows[0]=s.windows[1];s.windows[1]=t;}
  verify(false);verify(true);
 }
 for(unsigned f=0;f<5;++f){
  if(f==0)++s.damage_rects[0].x0;
  if(f==1)++s.damage_rects[0].y0;
  if(f==2)++s.damage_rects[0].x1;
  if(f==3)++s.damage_rects[0].y1;
  if(f==4){s.damage_rects[1]=(Rect){300,300,320,320};++s.damage_count;}
  verify(false);verify(true);
 }
 s.animations=true;
 assert(!compositor_tile_metadata_build(&s));
 assert(!compositor_build_render_plan(&s)&&!g_compositor_render_plan_valid&&
        !g_compositor_render_plan_count&&!g_compositor_plan_cache.valid);
 s.animations=false;verify(false);
 s.damage_count=0;
 assert(!compositor_build_render_plan(&s)&&!g_compositor_plan_cache.valid);
 s.damage_count=1;verify(false);
 s.damage_count=WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS+1U;
 assert(!compositor_build_render_plan(&s)&&!g_compositor_plan_cache.valid);
 s.damage_count=1;verify(false);
 /* 部分计划覆写后容量不足，恢复旧更新区域也必须重新生成，不能复用半成品。 */
 for(unsigned i=0;i<WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS;++i)s.damage_rects[i]=(Rect){0,0,256,256};
 s.damage_count=WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS;
 assert(compositor_tile_metadata_build(&s));
 assert(!compositor_build_render_plan(&s)&&!g_compositor_plan_cache.valid&&!g_compositor_render_plan_valid);
 s.damage_count=1;verify(false);verify(true);
 unsigned seed=123;
 for(unsigned i=0;i<300;++i){
  seed=seed*1664525U+1013904223U;
  s.windows[0].x=(int)(seed%512U)-128;
  s.windows[1].y=(int)((seed>>8U)%512U)-128;
  s.damage_rects[0]=(Rect){(int)(seed%64U),(int)((seed>>8U)%64U),256,256};
  verify(false);verify(true);
 }
 puts("绘制计划缓存：10000 次内容更新无需重建，几何/层级/屏幕/更新区域/动画独立失效，容量失败恢复与 300 组完整重建对照通过");
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-plan-cache-') as directory:
    work=Path(directory)
    fixture,binary=work/'test.c',work/'test'
    command=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-pie','-no-pie',
             '-I'+str(root/'include'),'-I'+str(root),str(fixture),'-o',str(binary)]
    fixture.write_text(program)
    subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True,timeout=30)
    for key in ['g_compositor_plan_cache.revision == revision',
                'memcmp(g_compositor_plan_cache.damage, snapshot->damage_rects, damage_bytes) == 0']:
        assert program.count(key)==1
        fixture.write_text(program.replace(key,'true'))
        subprocess.run(command,check=True)
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE,(0,0))
        result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=30,preexec_fn=no_core)
        assert result.returncode!=0 and 'Assertion' in result.stderr,result.stderr
print('负面对照通过：忽略几何修订号或更新矩形比较均被拒绝')
