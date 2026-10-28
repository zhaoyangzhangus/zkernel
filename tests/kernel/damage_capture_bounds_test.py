"""验证瓦片扩大重绘范围后，窗口快照不会遗漏触及该范围的窗口。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kernel/graphics/damage.c"
#include "kernel/graphics/damage_plan.c"
window_server_state_t g_window_server;
atomic_bool g_window_dirty_hint;
atomic_uint_fast64_t g_window_dirty_generation;
atomic_uint_fast64_t g_window_dirty_notified_generation;
uint32_t window_outer_width(uint32_t width, uint32_t flags) {(void)flags; return width;}
uint32_t window_outer_height(uint32_t height, uint32_t flags) {(void)flags; return height;}
static compositor_snapshot_t snapshot;
static unsigned seed=391;
static unsigned random_value(void) { seed=seed*1664525U+1013904223U; return seed; }
static bool intersects(window_server_window_t *window, Rect r) {
 return (int64_t)window->x < r.x1 && (int64_t)window->x+window->width > r.x0 &&
        (int64_t)window->y < r.y1 && (int64_t)window->y+window->height > r.y0;
}
static void capture_check(unsigned count) {
 Rect bounds=compositor_registry_damage_bounds_locked();
 for(unsigned r=0; r<count; ++r) {
  Rect rect=snapshot.damage_rects[r];
  /* 覆盖每个重绘矩形的四边及四角，窗口可小到只重叠一个像素。 */
  int xs[]={rect.x0-1,rect.x0,rect.x0+1,rect.x1-1,rect.x1,rect.x1+1};
  int ys[]={rect.y0-1,rect.y0,rect.y0+1,rect.y1-1,rect.y1,rect.y1+1};
  for(unsigned i=0;i<6;i++) for(unsigned j=0;j<6;j++) {
   window_server_window_t window={.x=xs[i],.y=ys[j],.width=2,.height=2};
   if (intersects(&window,rect))
    assert(compositor_registry_window_intersects_damage_bounds_locked(&window,&bounds));
  }
 }
}
int main(void) {
 memset(&g_window_server,0,sizeof(g_window_server));
 g_window_server.display_width=1920; g_window_server.display_height=1080;
 window_damage_reset_locked();
 /* 第 17 个互不相交的矩形触发瓦片模式；左侧邻窗只落在瓦片扩展区。 */
 for(unsigned i=0;i<17;i++) window_mark_rect_locked(257+(int)i*4,257,1,1);
 assert(g_window_server.damage_tiles_active && !g_window_server.damage_full);
 window_server_window_t neighbor={.x=255,.y=260,.width=2,.height=2};
 Rect bounds=compositor_registry_damage_bounds_locked();
 assert(compositor_registry_window_intersects_damage_bounds_locked(&neighbor,&bounds));
 for(unsigned trial=0;trial<500;trial++) {
  memset(&g_window_server,0,sizeof(g_window_server));
  g_window_server.display_width=512+random_value()%1408;
  g_window_server.display_height=512+random_value()%568;
  window_damage_reset_locked();
  int x=64+(int)(random_value()%(g_window_server.display_width-320));
  int y=64+(int)(random_value()%(g_window_server.display_height-320));
  for(unsigned i=0;i<17;i++) window_mark_rect_locked(x+(int)i*3,y,1,1);
  assert(g_window_server.damage_tiles_active && !g_window_server.damage_full);
  for(unsigned i=0;i<40;i++)
   window_mark_rect_locked((int)(random_value()%g_window_server.display_width),
     (int)(random_value()%g_window_server.display_height),1+random_value()%8,1+random_value()%8);
  memcpy(snapshot.damage_tiles,g_window_server.damage_tiles,sizeof(snapshot.damage_tiles));
  const unsigned capacities[]={1,7,WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS};
  for(unsigned i=0;i<3;i++) {
   unsigned count=compositor_snapshot_tiles_to_rects(&snapshot,capacities[i]);
   assert(count>0 && count<=capacities[i]); capture_check(count);
  }
 }
 /* 矩形模式保持精确范围；全屏模式也应捕获屏幕内的窗口。 */
 memset(&g_window_server,0,sizeof(g_window_server));
 g_window_server.display_width=1920;g_window_server.display_height=1080;
 window_damage_reset_locked();window_mark_rect_locked(257,257,1,1);
 assert(!g_window_server.damage_tiles_active);
 bounds=compositor_registry_damage_bounds_locked();
 assert(!compositor_registry_window_intersects_damage_bounds_locked(&neighbor,&bounds));
 window_mark_dirty_locked();bounds=compositor_registry_damage_bounds_locked();
 assert(compositor_registry_window_intersects_damage_bounds_locked(&neighbor,&bounds));
 puts("Damage capture: tile edges, 500 random scenes, three rectangle capacities and exact/full bounds PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='damage-capture-') as directory:
    binary = Path(directory) / 'test'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-I' + str(root / 'include'), '-I' + str(root),
                    '-x', 'c', '-', '-o', str(binary)],
                   input=program, text=True, check=True)
    subprocess.run([str(binary)], check=True)
