"""实际合成器透明背景与独立逐像素公式对照，包含裁剪、极端原点和护栏。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/control_document.c').read_text()
body = source[source.index('static void control_fill_alpha('):source.index('static void control_paint_text(')]
opaque = source[source.index('static void control_fill('):source.index('static void control_border_emit(')]
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/rect.h>
#include <kernel/control_raster.h>
#include <uapi/window_theme.h>
#define WINDOW_CORNER_RADIUS 8U
static uint32_t pixels[32*24+2];
static struct {Rect damage_bounds;uint32_t theme;} g_compositor_snapshot;
static struct {uint32_t *composite_framebuffer;uint32_t display_stride;} g_window_server={pixels+1,32};
static bool shape(int64_t px,int64_t py,int64_t x,int64_t y,unsigned w,unsigned h,unsigned r){
 if(px<x||py<y||px>=x+w||py>=y+h)return false;
 if(r>w/2)r=w/2;if(r>h/2)r=h/2;
 unsigned ix=px-x,iy=py-y;
 if(r==8)return ix>=chrome_frame_row_inset(iy,w,h)&&ix<w-chrome_frame_row_inset(iy,w,h);
 if(ix>=w/2)ix=w-1-ix;if(iy>=h/2)iy=h-1-iy;
 if(ix>=r||iy>=r)return true;
 int64_t dx=2LL*(r-ix)-1,dy=2LL*(r-iy)-1;
 return dx*dx+dy*dy<=4LL*r*r;
}
'''
main = r'''
int main(void){
 unsigned cases=0;
 for(unsigned a=0;a<=255;a+=17)for(unsigned sample=0;sample<160;++sample){
  uint32_t under=0x804080c0U, color=(a<<24)|0xc06030U;
  for(unsigned i=0;i<32*24+2;++i)pixels[i]=under;
  g_compositor_snapshot.damage_bounds=(Rect){2,3,28,20};
  int64_t x=(int)(sample%40)-10,y=(int)(sample%30)-8;
  uint32_t w=sample%25,h=sample%19;
  if(sample==158)x=INT32_MIN-10LL;
  if(sample==159)x=INT32_MAX+10LL;
  uint32_t radius=sample%13;
  control_fill_alpha(x,y,w,h,radius,color);
  assert(pixels[0]==under&&pixels[32*24+1]==under);
  for(int py=0;py<24;++py)for(int px=0;px<32;++px){
   bool inside=px>=2&&px<28&&py>=3&&py<20&&shape(px,py,x,y,w,h,radius);
   uint32_t expected=under;
   if(inside){expected=under&0xff000000U;
    for(unsigned s=0;s<24;s+=8)expected|=(((((color>>s)&255)*a+((under>>s)&255)*(255-a)+127)/255)<<s);
   }
   assert(pixels[1+py*32+px]==expected);++cases;
  }
 }
 printf("Control alpha: %u oracle pixels, opacity endpoints, clipped edges, corners and extreme origins PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='control-alpha-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(prefix+body+main)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
    # 相同的独立像素公式也必须覆盖真实不透明快路径，而非只验证透明实现。
    spans = r'''
static unsigned fill_calls,largest_height;
static bool count_only;
static void compositor_fill_locked(int32_t x,int32_t y,unsigned w,unsigned h,unsigned color){
 ++fill_calls;if(h>largest_height)largest_height=h;
 if(count_only)return;
 Rect clip=g_compositor_snapshot.damage_bounds;
 int64_t right=(int64_t)x+w,bottom=(int64_t)y+h;
 if(x<clip.x0)x=clip.x0;if(y<clip.y0)y=clip.y0;
 if(right>clip.x1)right=clip.x1;if(bottom>clip.y1)bottom=clip.y1;
 for(int64_t py=y;py<bottom;++py)for(int64_t px=x;px<right;++px){
  assert(px>=0&&px<32&&py>=0&&py<24);
  g_window_server.composite_framebuffer[py*g_window_server.display_stride+px]=color;
 }
}
static void compositor_fill_rounded_locked(int32_t x,int32_t y,unsigned w,unsigned h,unsigned r,unsigned color){
 assert(r==0);compositor_fill_locked(x,y,w,h,color);
}
'''
    opaque_main = main.replace('a=0;a<=255;a+=17', 'a=255;a<=255;a+=17').replace('sample<160', 'sample<4000')
    opaque_main = opaque_main.replace('0x804080c0U', '0xff4080c0U')
    opaque_main = opaque_main.replace('control_fill_alpha(', 'control_fill(')
    opaque_main = opaque_main.replace('Control alpha:', 'Control opaque:')
    opaque_main = opaque_main.replace('\n}\n', r'''
 count_only=true;fill_calls=largest_height=0;
 g_compositor_snapshot.damage_bounds=(Rect){0,0,3840,2160};
 control_fill(0,0,3840,2160,8,0xff123456);
 assert(fill_calls==17&&largest_height==2144);
 fill_calls=largest_height=0;
 g_compositor_snapshot.damage_bounds=(Rect){100,100,3000,2100};
 control_fill(0,0,3840,2160,8,0xff123456);
 assert(fill_calls==1&&largest_height==2000);
 fill_calls=largest_height=0;
 g_compositor_snapshot.damage_bounds=(Rect){0,0,1,2160};
 control_fill(0,0,1,2160,96,0xff123456);
 assert(fill_calls==1&&largest_height==2160);
 puts("Rounded fill: 4K background 17 submissions instead of 2160; clipped middle and clamped radius use one rectangle PASS");
}
''')
    (path/'test.c').write_text(prefix+spans+opaque+opaque_main)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-Wno-misleading-indentation','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-I'+str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
