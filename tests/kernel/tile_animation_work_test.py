"""动画跳过无消费者的瓦片计算，且不能向后续帧泄露旧结果。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/tile_metadata.c').read_text()
body = source[source.index('bool compositor_tile_metadata_build('):
              source.index('bool compositor_tile_self_test(')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define WINDOW_DAMAGE_MAX_TILES_X 8U
typedef struct {bool animations; unsigned generation;} compositor_snapshot_t;
typedef struct {unsigned generation;} compositor_tile_meta_t;
static compositor_tile_meta_t g_compositor_tile_meta[64];
static unsigned g_compositor_tile_columns,g_compositor_tile_rows;
static bool g_compositor_tile_meta_valid;
static struct {unsigned display_width,display_height;} g_window_server={1920,1080};
static unsigned builds;
static bool fail;
static bool tile_metadata_build_into(const compositor_snapshot_t *snapshot,
 unsigned width,unsigned height,compositor_tile_meta_t *tiles,unsigned *columns,unsigned *rows){
 ++builds;assert(width==1920&&height==1080);
 if(!snapshot||fail)return false;
 *columns=*rows=8;
 for(unsigned i=0;i<64;++i)tiles[i].generation=snapshot->generation;
 return true;
}
''' + body + r'''
int main(void){
 compositor_snapshot_t snapshot={0};
 for(unsigned round=1;round<=1000;++round){
  snapshot.animations=false;snapshot.generation=round;
  unsigned before=builds;
  assert(compositor_tile_metadata_build(&snapshot)&&builds==before+1);
  assert(compositor_tile_metadata_valid());
  compositor_tile_meta_t previous[64];memcpy(previous,g_compositor_tile_meta,sizeof(previous));
  for(unsigned frame=0;frame<20;++frame){
   snapshot.animations=true;
   assert(!compositor_tile_metadata_build(&snapshot)&&builds==before+1);
   assert(!compositor_tile_metadata_valid()&&!g_compositor_tile_columns&&!g_compositor_tile_rows);
   assert(!compositor_tile_metadata_at(0,0)&&!compositor_tile_metadata_at(UINT32_MAX,UINT32_MAX));
   assert(!memcmp(previous,g_compositor_tile_meta,sizeof(previous)));
  }
  snapshot.animations=false;++snapshot.generation;
  assert(compositor_tile_metadata_build(&snapshot)&&builds==before+2);
  assert(compositor_tile_metadata_at(7,7)->generation==round+1);
  assert(!compositor_tile_metadata_at(8,0));
  fail=true;assert(!compositor_tile_metadata_build(&snapshot));fail=false;
  assert(!compositor_tile_metadata_valid()&&!compositor_tile_metadata_at(0,0));
 }
 assert(!compositor_tile_metadata_build(0));
 puts("Animation tile work: 20000 frames, zero tile builds/writes; 1000 exit/rebuild/failure transitions PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='tile-animation-work-') as directory:
    work = Path(directory)
    fixture, binary = work / 'test.c', work / 'test'
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               str(fixture), '-o', str(binary)]
    fixture.write_text(program)
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
    fixture.write_text(program.replace('snapshot != 0 && snapshot->animations', 'false'))
    subprocess.run(command, check=True)
    assert subprocess.run([str(binary)], stderr=subprocess.DEVNULL).returncode != 0
    print('负面对照通过：动画重新执行瓦片构建时，工作量断言拒绝该实现')
