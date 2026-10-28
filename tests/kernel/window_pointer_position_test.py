"""Differential-test the shared production pointer calculation at ABI limits."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/input_router.c').read_text()
begin = source.index('static bool window_route_relative_pointer_locked(')
end = source.index('static window_server_window_t *window_route_pointer_scene_locked(', begin)
helper = source[begin:end]
assert source.count('window_route_relative_pointer_locked(event)') == 1
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
enum {INPUT_REL_X=0, INPUT_REL_Y=1};
typedef struct {uint32_t code;int32_t value;} input_event_t;
static struct {uint32_t pointer_x,pointer_y,display_width,display_height;} g_window_server;
static unsigned updates;
static void window_input_set_pointer_locked(uint32_t x,uint32_t y) {
 ++updates;g_window_server.pointer_x=x;g_window_server.pointer_y=y;
}
''' + helper + r'''
static uint32_t oracle(uint32_t position,int32_t delta,uint32_t extent) {
 __int128 sum=(__int128)position+delta;
 if(!extent || sum<0)return 0;
 if(sum>=extent)return extent-1;
 return (uint32_t)sum;
}
static void check(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t axis,int32_t delta) {
 g_window_server.pointer_x=x;g_window_server.pointer_y=y;
 g_window_server.display_width=w;g_window_server.display_height=h;
 uint32_t next_x=axis==INPUT_REL_X?oracle(x,delta,w):x;
 uint32_t next_y=axis==INPUT_REL_Y?oracle(y,delta,h):y;
 input_event_t event={axis,delta};unsigned before=updates;
 bool moved=window_route_relative_pointer_locked(&event);
 assert(g_window_server.pointer_x==next_x && g_window_server.pointer_y==next_y);
 assert(moved==(next_x!=x || next_y!=y));
 assert(updates-before==(unsigned)moved);
}
int main(void) {
 const uint32_t positions[]={0,1,1079,1919,INT32_MAX,UINT32_MAX-1,UINT32_MAX};
 const uint32_t extents[]={0,1,1080,1920,INT32_MAX,UINT32_MAX};
 const int32_t deltas[]={INT32_MIN,-2500,-1,0,1,2500,INT32_MAX};
 unsigned cases=0;
 for(unsigned x=0;x<7;++x)for(unsigned y=0;y<7;++y)
 for(unsigned w=0;w<6;++w)for(unsigned h=0;h<6;++h)
 for(unsigned axis=0;axis<3;++axis)for(unsigned delta=0;delta<7;++delta) {
  check(positions[x],positions[y],extents[w],extents[h],axis,deltas[delta]);++cases;
 }
 uint32_t random=0xabcdef;
 for(unsigned i=0;i<200000;++i) {
  uint32_t values[6];for(unsigned j=0;j<6;++j){random=random*1664525U+1013904223U;values[j]=random;}
  check(values[0],values[1],values[2],values[3],values[4]%4,(int32_t)values[5]);++cases;
 }
 printf("Shared pointer positions: %u boundary/random cases; zero dimensions, saturation, unknown axes and no-op updates passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-pointer-position-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
