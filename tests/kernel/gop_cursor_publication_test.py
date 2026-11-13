"""Production publication keeps the cursor in each visible GOP scanline."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/present_cursor.c').read_text()
bounds = source[source.index('bool window_present_cursor_bounds('):source.index('/* 先发布包围盒')]
function = bounds + source[source.index('bool window_present_cursor_publish_span('):
                  source.index('void compositor_present_cursor_direct(')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <kernel/rect.h>
#define WINDOW_CURSOR_WIDTH 4
#define WINDOW_CURSOR_HEIGHT 4
static struct {bool presented_pointer_valid;unsigned display_height,display_width;
 unsigned presented_pointer_x,presented_pointer_y,presented_cursor_shape;}g_window_server;
static int window_cursor_hotspot_x(unsigned s){(void)s;return 1;}
static int window_cursor_hotspot_y(unsigned s){(void)s;return 1;}
static void display_core_publish_xrgb8888_span(volatile unsigned *d,const unsigned *s,unsigned n){
 for(unsigned i=0;i<n;++i)d[i]=s[i];
}
''' + function + r'''
int main(void){
 g_window_server.display_width=g_window_server.display_height=8;
 for(unsigned valid=0;valid<2;++valid)
 for(unsigned py=0;py<11;++py)for(unsigned px=0;px<11;++px)
 for(unsigned y=0;y<9;++y)for(unsigned x=0;x<9;++x)for(unsigned n=0;n<=8-x;++n){
  g_window_server.presented_pointer_valid=valid;
  g_window_server.presented_pointer_x=px;g_window_server.presented_pointer_y=py;
  unsigned src[8],dst[10];for(unsigned i=0;i<8;++i)src[i]=i+1;
  for(unsigned i=0;i<10;++i)dst[i]=0xdeadbeef;
  int left=(int)px-1,top=(int)py-1;
  bool intersects=false;
  for(unsigned i=0;i<n;++i)intersects|=(int)(x+i)>=left && (int)(x+i)<left+4 &&
    (int)y>=top && (int)y<top+4;
  bool expected=valid && n && y<8 && intersects;
  Rect captured;bool visible=window_present_cursor_bounds(&captured);
  /* 捕获后的跨度只能依赖显式包围盒，实时光标状态变化不能改变本次复制。 */
  g_window_server.presented_pointer_valid=false;
  g_window_server.presented_pointer_x=g_window_server.presented_pointer_y=999;
  assert(window_present_cursor_publish_span(dst+1,src,x,y,n,visible?&captured:0)==expected);
  assert(dst[0]==0xdeadbeef && dst[9]==0xdeadbeef);
  for(unsigned i=0;i<8;++i){
   unsigned want=0xdeadbeef;
   if(expected && i<n){
    if(!((int)(x+i)>=left && (int)(x+i)<left+4 && (int)y>=top && (int)y<top+4))
     want=src[i];
   }
   assert(dst[i+1]==want);
  }
 }
 assert(!window_present_cursor_publish_span(0,0,0,0,1,0));
 unsigned src=123,dst=321;
 const Rect invalid[]={{0,0,0,1},{0,0,1,0},{2,2,1,1}};
 for(unsigned i=0;i<3;++i){
  assert(!window_present_cursor_publish_span(&dst,&src,0,0,1,&invalid[i]));
  assert(dst==321);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='gop-cursor-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+str(root/'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'test')], check=True)
    # 即使像素裁剪仍正确，重新依赖实时光标状态也必须被捕获后状态变化的断言拒绝。
    broken = program.replace('cursor_bounds == 0 || rect_is_empty',
        '!g_window_server.presented_pointer_valid || cursor_bounds == 0 || rect_is_empty')
    assert broken != program
    (path / 'test.c').write_text(broken)
    subprocess.run(command, check=True)
    assert subprocess.run([str(path/'test')], stderr=subprocess.DEVNULL,
                          stdout=subprocess.DEVNULL).returncode != 0
print('GOP cursor publication: bounding box untouched, outside spans copied,  clipped canaries PASS')
print('光标跨度只读取显式包围盒，空区域不写入，重新读取实时光标状态的反例被拒绝')
