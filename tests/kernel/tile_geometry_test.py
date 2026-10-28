"""Compare production tile masks against the previous full-screen algorithm."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/graphics/tile_metadata.c').read_text()
implementation = source.split('static uint32_t tile_count_for_extent', 1)[1].split(
    'bool compositor_tile_metadata_build(', 1)[0]
implementation = 'static uint32_t tile_count_for_extent' + implementation
implementation = implementation.replace(
    '*left = view->x;', '++bounds_calls; *left = view->x;')
reference = r'''
static bool tile_is_covered(int64_t left, int64_t top,
                            int64_t right, int64_t bottom,
                            int64_t tile_left, int64_t tile_top,
                            int64_t tile_right, int64_t tile_bottom) {
    return left <= tile_left && top <= tile_top &&
           right >= tile_right && bottom >= tile_bottom;
}

static bool reference_build(
    const compositor_snapshot_t *snapshot,
    uint32_t display_width,
    uint32_t display_height,
    legacy_tile_meta_t *metadata,
    uint32_t *out_columns,
    uint32_t *out_rows) {
    uint32_t columns;
    uint32_t rows;

    if (snapshot == 0 || metadata == 0 || out_columns == 0 ||
        out_rows == 0 || snapshot->window_count > WINDOW_SERVER_MAX_WINDOWS) {
        return false;
    }

    columns =
        tile_count_for_extent(
            display_width,
            WINDOW_DAMAGE_MAX_TILES_X);

    rows =
        tile_count_for_extent(
            display_height,
            WINDOW_DAMAGE_MAX_TILES_Y);

    if (columns == 0U || rows == 0U) return false;

    for (uint32_t tile_y = 0U; tile_y < rows; ++tile_y) {
        for (uint32_t tile_x = 0U; tile_x < columns; ++tile_x) {
            uint32_t index =
                tile_y * WINDOW_DAMAGE_MAX_TILES_X + tile_x;

            metadata[index] =
                (legacy_tile_meta_t){0};

            int64_t tile_left =
                (int64_t)tile_x * WINDOW_DAMAGE_TILE_SIZE;
            int64_t tile_top =
                (int64_t)tile_y * WINDOW_DAMAGE_TILE_SIZE;
            int64_t tile_right =
                tile_left + WINDOW_DAMAGE_TILE_SIZE;
            int64_t tile_bottom =
                tile_top + WINDOW_DAMAGE_TILE_SIZE;

            if (tile_right > (int64_t)display_width) {
                tile_right = display_width;
            }

            if (tile_bottom > (int64_t)display_height) {
                tile_bottom = display_height;
            }

            for (uint32_t window_index = 0U;
                 window_index < snapshot->window_count;
                 ++window_index) {
                const compositor_window_view_t *view =
                    &snapshot->windows[window_index];
                int64_t left;
                int64_t top;
                int64_t right;
                int64_t bottom;
                uint64_t bit = 1ULL << window_index;

                if ((view->flags & OS_WINDOW_VISIBLE) == 0U ||
                    !tile_view_bounds(
                        view,
                        &left,
                        &top,
                        &right,
                        &bottom) ||
                    !tile_intersects(
                        left,
                        top,
                        right,
                        bottom,
                        tile_left,
                        tile_top,
                        tile_right,
                        tile_bottom)) {
                    continue;
                }

                metadata[index].touch_mask |= bit;

                if (tile_is_covered(
                        left,
                        top,
                        right,
                        bottom,
                        tile_left,
                        tile_top,
                        tile_right,
                        tile_bottom)) {
                    metadata[index].full_mask |= bit;
                }

                if (compositor_view_opaque_bounds(
                        view,
                        &left,
                        &top,
                        &right,
                        &bottom) &&
                    tile_is_covered(
                        left,
                        top,
                        right,
                        bottom,
                        tile_left,
                        tile_top,
                        tile_right,
                        tile_bottom)) {
                    metadata[index].opaque_mask |= bit;
                }
            }
        }
    }

    *out_columns = columns;
    *out_rows = rows;
    return true;
}
'''
harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#define WINDOW_DAMAGE_TILE_SIZE 64U
#define WINDOW_DAMAGE_MAX_TILES_X 32U
#define WINDOW_DAMAGE_MAX_TILES_Y 20U
#define WINDOW_SERVER_MAX_WINDOWS 64U
#define WINDOW_CORNER_RADIUS 8U
#define OS_WINDOW_VISIBLE 1U
#define OS_WINDOW_CLIENT_DECORATIONS 2U
#define OS_WINDOW_PREMULTIPLIED_ALPHA 4U
#define CAPTION 8U
typedef struct {
 int32_t x,y; uint32_t width,height,flags; bool maximized;
} compositor_window_view_t;
typedef struct {
 uint32_t window_count; compositor_window_view_t windows[64];
} compositor_snapshot_t;
typedef struct {uint64_t touch_mask,opaque_mask;} compositor_tile_meta_t;
typedef struct {uint64_t touch_mask,full_mask,opaque_mask;} legacy_tile_meta_t;
static uint64_t bounds_calls;
static bool window_client_decorations(uint32_t f){return (f&2)!=0;}
static uint32_t window_outer_width(uint32_t w,uint32_t f){return w+((f&2)?0:2);}
static uint32_t window_outer_height(uint32_t h,uint32_t f){return h+((f&2)?0:32);}
static uint64_t telemetry_timestamp(void){return 0;}
static void kernel_perf_emit_scope(const char *name,uint64_t start){(void)name;(void)start;}
''' + implementation + source[source.index('bool compositor_tile_self_test('):source.index('/* 规划与回退绘制共用瓦片遮挡下限')] + reference + r'''
static uint32_t seed=17;
static uint32_t next(void){seed=seed*1664525U+1013904223U;return seed>>8;}
int main(void){
 assert(compositor_tile_self_test()); // Execute the actual boot gate too.
 compositor_snapshot_t snapshot={0};
 compositor_tile_meta_t actual[640];legacy_tile_meta_t expected[640];
 uint64_t old_calls=0,new_calls=0;
 for(unsigned trial=0;trial<10000;++trial){
  uint32_t width=1+next()%2500,height=1+next()%1600;
  uint32_t ac=0,ar=0,ec=0,er=0;
  snapshot.window_count=trial%65;
  for(unsigned i=0;i<snapshot.window_count;++i){
   snapshot.windows[i]=(compositor_window_view_t){
    .x=(int32_t)(next()%3000)-1000,.y=(int32_t)(next()%2400)-800,
    .width=next()%1400,.height=next()%1000,.flags=next()%16,
    .maximized=(next()&1)!=0};
  }
  if(trial<65){
   width=1920;height=1080;
   for(unsigned i=0;i<snapshot.window_count;++i)
    snapshot.windows[i]=(compositor_window_view_t){-32,63,800,600,3,false};
  }
  memset(actual,0xa5,sizeof(actual));memset(expected,0xa5,sizeof(expected));
  bounds_calls=0;
  assert(tile_metadata_build_into(&snapshot,width,height,actual,&ac,&ar));
  new_calls+=bounds_calls;
  assert(bounds_calls<=snapshot.window_count);
  bounds_calls=0;
  assert(reference_build(&snapshot,width,height,expected,&ec,&er));
  old_calls+=bounds_calls;
  assert(ac==ec&&ar==er);
  for(unsigned i=0;i<640;++i){
   assert(actual[i].touch_mask==expected[i].touch_mask);
   assert(actual[i].opaque_mask==expected[i].opaque_mask);
   /* 旧算法的不透明覆盖始终包含完整外框覆盖，删除后者不改变遮挡。 */
   assert(!(expected[i].opaque_mask&~expected[i].full_mask));
  }
 }
 /* 65×65 屏幕的右下角只有一个像素，仍须按实际边界判断完整覆盖。 */
 const struct { compositor_window_view_t view; unsigned opaque; } edges[]={
  {{0,0,65,65,3,true},15},{{0,1,65,64,3,true},12},
  {{1,0,64,65,3,true},10},{{0,0,65,64,3,true},3},
  {{0,0,64,65,3,true},5},{{0,0,65,65,7,true},0},
  {{0,0,65,65,3,false},0},{{-8,-8,81,81,3,false},15},
  {{-8,-8,79,49,1,false},15},
 };
 snapshot.window_count=1;
 for(unsigned i=0;i<sizeof(edges)/sizeof(edges[0]);++i){
  uint32_t c=0,r=0;unsigned mask=0;
  snapshot.windows[0]=edges[i].view;
  assert(tile_metadata_build_into(&snapshot,65,65,actual,&c,&r)&&c==2&&r==2);
  for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)
   if(actual[y*32+x].opaque_mask)mask|=1U<<(y*2+x);
  assert(mask==edges[i].opaque);
 }
 uint32_t c=0,r=0;
 assert(!tile_metadata_build_into(0,1920,1080,actual,&c,&r));
 snapshot.window_count=65;
 assert(!tile_metadata_build_into(&snapshot,1920,1080,actual,&c,&r));
 snapshot.window_count=1;
 assert(!tile_metadata_build_into(&snapshot,0,1080,actual,&c,&r));
 snapshot.windows[0]=(compositor_window_view_t){INT32_MIN,INT32_MAX,800,600,3,false};
 assert(tile_metadata_build_into(&snapshot,1920,1080,actual,&c,&r));
 for(unsigned y=0;y<r;++y)for(unsigned x=0;x<c;++x)
  assert(!actual[y*32+x].touch_mask);
 printf("Tile geometry: 9 partial-edge cases, 10000 equivalent frames; bounds calls %llu -> %llu\n",
        (unsigned long long)old_calls,(unsigned long long)new_calls);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-tile-geometry-') as directory:
    base=Path(directory)
    fixture,binary=base/'test.c',base/'test'
    fixture.write_text(harness)
    subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(fixture),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
