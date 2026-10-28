"""执行生产损伤瓦片合并，验证精确覆盖、边缘裁剪及容量不足时的保守回退。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/graphics/damage_plan.c"
window_server_state_t g_window_server;
uint32_t window_outer_width(uint32_t width,uint32_t flags){(void)flags;return width;}
uint32_t window_outer_height(uint32_t height,uint32_t flags){(void)flags;return height;}
bool compositor_view_fully_covers_snapshot_bounds(const compositor_window_view_t *w,const Rect *r){(void)w;(void)r;return false;}
uint32_t window_damage_tile_columns_locked(void){return (g_window_server.display_width+63)/64;}
uint32_t window_damage_tile_rows_locked(void){return (g_window_server.display_height+63)/64;}
static compositor_snapshot_t snapshot;
static unsigned char covered[WINDOW_DAMAGE_TILE_COUNT];
static uint64_t seed=91;
static uint32_t random_value(void){seed=seed*6364136223846793005ULL+1;return seed>>32;}
static void mark(unsigned x,unsigned y){unsigned i=y*WINDOW_DAMAGE_MAX_TILES_X+x;snapshot.damage_tiles[i/64]|=1ULL<<(i%64);}
static void verify(unsigned capacity,bool exact){
 memset(covered,0,sizeof(covered));
 unsigned count=compositor_snapshot_tiles_to_rects(&snapshot,capacity);
 assert(count<=capacity);
 unsigned columns=window_damage_tile_columns_locked(),rows=window_damage_tile_rows_locked();
 for(unsigned i=0;i<count;++i){
  Rect r=snapshot.damage_rects[i];
  assert(r.x0>=0&&r.y0>=0&&r.x0<r.x1&&r.y0<r.y1&&
    r.x1<=(int)g_window_server.display_width&&r.y1<=(int)g_window_server.display_height);
  assert(r.x0%64==0&&r.y0%64==0);
  assert(r.x1%64==0||r.x1==(int)g_window_server.display_width);
  assert(r.y1%64==0||r.y1==(int)g_window_server.display_height);
  for(unsigned y=r.y0/64;y<((unsigned)r.y1+63)/64;++y)
   for(unsigned x=r.x0/64;x<((unsigned)r.x1+63)/64;++x)
    covered[y*WINDOW_DAMAGE_MAX_TILES_X+x]=1;
 }
 for(unsigned y=0;y<rows;++y)for(unsigned x=0;x<columns;++x){
  unsigned i=y*WINDOW_DAMAGE_MAX_TILES_X+x;
  bool dirty=(snapshot.damage_tiles[i/64]>>(i%64))&1;
  assert(!dirty||covered[i]);
  if(exact)assert(covered[i]==dirty);
 }
}
int main(void){
 /* 任意起点、尾字及查找状态与独立逐位实现比较。 */
 for(unsigned trial=0;trial<10000;++trial){
  for(unsigned i=0;i<WINDOW_DAMAGE_TILE_WORDS;++i)
   snapshot.damage_tiles[i]=((uint64_t)random_value()<<32)|random_value();
  unsigned row=random_value()%128,limit=1+random_value()%128,start=random_value()%129;
  for(unsigned dirty=0;dirty<2;++dirty){
   unsigned expected=limit;
   for(unsigned x=start;x<limit;++x){
    unsigned bit=row*WINDOW_DAMAGE_MAX_TILES_X+x;
    if(((snapshot.damage_tiles[bit/64]>>(bit%64))&1)==dirty){expected=x;break;}
   }
   assert(compositor_snapshot_find_damage_bit(&snapshot,row,start,limit,dirty)==expected);
  }
 }
 const unsigned capacities[]={1,2,7,WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS};
 for(unsigned trial=0;trial<1000;++trial){
  g_window_server.display_width=1+random_value()%2048;
  g_window_server.display_height=1+random_value()%1536;
  memset(snapshot.damage_tiles,0,sizeof(snapshot.damage_tiles));
  unsigned columns=window_damage_tile_columns_locked(),rows=window_damage_tile_rows_locked();
  unsigned runs=0;
  for(unsigned y=0;y<rows;++y){
   bool previous=false;
   for(unsigned x=0;x<columns;++x){
    bool dirty=trial%5==0?false:trial%5==1?true:trial%5==2?(x+y)%2:random_value()%4==0;
    if(dirty){mark(x,y);runs+=!previous;}previous=dirty;
   }
  }
  for(unsigned i=0;i<4;++i)verify(capacities[i],capacities[i]>=runs);
 }
 /* 最大宽高和隔列条纹填满活动行缓冲，所有行仍应合并成 64 个矩形。 */
 g_window_server.display_width=8191;g_window_server.display_height=8191;
 memset(snapshot.damage_tiles,0,sizeof(snapshot.damage_tiles));
 for(unsigned y=0;y<128;++y)for(unsigned x=0;x<128;x+=2)mark(x,y);
 verify(WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS,true);
 assert(compositor_snapshot_tiles_to_rects(&snapshot,WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS)==64);
 for(unsigned i=0;i<64;++i){Rect r=snapshot.damage_rects[i];assert(r.x0==(int)i*128&&r.x1==(int)i*128+64&&r.y0==0&&r.y1==8191);}
 /* 空行必须截断纵向合并；较小容量只允许扩大重绘范围，不能漏掉脏瓦片。 */
 memset(snapshot.damage_tiles,0,sizeof(snapshot.damage_tiles));
 for(unsigned y=0;y<128;y+=2)for(unsigned x=0;x<128;x+=2)mark(x,y);
 verify(1,false);verify(7,false);verify(WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS,true);
 Rect saved=snapshot.damage_rects[0];
 assert(!compositor_snapshot_tiles_to_rects(&snapshot,0));
 assert(!memcmp(&saved,&snapshot.damage_rects[0],sizeof(saved)));
 assert(!compositor_snapshot_tiles_to_rects(0,1));
 puts("Damage tiles: 20000 word-scan oracles, 4000 sparse/full/checkerboard/clipped/capacity cases, 128-column full rows and gaps, no missed dirty tiles PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='damage-tiles-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-I' + str(root),
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True)
