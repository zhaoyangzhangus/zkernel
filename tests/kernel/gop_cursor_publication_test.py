"""Production publication keeps the cursor in each visible GOP scanline."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/present_cursor.c').read_text()
function = source[source.index('bool window_present_cursor_publish_span('):
                  source.index('void compositor_present_cursor_direct(')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
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
  assert(window_present_cursor_publish_span(dst+1,src,x,y,n)==expected);
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
 assert(!window_present_cursor_publish_span(0,0,0,0,1));
}
'''
with tempfile.TemporaryDirectory(prefix='gop-cursor-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('GOP cursor publication: bounding box untouched, outside spans copied,  clipped canaries PASS')
