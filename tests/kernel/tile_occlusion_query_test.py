"""使用生产瓦片构建与查询，验证规划和回退共用遮挡数据。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
region = (root / 'kernel/graphics/compositor.c').read_text()
region = region[region.index('void compositor_region_locked('):]
fallback = (root / 'kernel/graphics/cursor_occlusion.c').read_text()
fallback = fallback[fallback.index('static bool compositor_window_fully_covers_damage('):fallback.index('void compositor_cursor_locked(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/graphics/tile_metadata.c"
#include "kernel/graphics/render_plan.c"
window_server_state_t g_window_server;
compositor_snapshot_t g_compositor_snapshot;
bool window_client_decorations(uint32_t flags){return (flags&OS_WINDOW_CLIENT_DECORATIONS)!=0;}
static unsigned geometry_checks;
uint32_t window_outer_width(uint32_t width,uint32_t flags){(void)flags;++geometry_checks;return width;}
uint32_t window_outer_height(uint32_t height,uint32_t flags){(void)flags;return height;}
uint32_t window_damage_tile_columns_locked(void){return (g_window_server.display_width+WINDOW_DAMAGE_TILE_SIZE-1)/WINDOW_DAMAGE_TILE_SIZE;}
uint32_t window_damage_tile_rows_locked(void){return (g_window_server.display_height+WINDOW_DAMAGE_TILE_SIZE-1)/WINDOW_DAMAGE_TILE_SIZE;}
uint64_t telemetry_timestamp(void){return 0;}
void kernel_perf_emit_scope_at(const char *name,uint64_t start,const char *file,uint32_t line){
 (void)name;(void)start;(void)file;(void)line;
}
static unsigned wallpaper_calls,layer_calls,menu_calls,cursor_calls;
static uint64_t drawn_windows;
void desktop_draw_wallpaper_locked(void){++wallpaper_calls;}
void compositor_animation_window(const compositor_window_view_t *w){
 unsigned i=(unsigned)(w-g_compositor_snapshot.windows);assert(i<64);drawn_windows|=1ULL<<i;
}
void compositor_animation_layers(void){++layer_calls;}
void desktop_draw_menu(void){++menu_calls;}
void compositor_cursor_locked(void){++cursor_calls;}
__FALLBACK__
bool compositor_window_intersects_damage_locked(const compositor_window_view_t *w){(void)w;return true;}
static uint32_t expected(uint64_t opaque,unsigned count){
 uint32_t result=UINT32_MAX;
 for(unsigned i=0;i<64;++i)if(opaque&(1ULL<<i))result=i;
 return result<count?result:UINT32_MAX;
}
__REGION__
static void verify_region(const compositor_render_span_t *span){
 wallpaper_calls=layer_calls=menu_calls=cursor_calls=0;drawn_windows=0;
 g_compositor_snapshot.damage_bounds=span->rect;
 compositor_region_locked(span);
 assert(wallpaper_calls==(span->first_window==UINT32_MAX));
 assert(drawn_windows==span->window_mask);
 assert(layer_calls==1&&menu_calls==1&&cursor_calls==1);
}
int main(void){
 g_window_server.display_width=128;g_window_server.display_height=128;
 g_compositor_snapshot.window_count=64;
 for(unsigned i=0;i<64;++i)g_compositor_snapshot.windows[i]=(compositor_window_view_t){
  .width=128,.height=128,.flags=OS_WINDOW_VISIBLE|OS_WINDOW_CLIENT_DECORATIONS,.maximized=true};
 assert(compositor_tile_metadata_build(&g_compositor_snapshot));
 assert(compositor_tile_occlusion_floor(0,0,64)==63);
 Rect top_bit_rect={10,10,30,30};
 assert(compositor_render_plan_exact_floor(&g_compositor_snapshot,&top_bit_rect)==63);
 g_compositor_snapshot.damage_count=1;
 g_compositor_snapshot.damage_rects[0]=(Rect){0,0,128,128};
 assert(compositor_build_render_plan(&g_compositor_snapshot));
 for(unsigned i=0;i<g_compositor_render_plan_count;++i){
  assert(g_compositor_render_plan[i].first_window==63);
  assert(g_compositor_render_plan[i].window_mask==(1ULL<<63));
 }
 g_compositor_snapshot.damage_bounds=(Rect){-20,-20,200,200};
 assert(compositor_occlusion_floor_for_damage()==63);
 /* 透明顶层不能遮挡；圆角恢复窗口也不能覆盖边缘瓦片。 */
 g_compositor_snapshot.windows[63].flags|=OS_WINDOW_PREMULTIPLIED_ALPHA;
 assert(compositor_tile_metadata_build(&g_compositor_snapshot));
 assert(compositor_occlusion_floor_for_damage()==62);
 assert(compositor_build_render_plan(&g_compositor_snapshot));
 for(unsigned i=0;i<g_compositor_render_plan_count;++i){
  assert(g_compositor_render_plan[i].first_window==62);
  assert(g_compositor_render_plan[i].window_mask==((1ULL<<63)|(1ULL<<62)));
 }
 for(unsigned i=0;i<63;++i)g_compositor_snapshot.windows[i].flags|=OS_WINDOW_PREMULTIPLIED_ALPHA;
 assert(compositor_tile_metadata_build(&g_compositor_snapshot));
 assert(compositor_occlusion_floor_for_damage()==UINT32_MAX);
 assert(compositor_build_render_plan(&g_compositor_snapshot));
 for(unsigned i=0;i<g_compositor_render_plan_count;++i){
  assert(g_compositor_render_plan[i].window_mask==UINT64_MAX);
 }
 g_compositor_snapshot.windows[63].flags&=~OS_WINDOW_PREMULTIPLIED_ALPHA;
 g_compositor_snapshot.windows[63].maximized=false;
 assert(compositor_tile_metadata_build(&g_compositor_snapshot));
 assert(compositor_occlusion_floor_for_damage()==UINT32_MAX);
 /* 独立的逐位参考实现覆盖最高位、越界索引及任意掩码组合。 */
 uint64_t seed=17;
 for(unsigned trial=0;trial<10000;++trial){
  seed=seed*6364136223846793005ULL+1;uint64_t opaque=seed;
  g_compositor_tile_meta[0].opaque_mask=opaque;
  for(unsigned count=0;count<=64;++count)
   assert(compositor_tile_occlusion_floor(0,0,count)==expected(opaque,count));
 }
 assert(compositor_tile_occlusion_floor(UINT32_MAX,UINT32_MAX,64)==UINT32_MAX);
 /* 某个瓦片没有遮挡下限时，整块回退绘制不能跳过底层。 */
 for(unsigned i=0;i<WINDOW_DAMAGE_TILE_COUNT;++i)
  g_compositor_tile_meta[i]=(compositor_tile_meta_t){.opaque_mask=2};
 g_compositor_tile_meta[1].opaque_mask=0;
 g_compositor_snapshot.damage_bounds=(Rect){0,0,128,64};
 assert(compositor_occlusion_floor_for_damage()==UINT32_MAX);
 g_compositor_tile_meta[1].opaque_mask=2;
 assert(compositor_occlusion_floor_for_damage()==1);
 g_compositor_tile_meta[0].opaque_mask=1;
 assert(compositor_occlusion_floor_for_damage()==0);
 const Rect empty[]={{0,0,0,1},{130,0,150,1},{-50,-50,-1,-1}};
 for(unsigned i=0;i<sizeof(empty)/sizeof(*empty);++i){
  g_compositor_snapshot.damage_bounds=empty[i];assert(compositor_occlusion_floor_for_damage()==UINT32_MAX);
 }
 /* 64 个窗口只有两个接触当前瓦片，不应再做 64 次几何检查。 */
 for(unsigned i=0;i<64;++i)g_compositor_snapshot.windows[i]=(compositor_window_view_t){
  .x=i<2?0:96,.y=i<2?0:96,.width=24,.height=24,
  .flags=OS_WINDOW_VISIBLE|OS_WINDOW_PREMULTIPLIED_ALPHA};
 assert(compositor_tile_metadata_build(&g_compositor_snapshot));
 compositor_render_span_t sparse={.rect={8,8,24,24},.first_window=UINT32_MAX};
 geometry_checks=0;
 assert(compositor_compile_render_span_mask(&g_compositor_snapshot,&sparse)==3);
 assert(geometry_checks==2);
 /* 精确遮挡只检查接触窗口；同瓦片内仍须核验圆角内接矩形。 */
 for(unsigned i=0;i<64;++i)g_compositor_snapshot.windows[i].flags=OS_WINDOW_VISIBLE;
 assert(compositor_tile_metadata_build(&g_compositor_snapshot));
 geometry_checks=0;
 assert(compositor_render_plan_exact_floor(&g_compositor_snapshot,&sparse.rect)==UINT32_MAX);
 assert(geometry_checks==2);
 Rect empty_tile={64,0,80,16};geometry_checks=0;
 assert(compositor_render_plan_exact_floor(&g_compositor_snapshot,&empty_tile)==UINT32_MAX);
 assert(geometry_checks==0);
 Rect covered={8,8,16,16};
 assert(compositor_render_plan_exact_floor(&g_compositor_snapshot,&covered)==1);
 /* 横向或纵向跨瓦片也只检查真正接触的两个窗口。 */
 const Rect multi[]={{8,8,80,24},{8,8,24,80}};
 for(unsigned i=0;i<2;++i){
  compositor_render_span_t span={.rect=multi[i],.first_window=UINT32_MAX};
  geometry_checks=0;
  assert(compositor_compile_render_span_mask(&g_compositor_snapshot,&span)==3);
  assert(geometry_checks==2);
  geometry_checks=0;
  assert(compositor_render_plan_exact_floor(&g_compositor_snapshot,&span.rect)==UINT32_MAX);
  assert(geometry_checks==2);
 }
 /* 已取得首瓦片后遇到缺失元数据，必须放弃整个并集并扫描全部窗口。 */
 g_compositor_tile_columns=1;
 compositor_render_span_t incomplete={.rect=multi[0],.first_window=UINT32_MAX};
 geometry_checks=0;
 assert(compositor_compile_render_span_mask(&g_compositor_snapshot,&incomplete)==3);
 assert(geometry_checks==64);
 assert(compositor_tile_metadata_build(&g_compositor_snapshot));
 /* 元数据失效必须恢复完整扫描，不能复用上一帧的空候选集。 */
 g_compositor_snapshot.animations=true;
 assert(!compositor_tile_metadata_build(&g_compositor_snapshot));
 g_compositor_snapshot.animations=false;geometry_checks=0;
 assert(compositor_render_plan_exact_floor(&g_compositor_snapshot,&sparse.rect)==UINT32_MAX);
 assert(geometry_checks==64);
 assert(compositor_tile_metadata_build(&g_compositor_snapshot));
 /* 随机实际几何与逐窗参考比较，涵盖单瓦片、多瓦片筛选和屏外回退。 */
 for(unsigned trial=0;trial<2500;++trial){
  seed=seed*6364136223846793005ULL+1;
  unsigned count=1+(seed%64);
  g_compositor_snapshot.window_count=count;
  for(unsigned i=0;i<count;++i){
   seed=seed*6364136223846793005ULL+1;
   g_compositor_snapshot.windows[i]=(compositor_window_view_t){
    .x=(int32_t)(seed%256)-64,.y=(int32_t)((seed>>16)%256)-64,
    .width=1+((seed>>32)%128),.height=1+((seed>>48)%128),
    .flags=OS_WINDOW_VISIBLE|((seed&8)?OS_WINDOW_PREMULTIPLIED_ALPHA:0)};
  }
  assert(compositor_tile_metadata_build(&g_compositor_snapshot));
  for(unsigned variant=0;variant<6;++variant){
   compositor_render_span_t span={.rect=variant==0?(Rect){8,8,24,24}:
    variant==1?(Rect){32,32,120,120}:variant==2?(Rect){-8,-8,24,24}:
    variant==3?(Rect){8,8,80,24}:variant==4?(Rect){8,8,24,80}:(Rect){0,0,INT32_MAX,INT32_MAX},
    .first_window=(trial%3)?(trial%(count+1)):UINT32_MAX};
   uint64_t reference=0;
   uint32_t exact_floor=UINT32_MAX;
   for(unsigned i=0;i<count;++i){
    compositor_window_view_t w=g_compositor_snapshot.windows[i];
    if(!(w.flags&OS_WINDOW_PREMULTIPLIED_ALPHA)&&w.width>16&&w.height>16&&
      (int64_t)w.x+8<=span.rect.x0&&(int64_t)w.y+8<=span.rect.y0&&
      (int64_t)w.x+w.width-8>=span.rect.x1&&
      (int64_t)w.y+w.height-8>=span.rect.y1)exact_floor=i;
   }
   assert(compositor_render_plan_exact_floor(&g_compositor_snapshot,&span.rect)==exact_floor);
   g_compositor_snapshot.damage_bounds=span.rect;
   assert(compositor_topmost_damage_cover()==exact_floor);
   unsigned first=span.first_window==UINT32_MAX?0:span.first_window;
   for(unsigned i=first;i<count;++i){
    const compositor_window_view_t *w=&g_compositor_snapshot.windows[i];
    if((int64_t)w->x+w->width>span.rect.x0&&
       (int64_t)w->y+w->height>span.rect.y0&&
       w->x<span.rect.x1&&w->y<span.rect.y1)reference|=1ULL<<i;
   }
   assert(compositor_compile_render_span_mask(&g_compositor_snapshot,&span)==reference);
  }
 }
 /* 最大化不透明客户区的边缘必须采用与瓦片判定一致的覆盖范围。 */
 g_compositor_snapshot.window_count=1;g_compositor_snapshot.damage_count=1;
 g_compositor_snapshot.windows[0]=(compositor_window_view_t){.x=4,.y=4,
  .width=120,.height=120,.maximized=true,.flags=OS_WINDOW_VISIBLE|OS_WINDOW_CLIENT_DECORATIONS};
 g_compositor_snapshot.damage_rects[0]=(Rect){4,4,12,12};
 assert(compositor_tile_metadata_build(&g_compositor_snapshot));
 assert(compositor_tile_occlusion_floor(0,0,1)==UINT32_MAX);
 assert(compositor_build_render_plan(&g_compositor_snapshot));
 assert(g_compositor_render_plan_count==1&&g_compositor_render_plan[0].first_window==0);
 verify_region(&g_compositor_render_plan[0]);
 for(unsigned variant=0;variant<3;++variant){
  g_compositor_snapshot.windows[0].maximized=variant!=0;
  g_compositor_snapshot.windows[0].flags=OS_WINDOW_VISIBLE|
    (variant!=2?OS_WINDOW_CLIENT_DECORATIONS:0)|
    (variant==1?OS_WINDOW_PREMULTIPLIED_ALPHA:0);
  assert(compositor_tile_metadata_build(&g_compositor_snapshot));
  assert(compositor_build_render_plan(&g_compositor_snapshot));
  assert(g_compositor_render_plan_count==1&&g_compositor_render_plan[0].first_window==UINT32_MAX);
  verify_region(&g_compositor_render_plan[0]);
 }
 /* 最大列数且遮挡交错：行缓冲恰好填满，下一行逐段垂直合并。 */
 g_window_server.display_width=8192;g_window_server.display_height=128;
 g_compositor_snapshot.window_count=64;g_compositor_snapshot.damage_count=1;
 g_compositor_snapshot.damage_rects[0]=(Rect){0,0,8192,128};
 for(unsigned i=0;i<64;++i)g_compositor_snapshot.windows[i]=(compositor_window_view_t){
  .x=i*128,.width=64,.height=128,.flags=OS_WINDOW_VISIBLE|OS_WINDOW_CLIENT_DECORATIONS,.maximized=true};
 assert(compositor_tile_metadata_build(&g_compositor_snapshot));
 assert(compositor_build_render_plan(&g_compositor_snapshot));
 assert(g_compositor_render_plan_count==128);
 for(unsigned i=0;i<128;++i){
  compositor_render_span_t p=g_compositor_render_plan[i];
  assert(p.rect.x0==(int)i*64&&p.rect.x1==(int)(i+1)*64&&p.rect.y0==0&&p.rect.y1==128);
  assert(p.first_window==((i&1)?UINT32_MAX:i/2));
  assert(p.window_mask==((i&1)?0:1ULL<<(i/2)));
 }
 /* 多行、多个损伤矩形反复复用行缓冲，校验合并后的覆盖和指令。 */
 static unsigned char expected_coverage[384*448],actual_coverage[384*448];
 g_window_server.display_width=384;g_window_server.display_height=448;
 g_compositor_snapshot.window_count=42;g_compositor_snapshot.damage_count=3;
 for(unsigned trial=0;trial<200;++trial){
  for(unsigned i=0;i<42;++i){
   seed=seed*6364136223846793005ULL+1;
   g_compositor_snapshot.windows[i]=(compositor_window_view_t){
    .x=(seed%6)*64,.y=((seed>>16)%7)*64,
    .width=(1+((seed>>32)%3))*64,.height=(1+((seed>>48)%3))*64,
    .flags=OS_WINDOW_VISIBLE|OS_WINDOW_CLIENT_DECORATIONS|
      ((seed&8)?OS_WINDOW_PREMULTIPLIED_ALPHA:0),.maximized=true};
  }
  assert(compositor_tile_metadata_build(&g_compositor_snapshot));
  memset(expected_coverage,0,sizeof(expected_coverage));
  memset(actual_coverage,0,sizeof(actual_coverage));
  for(unsigned d=0;d<3;++d){
   seed=seed*6364136223846793005ULL+1;
   Rect r=d==0?(Rect){0,0,384,448}:(Rect){
    (int32_t)(seed%512)-64,(int32_t)((seed>>16)%576)-64,
    (int32_t)((seed>>32)%512)-64,(int32_t)((seed>>48)%576)-64};
   g_compositor_snapshot.damage_rects[d]=r;
   for(int y=0;y<448;++y)for(int x=0;x<384;++x)
    expected_coverage[y*384+x]+=x>=r.x0&&x<r.x1&&y>=r.y0&&y<r.y1;
  }
  assert(compositor_build_render_plan(&g_compositor_snapshot));
  for(unsigned s=0;s<g_compositor_render_plan_count;++s){
   compositor_render_span_t p=g_compositor_render_plan[s];
   verify_region(&p);
   assert(p.rect.x0>=0&&p.rect.y0>=0&&p.rect.x0<p.rect.x1&&p.rect.y0<p.rect.y1&&
     p.rect.x1<=384&&p.rect.y1<=448);
   for(int y=p.rect.y0;y<p.rect.y1;++y)for(int x=p.rect.x0;x<p.rect.x1;++x)
    ++actual_coverage[y*384+x];
   unsigned first=p.first_window==UINT32_MAX?0:p.first_window;
   if(p.first_window!=UINT32_MAX){
    assert(first<42);
    compositor_window_view_t w=g_compositor_snapshot.windows[first];
    assert(!(w.flags&OS_WINDOW_PREMULTIPLIED_ALPHA));
    assert(w.x<=p.rect.x0&&w.y<=p.rect.y0&&
      (int64_t)w.x+w.width>=p.rect.x1&&(int64_t)w.y+w.height>=p.rect.y1);
   }
   uint64_t expected_mask=0;
   for(unsigned i=first;i<42;++i){
    compositor_window_view_t w=g_compositor_snapshot.windows[i];
    if(w.x<p.rect.x1&&w.y<p.rect.y1&&
      (int64_t)w.x+w.width>p.rect.x0&&(int64_t)w.y+w.height>p.rect.y0)
     expected_mask|=1ULL<<i;
   }
   assert(p.window_mask==expected_mask);
  }
  assert(memcmp(actual_coverage,expected_coverage,sizeof(actual_coverage))==0);
 }
 g_compositor_snapshot.damage_count=1;
 /* 非瓦片整数倍的屏幕也必须裁掉右下边缘。 */
 g_window_server.display_width=127;g_window_server.display_height=113;
 assert(compositor_tile_metadata_build(&g_compositor_snapshot));
 /* 规划必须覆盖损伤矩形的屏内部分，不能因负坐标丢失或伸出屏幕。 */
 const Rect edge_cases[]={{-20,-20,30,30},{100,100,200,200},
  {INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX},{-20,0,-1,20},
  {128,0,200,20},{20,20,20,30},{30,30,20,20}};
 for(unsigned trial=0;trial<507;++trial){
  seed=seed*6364136223846793005ULL+1;
  Rect r=trial<7?edge_cases[trial]:(Rect){
   (int32_t)(seed%512)-192,(int32_t)((seed>>16)%512)-192,
   (int32_t)((seed>>32)%512)-192,(int32_t)((seed>>48)%512)-192};
  g_compositor_snapshot.damage_rects[0]=r;
  bool built=compositor_build_render_plan(&g_compositor_snapshot);
  unsigned visible=0;
  for(unsigned i=0;i<g_compositor_render_plan_count;++i){
   Rect p=g_compositor_render_plan[i].rect;
   assert(p.x0>=0&&p.y0>=0&&p.x0<p.x1&&p.y0<p.y1&&p.x1<=127&&p.y1<=113);
  }
  for(int y=0;y<113;++y)for(int x=0;x<127;++x){
   bool expected_pixel=x>=r.x0&&x<r.x1&&y>=r.y0&&y<r.y1;
   unsigned hits=0;
   for(unsigned i=0;i<g_compositor_render_plan_count;++i){
    Rect p=g_compositor_render_plan[i].rect;
    hits+=x>=p.x0&&x<p.x1&&y>=p.y0&&y<p.y1;
   }
   assert(hits==(unsigned)expected_pixel);visible+=expected_pixel;
  }
  assert(built==(visible!=0));
 }
 g_compositor_snapshot.animations=true;
 assert(!compositor_tile_metadata_build(&g_compositor_snapshot));
 assert(!compositor_build_render_plan(&g_compositor_snapshot)&&!g_compositor_render_plan_valid);
 assert(compositor_tile_occlusion_floor(0,0,64)==UINT32_MAX);
 assert(compositor_occlusion_floor_for_damage()==UINT32_MAX);
 /* 规划和瓦片元数据不可用时，透明顶层不能跳过下层窗口或桌面。 */
 g_compositor_tile_meta_valid=false;
 g_compositor_snapshot.window_count=2;g_compositor_snapshot.animations=false;
 g_compositor_snapshot.damage_bounds=(Rect){32,32,64,64};
 for(unsigned i=0;i<2;++i)g_compositor_snapshot.windows[i]=(compositor_window_view_t){
  .width=128,.height=128,.flags=OS_WINDOW_VISIBLE|OS_WINDOW_PREMULTIPLIED_ALPHA};
 assert(compositor_topmost_damage_cover()==UINT32_MAX);
 wallpaper_calls=layer_calls=menu_calls=cursor_calls=0;drawn_windows=0;
 compositor_region_locked(0);
 assert(wallpaper_calls==1&&drawn_windows==3&&layer_calls==1&&menu_calls==1&&cursor_calls==1);
 g_compositor_snapshot.windows[0].flags=OS_WINDOW_VISIBLE;
 assert(compositor_topmost_damage_cover()==0);
 wallpaper_calls=0;drawn_windows=0;compositor_region_locked(0);
 assert(!wallpaper_calls&&drawn_windows==3);
 /* 最大化客户端外框是方角，损伤在边缘时也使用统一覆盖规则。 */
 g_compositor_snapshot.windows[0].flags=OS_WINDOW_VISIBLE|OS_WINDOW_CLIENT_DECORATIONS;
 g_compositor_snapshot.windows[0].maximized=true;
 g_compositor_snapshot.damage_bounds=(Rect){0,0,4,4};
 assert(compositor_topmost_damage_cover()==0);
 g_compositor_snapshot.windows[0].maximized=false;
 assert(compositor_topmost_damage_cover()==UINT32_MAX);
 g_compositor_snapshot.windows[0].maximized=true;
 g_compositor_snapshot.windows[0].flags&=~OS_WINDOW_VISIBLE;
 assert(compositor_topmost_damage_cover()==UINT32_MAX);
 assert(compositor_tile_self_test());
 puts("Tile occlusion: 650000 mask/count queries, real alpha/rounded coverage, 15000 span/exact-floor geometry comparisons, bounded multi-tile unions and partial-metadata fallback, 507 clipped coverage oracles, 200 multi-row/multi-damage plans, full 128-column capacity, actual planned/fallback region desktop/window dispatch, transparent fallback exclusion, shared maximized/rounded/alpha coverage, mixed region floors, clipping and animation invalidation PASS");
}
'''
program = program.replace('__REGION__', region).replace('__FALLBACK__',fallback)
with tempfile.TemporaryDirectory(prefix='tile-occlusion-') as directory:
    binary = Path(directory) / 'test'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-I' + str(root),
                    '-x', 'c', '-', '-o', str(binary)], input=program, text=True, check=True)
    subprocess.run([str(binary)], check=True)
