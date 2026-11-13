"""对照完整重算，验证几何复用、动画跳过与所有失效条件。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/tile_metadata.c').read_text()
source = source.replace('#include "internal.h"', '#include "kernel/graphics/internal.h"')
# 仅在测试中记录实际完整构建次数，像素和掩码仍使用未替换的生产计算。
marker = '    uint32_t columns = tile_count_for_extent(display_width, WINDOW_DAMAGE_MAX_TILES_X);'
assert source.count(marker) == 1
source = source.replace(marker, '    ++builds;\n' + marker)
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned builds;
__SOURCE__
window_server_state_t g_window_server;
compositor_snapshot_t g_compositor_snapshot;
bool window_client_decorations(uint32_t flags){return (flags&OS_WINDOW_CLIENT_DECORATIONS)!=0;}
uint32_t window_outer_width(uint32_t width,uint32_t flags){(void)flags;return width;}
uint32_t window_outer_height(uint32_t height,uint32_t flags){(void)flags;return height;}
uint64_t telemetry_timestamp(void){return 0;}
void kernel_perf_emit_scope_at(const char *name,uint64_t start,const char *file,uint32_t line){
 (void)name;(void)start;(void)file;(void)line;
}
static compositor_snapshot_t snapshot;
static compositor_tile_meta_t reference[WINDOW_DAMAGE_TILE_COUNT],previous[WINDOW_DAMAGE_TILE_COUNT];
static void verify(void){
 unsigned before=builds,columns=0,rows=0;
 assert(compositor_tile_metadata_build(&snapshot));
 unsigned built=builds-before;
 assert(tile_metadata_build_into(&snapshot,g_window_server.display_width,
  g_window_server.display_height,reference,&columns,&rows));
 assert(compositor_tile_metadata_valid());
 assert(columns==g_compositor_tile_columns&&rows==g_compositor_tile_rows);
 for(unsigned y=0;y<rows;++y)for(unsigned x=0;x<columns;++x){
  unsigned i=y*WINDOW_DAMAGE_MAX_TILES_X+x;
  assert(g_compositor_tile_meta[i].touch_mask==reference[i].touch_mask);
  assert(g_compositor_tile_meta[i].opaque_mask==reference[i].opaque_mask);
 }
 /* 完整对照重算写入独立数组，不能破坏或刷新生产缓存。 */
 before=builds;
 for(unsigned i=0;i<20;++i)assert(compositor_tile_metadata_build(&snapshot));
 assert(builds==before);
 assert(built<=1);
}
static unsigned seed=123;
static unsigned next(void){seed=seed*1664525U+1013904223U;return seed;}
int main(void){
 g_window_server.display_width=1920;g_window_server.display_height=1080;
 snapshot.window_count=1;
 snapshot.windows[0]=(compositor_window_view_t){.x=64,.y=48,.width=800,.height=600,
  .flags=OS_WINDOW_VISIBLE|OS_WINDOW_CLIENT_DECORATIONS};
 verify();
 /* 同一几何的 10000 次像素提交不应再次构建遮挡表。 */
 unsigned before=builds;
 for(unsigned i=0;i<10000;++i){
  snapshot.windows[0].surface_offset=(i&1)*4096;
  snapshot.damage_count=1;
  snapshot.damage_rects[0]=(Rect){i%100,i%50,900,700};
  assert(compositor_tile_metadata_build(&snapshot));
 }
 assert(builds==before);
 /* 每个几何字段都必须单独触发失效，不能被之后的其他变化掩盖。 */
 for(unsigned field=0;field<6;++field){
  compositor_window_view_t *v=&snapshot.windows[0];
  before=builds;
  if(field==0)v->x+=64;
  if(field==1)v->y+=64;
  if(field==2)v->width+=64;
  if(field==3)v->height+=64;
  if(field==4)v->flags^=OS_WINDOW_PREMULTIPLIED_ALPHA;
  if(field==5)v->maximized=!v->maximized;
  assert(compositor_tile_metadata_build(&snapshot)&&builds==before+1);
  verify();
 }
 /* 屏幕尺寸和窗口数量独立变化，同样必须失效，不能只由随机布局间接覆盖。 */
 for(unsigned field=0;field<3;++field){
  before=builds;
  if(field==0)++g_window_server.display_width;
  if(field==1)++g_window_server.display_height;
  if(field==2){snapshot.windows[1]=snapshot.windows[0];snapshot.windows[1].x+=128;snapshot.window_count=2;}
  assert(compositor_tile_metadata_build(&snapshot)&&builds==before+1);
  verify();
 }
 compositor_window_view_t swapped=snapshot.windows[0];
 snapshot.windows[0]=snapshot.windows[1];snapshot.windows[1]=swapped;before=builds;
 assert(compositor_tile_metadata_build(&snapshot)&&builds==before+1);verify();
 --snapshot.window_count;before=builds;
 assert(compositor_tile_metadata_build(&snapshot)&&builds==before+1);verify();
 /* 同样的窗口换顺序、增删、透明/圆角变化、屏外裁剪和不同屏幕尺寸。 */
 for(unsigned round=0;round<1000;++round){
  unsigned count=next()%65;snapshot.window_count=count;
  for(unsigned i=0;i<count;++i)snapshot.windows[i]=(compositor_window_view_t){
   .x=(int)(next()%2400)-400,.y=(int)(next()%1500)-300,
   .width=next()%1600,.height=next()%1000,
   .flags=(next()&1?OS_WINDOW_VISIBLE:0)|(next()&1?OS_WINDOW_CLIENT_DECORATIONS:0)|
          (next()&1?OS_WINDOW_PREMULTIPLIED_ALPHA:0),.maximized=(next()&1)!=0};
  g_window_server.display_width=1+next()%3840;
  g_window_server.display_height=1+next()%2160;
  verify();
  if(count>1){compositor_window_view_t a=snapshot.windows[0];
   snapshot.windows[0]=snapshot.windows[count-1];snapshot.windows[count-1]=a;verify();}
  memcpy(previous,g_compositor_tile_meta,sizeof(previous));before=builds;
  for(unsigned i=0;i<20;++i){
   snapshot.animations=true;
   assert(!compositor_tile_metadata_build(&snapshot)&&builds==before);
   assert(!compositor_tile_metadata_valid()&&!g_compositor_tile_columns&&!g_compositor_tile_rows);
   assert(!compositor_tile_metadata_at(0,0)&&!compositor_tile_metadata_at(UINT32_MAX,UINT32_MAX));
   assert(!memcmp(previous,g_compositor_tile_meta,sizeof(previous)));
  }
  snapshot.animations=false;
  assert(compositor_tile_metadata_build(&snapshot)&&builds==before+1);
  verify();
  /* 失败后原有缓存必须作废；恢复原尺寸也必须重新构建。 */
  unsigned width=g_window_server.display_width;g_window_server.display_width=0;
  assert(!compositor_tile_metadata_build(&snapshot));
  assert(!compositor_tile_metadata_valid()&&!compositor_tile_metadata_at(0,0));
  g_window_server.display_width=width;before=builds;
  assert(compositor_tile_metadata_build(&snapshot)&&builds==before+1);
  verify();
 }
 assert(!compositor_tile_metadata_build(0)&&!compositor_tile_metadata_valid());
 snapshot.window_count=WINDOW_SERVER_MAX_WINDOWS+1;
 assert(!compositor_tile_metadata_build(&snapshot)&&!compositor_tile_metadata_valid());
 snapshot.window_count=0;verify();
 puts("Tile geometry reuse: 10000 content-only frames, six isolated field invalidations, 1000 randomized layouts/order/display/animation/failure transitions match full production rebuild; 20000 animation frames write no tiles PASS");
}
'''.replace('__SOURCE__', source)
with tempfile.TemporaryDirectory(prefix='tile-animation-work-') as directory:
    work = Path(directory)
    fixture, binary = work / 'test.c', work / 'test'
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I'+str(root/'include'), '-I'+str(root),
               str(fixture), '-o', str(binary)]
    fixture.write_text(program)
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
    # 去掉任一字段比较后，其独立失效断言必须失败。
    for field in ['x', 'y', 'width', 'height', 'flags', 'maximized']:
        comparison = f'view->{field} != saved->{field}'
        assert program.count(comparison) == 1
        fixture.write_text(program.replace(comparison, 'false'))
        subprocess.run(command, check=True)
        assert subprocess.run([str(binary)], stderr=subprocess.DEVNULL, timeout=30).returncode != 0
    fixture.write_text(program.replace('snapshot != 0 && snapshot->animations', 'false'))
    subprocess.run(command, check=True)
    assert subprocess.run([str(binary)], stderr=subprocess.DEVNULL, timeout=30).returncode != 0
print('负面对照通过：遗漏六个几何字段任一个、或动画时重建，均被拒绝')
