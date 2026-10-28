"""真实桌面悬停描边与逐像素参考公式对照，覆盖样式继承后的几何和双目标裁剪。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/shell.c').read_text()
body = source[source.index('static void desktop_hover_span('):source.index('/* 遍历像素前先求一次交集')]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
#include <kernel/control_raster.h>
typedef struct {Rect bounds;uint32_t hovered_app;volatile uint32_t *target;
 uint32_t target_width,target_height,target_stride;} desktop_render_context_t;
static uint32_t screen[44*32+2],cache[44*32+2];
static struct {uint32_t display_width,display_height,display_stride;uint32_t *composite_framebuffer;}
 g_window_server={40,32,44,screen+1};
''' + body + r'''
static bool shape(int64_t px,int64_t py,int64_t x,int64_t y,unsigned w,unsigned h,unsigned r){
 if(px<x||py<y||px>=x+w||py>=y+h)return false;
 if(r>w/2)r=w/2;if(r>h/2)r=h/2;
 unsigned ix=(unsigned)(px-x),iy=(unsigned)(py-y);
 if(r==8)return ix>=chrome_frame_row_inset(iy,w,h)&&ix<w-chrome_frame_row_inset(iy,w,h);
 if(ix>=w/2)ix=w-1-ix;if(iy>=h/2)iy=h-1-iy;
 if(ix>=r||iy>=r)return true;
 int64_t dx=2LL*(r-ix)-1,dy=2LL*(r-iy)-1;
 return dx*dx+dy*dy<=4LL*r*r;
}
int main(void){
 unsigned pixels=0;
 for(unsigned sample=0;sample<2400;++sample){
  uint32_t under=0xa0306090U,color=((sample%16)*17U<<24)|0x1a73e8U;
  for(unsigned i=0;i<44*32+2;++i)screen[i]=cache[i]=under;
  bool private_target=sample&1;unsigned tw=private_target?36:40,th=private_target?28:32;
  uint32_t *target=private_target?cache:screen;
  desktop_render_context_t c={.bounds={2,3,39,31},.target=private_target?cache+1:0,
   .target_width=private_target?36:0,.target_height=private_target?28:0,.target_stride=private_target?44:0};
  if(sample%3==0)c.bounds.x0=-5;if(sample%4==0)c.bounds.y0=-3;
  if(sample%5==0)c.bounds.x1=70;if(sample%7==0)c.bounds.y1=100;
  int64_t x=(int)(sample%33)-8,y=(int)(sample%29)-6;
  unsigned w=sample%39,h=sample%31,b=sample%9,r=sample%24;
  if(sample==2398)x=INT32_MIN;if(sample==2399)x=INT32_MAX;
  control_tree_style_t style={.border_width=b,.radius=r,.border_color=color};
  desktop_draw_hover_frame_locked((int32_t)x,(int32_t)y,w,h,&c,&style);
  if(b>w/2)b=w/2;if(b>h/2)b=h/2;if(r>w/2)r=w/2;if(r>h/2)r=h/2;
  for(unsigned py=0;py<32;++py)for(unsigned px=0;px<44;++px){
   bool inside=b&&(int32_t)px>=c.bounds.x0&&(int32_t)py>=c.bounds.y0&&
    (int32_t)px<c.bounds.x1&&(int32_t)py<c.bounds.y1&&px<tw&&py<th&&
    shape(px,py,x,y,w,h,r)&&!shape(px,py,x+b,y+b,w-2*b,h-2*b,r>b?r-b:0);
   uint32_t expected=under;
   if(inside){expected=under&0xff000000U;unsigned alpha=color>>24;
    for(unsigned s=0;s<24;s+=8)
     expected|=(((((under>>s)&255U)*(255-alpha)+((color>>s)&255U)*alpha+127)/255)<<s);
   }
   assert(target[1+py*44+px]==expected);
   assert((private_target?screen:cache)[1+py*44+px]==under);++pixels;
  }
  assert(screen[0]==under&&cache[0]==under&&screen[44*32+1]==under&&cache[44*32+1]==under);
 }
 desktop_render_context_t invalid={.bounds={0,0,40,32},.target_width=40,.target_stride=1};
 control_tree_style_t style={.border_width=2,.radius=8,.border_color=0xff123456};
 uint32_t before[44*32+2];memcpy(before,screen,sizeof(screen));
 desktop_draw_hover_frame_locked(0,0,32,24,&invalid,&style);assert(!memcmp(before,screen,sizeof(screen)));
 desktop_draw_hover_frame_locked(0,0,32,24,0,&style);
 desktop_draw_hover_frame_locked(0,0,32,24,&invalid,0);
 printf("Desktop shared border: %u oracle pixels, 2400 style/alpha/clip cases, private and display targets, invalid stride and guards PASS\n",pixels);
}
'''
with tempfile.TemporaryDirectory(prefix='desktop-hover-style-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True)
