"""验证真实装饰像素函数的主题、透明度、损坏裁剪与屏幕边界。"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/graphics/decorations.c').read_text()
a=source.index('static void compositor_blend_pixel_locked(')
body=source[a:source.index('static void compositor_title_controls_locked(',a)]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_raster.h>
#include <uapi/window_theme.h>
static uint32_t frame[13*9],expected[13*9];
static struct {volatile uint32_t *composite_framebuffer;uint32_t display_width,display_height,display_stride;}g_window_server;
static struct {uint32_t theme;Rect damage_bounds;}g_compositor_snapshot;
static uint32_t state=19;
static uint32_t next(void){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
static uint32_t reference(uint32_t under,uint32_t color,uint32_t alpha){
 color=os_theme_resolve(color,g_compositor_snapshot.theme);
 if(alpha>=255)return os_theme_resolve(color,g_compositor_snapshot.theme);
 uint32_t out=under&0xff000000U;
 for(unsigned s=0;s<24;s+=8)
  out|=((((under>>s)&255U)*(255U-alpha)+((color>>s)&255U)*alpha+127U)/255U)<<s;
 return out;
}
''' + body + r'''
int main(void){
 g_window_server.display_width=10;g_window_server.display_height=8;g_window_server.display_stride=13;
 const uint32_t colors[]={OS_THEME_HEADER,OS_THEME_SURFACE,OS_THEME_BORDER,OS_THEME_ACCENT,OS_THEME_TEXT,0x9a123456U,0xffabcdefU};
 for(unsigned i=0;i<200000;++i){
  for(unsigned n=0;n<117;++n)frame[n]=expected[n]=next();
  g_compositor_snapshot.theme=(i%3)|(((i/3)%4)<<8);
  Rect clip={(int)(i%5)-2,(int)((i/5)%5)-2,7+(int)(i%5),5+(int)(i%5)};
  g_compositor_snapshot.damage_bounds=clip;
  int x=(int)(next()%17)-3,y=(int)(next()%13)-3;
  uint32_t alpha=i%301,color=i%2?next():colors[(i/2)%7];
  bool present=i%19!=0;
  g_window_server.composite_framebuffer=present?frame:0;
  if(present&&alpha&&x>=0&&x<10&&y>=0&&y<8&&x>=clip.x0&&x<clip.x1&&y>=clip.y0&&y<clip.y1)
   expected[y*13+x]=reference(expected[y*13+x],color,alpha);
  compositor_blend_pixel_locked(clip,x,y,color,alpha);
  assert(!memcmp(frame,expected,sizeof(frame)));
 }
 puts("Decoration blend: 200000 independent pixel/clip/theme/stride/alpha cases PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='decoration-blend-') as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
