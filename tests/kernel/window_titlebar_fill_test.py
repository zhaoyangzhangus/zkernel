"""执行真实标题栏绘制，防止圆角区域遗留整块边框底色。"""
from pathlib import Path
import os
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=Path(os.environ.get('TITLEBAR_SOURCE',root/'kernel/graphics/decorations.c')).read_text()
body=source[source.index('void compositor_titlebar_locked('):]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <uapi/window_theme.h>
#include <uapi/window.h>
#define WINDOW_CORNER_RADIUS 8U
#define WINDOW_TITLE_CONTROLS_WIDTH 96U
typedef struct {uint32_t width,height;uint8_t pixels[128];} caption_text_t;
typedef struct {uint32_t identifier,width,height,flags;int32_t x,y;void *controls;caption_text_t *caption;} compositor_window_view_t;
static struct {uint32_t focused_identifier,theme;} g_compositor_snapshot;
static uint32_t pixels[128*128];
static bool window_client_decorations(uint32_t f){return f&OS_WINDOW_CLIENT_DECORATIONS;}
static bool window_standard_caption(uint32_t f){return f&OS_WINDOW_STANDARD_CAPTION_CONTROLS;}
static uint32_t window_frame_border(uint32_t f){return window_client_decorations(f)?0:1;}
static uint32_t window_frame_extra(uint32_t f){return window_frame_border(f)*2;}
static uint32_t window_titlebar_height(uint32_t f){return window_client_decorations(f)?0:40;}
static uint32_t window_client_offset_y(uint32_t f){return window_frame_border(f)+window_titlebar_height(f);}
static bool window_control_has_caption(const void *c){return c!=0;}
static void compositor_title_controls_locked(const compositor_window_view_t *w){(void)w;}
static void compositor_blend_pixel_locked(int32_t x,int32_t y,uint32_t c,uint32_t a){(void)x;(void)y;(void)c;(void)a;}
static void compositor_fill_locked(int32_t x,int32_t y,uint32_t w,uint32_t h,uint32_t c){
 c=os_theme_resolve(c,g_compositor_snapshot.theme);
 for(int py=y;py<y+(int)h;++py)for(int px=x;px<x+(int)w;++px)
  if(px>=0&&py>=0&&px<128&&py<128)pixels[py*128+px]=c;
}
static void compositor_fill_rounded_locked(int32_t x,int32_t y,uint32_t w,uint32_t h,uint32_t r,uint32_t c){
 (void)r;compositor_fill_locked(x,y,w,h,c);
}
''' + body + r'''
int main(void){
 for(unsigned palette=0;palette<12;++palette)for(unsigned focus=0;focus<2;++focus){
  g_compositor_snapshot.theme=(palette%3)|((palette/3)<<8);
  g_compositor_snapshot.focused_identifier=focus?1:0;
  compositor_window_view_t w={.identifier=1,.width=96,.height=50,.x=10,.y=10,
   .flags=OS_WINDOW_STANDARD_CAPTION_CONTROLS,.controls=(void*)1};
  for(unsigned i=0;i<128*128;++i)pixels[i]=0x123456;
  compositor_titlebar_locked(&w,OS_THEME_BORDER);
  uint32_t title=os_theme_resolve(focus?OS_THEME_HEADER:OS_THEME_HEADER_INACTIVE,g_compositor_snapshot.theme);
  for(unsigned y=1;y<8;++y)for(unsigned x=1;x<8;++x){
   assert(pixels[(10+y)*128+10+x]==title);
   assert(pixels[(10+y)*128+10+97-x]==title);
  }
  assert(pixels[10*128+10+40]==os_theme_resolve(OS_THEME_BORDER,g_compositor_snapshot.theme));
 }
 puts("PASS: real titlebar fills both corner interiors in 12 palettes, active and inactive; frame remains separate");
}
'''
with tempfile.TemporaryDirectory() as d:
    binary=Path(d)/'test'
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-I'+str(root/'include'),'-x','c','-','-o',str(binary)],input=code,text=True,check=True)
    subprocess.run([str(binary)],check=True)
