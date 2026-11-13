"""低内存光标绘制的独立像素对照：显式裁剪、边界、透明度及行距。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/cursor_occlusion.c').read_text()
body = source[source.index('void compositor_cursor_locked('):]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/rect.h>
#include <kernel/pixel_blend.h>
#define WINDOW_CURSOR_WIDTH 24U
#define WINDOW_CURSOR_HEIGHT 24U
static uint32_t actual[71*48+2], expected[71*48+2];
static unsigned seen_alpha[256];
static struct {volatile uint32_t *composite_framebuffer;uint32_t display_width,display_height,display_stride;} g_window_server;
/* 不提供全局裁剪字段，绘制必须使用调用方传入的范围。 */
static struct {uint32_t pointer_x,pointer_y,cursor_shape;} g_compositor_snapshot;
static uint32_t window_cursor_hotspot_x(uint32_t shape){return shape%3==0?0:12;}
static uint32_t window_cursor_hotspot_y(uint32_t shape){return shape%3==1?0:12;}
static uint32_t window_cursor_pixel(uint32_t shape,uint32_t x,uint32_t y){
 assert(x<24&&y<24);
 unsigned alpha=(x+3*y+shape)%256;
 seen_alpha[alpha]=1;
 return (alpha<<24)|((x*937U+y*7211U+shape*17U)&0xffffffU);
}
''' + body + r'''
int main(void){
 g_window_server.display_width=64;g_window_server.display_height=48;g_window_server.display_stride=71;
 for(unsigned sample=0;sample<3000;++sample){
  g_window_server.display_width=sample%43==0?0:sample%3==0?31:64;
  g_window_server.display_height=sample%47==0?0:sample%5==0?23:48;
  for(unsigned i=0;i<71*48+2;++i)actual[i]=expected[i]=i*2654435761U+sample;
  g_window_server.composite_framebuffer=sample%31?actual+1:0;
  g_compositor_snapshot.pointer_x=sample%97==0?UINT32_MAX:sample%80;
  g_compositor_snapshot.pointer_y=sample%101==0?UINT32_MAX:(sample/7)%64;
  g_compositor_snapshot.cursor_shape=sample%257;
  Rect clip={(int)(sample%80)-8,(int)((sample/11)%64)-8,
             (int)((sample/5)%80),(int)((sample/13)%64)};
  if(sample%7==0)clip=(Rect){INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX};
  int64_t left=(int64_t)g_compositor_snapshot.pointer_x-window_cursor_hotspot_x(g_compositor_snapshot.cursor_shape);
  int64_t top=(int64_t)g_compositor_snapshot.pointer_y-window_cursor_hotspot_y(g_compositor_snapshot.cursor_shape);
  for(int y=0;y<48;++y)for(int x=0;x<64;++x){
   if(!g_window_server.composite_framebuffer||(unsigned)x>=g_window_server.display_width||
      (unsigned)y>=g_window_server.display_height||x<clip.x0||x>=clip.x1||y<clip.y0||y>=clip.y1||
      x<left||x>=left+24||y<top||y>=top+24)continue;
   unsigned pixel=window_cursor_pixel(g_compositor_snapshot.cursor_shape,x-left,y-top),alpha=pixel>>24;
   if(!alpha)continue;
   unsigned old=expected[1+y*71+x],result=0;
   for(unsigned shift=0;shift<24;shift+=8)
    result|=(((((old>>shift)&255)*(255-alpha)+((pixel>>shift)&255)*alpha+127)/255)<<shift);
   expected[1+y*71+x]=result;
  }
  compositor_cursor_locked(clip);
  assert(!memcmp(actual,expected,sizeof(actual)));
 }
 for(unsigned alpha=0;alpha<256;++alpha)assert(seen_alpha[alpha]);
 puts("低内存光标：3000 组独立像素对照通过，覆盖显式裁剪、屏外热点、透明度、极端坐标、全部 256 档透明度与行尾保护");
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-cursor-clip-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
