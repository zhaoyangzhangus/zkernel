"""标题栏默认图标使用真实公共轮廓：缩放、还原状态、焦点及损坏裁剪。"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/graphics/decorations.c').read_text()
a=source.index('void compositor_control_caption_icon(')
body=source[a:source.index('void compositor_standard_caption_locked(',a)]
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/rect.h>
#include <uapi/control_tree.h>
#include <uapi/chrome_caption.h>
#include <uapi/window_theme.h>
static unsigned pixels[180*150+2];static Rect clip;
static void compositor_blend_pixel_locked(int32_t x,int32_t y,uint32_t color,uint32_t alpha){
 assert(x>=0&&x<180&&y>=0&&y<150);
 if(x<clip.x0||x>=clip.x1||y<clip.y0||y>=clip.y1)return;
 unsigned *p=pixels+1+y*180+x,result=0;
 for(unsigned shift=0;shift<24;shift+=8)
  result|=((((*p>>shift)&255)*(255-alpha)+((color>>shift)&255)*alpha+127)/255)<<shift;
 *p=result;
}
'''+body+r'''
int main(void){
 unsigned cases=0;
 for(unsigned h=40;h<=128;h+=8)for(unsigned a=1;a<=3;++a)
 for(unsigned maximized=0;maximized<2;++maximized)for(unsigned active=0;active<2;++active)
 for(unsigned clipped=0;clipped<2;++clipped){
  unsigned w=(32*h+20)/40;
  for(unsigned i=0;i<180*150+2;++i)pixels[i]=0x345678;
  clip=clipped?(Rect){23,24,40,45}:(Rect){0,0,180,150};
  compositor_control_caption_icon(11,9,w,h,a,maximized,active);
  unsigned side=(w<h?w:h)*12/32,left=11+(w-side)/2,top=9+(h-side)/2;
  unsigned icon=a==OS_CONTROL_WINDOW_MAXIMIZE&&maximized?4:a;
  for(unsigned y=0;y<150;++y)for(unsigned x=0;x<180;++x){
   unsigned expected=0x345678;
   if(x>=left&&x<left+side&&y>=top&&y<top+side&&x>=(unsigned)clip.x0&&x<(unsigned)clip.x1&&y>=(unsigned)clip.y0&&y<(unsigned)clip.y1){
    unsigned alpha=(chrome_caption_alpha(icon,(x-left)*12/side,(y-top)*12/side)*(active?255:97)+127)/255;
    expected=0;
    for(unsigned shift=0;shift<24;shift+=8)
     expected|=((((0x345678>>shift)&255)*(255-alpha)+((OS_THEME_MUTED>>shift)&255)*alpha+127)/255)<<shift;
   }
   assert(pixels[1+y*180+x]==expected);
  }
  assert(pixels[0]==0x345678&&pixels[180*150+1]==0x345678);++cases;
 }
 printf("Caption icons: %u pixel oracles, scale/focus/maximize/clip and guards PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='control-caption-') as directory:
 p=Path(directory);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
  '-fno-pie','-no-pie','-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
