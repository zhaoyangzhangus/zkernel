"""对照独立逐像素公式验证真实边框代码，不允许覆盖圆角外或内容内部。"""
from pathlib import Path
import argparse
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-controls', action='store_true')
args = parser.parse_args()
source = (root / 'kernel/graphics/control_document.c').read_text()
body = (source[source.index('static void control_border_emit('):source.index('/* 底纹锚定到控件原点')]
        + source[source.index('static void control_border('):source.index('static void control_fill_alpha(')])
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/rect.h>
#include <uapi/chrome_caption.h>
#include <kernel/control_raster.h>
#define WINDOW_CORNER_RADIUS 8U
static unsigned pixels[40*32+2];
static unsigned calls,max_height;
static bool count_only;
static struct {Rect damage_bounds;} g_compositor_snapshot;
static void control_fill(int64_t x,int64_t y,unsigned w,unsigned h,unsigned r,unsigned color){
 assert(!r&&h&&w);++calls;if(h>max_height)max_height=h;
 if(count_only)return;
 assert(x>=0&&y>=0&&x+w<=40&&y+h<=32);
 for(unsigned row=0;row<h;++row)for(unsigned i=0;i<w;++i){
  unsigned offset=1+(y+row)*40+x+i;assert(pixels[offset]==0);pixels[offset]=color;
 }
}
''' + body + r'''
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
int main(void){
 unsigned cases=0;
 for(unsigned n=0;n<5000;++n){
  memset(pixels,0,sizeof(pixels));pixels[0]=pixels[1281]=123;
  int64_t x=(int)(n%25)-8,y=(int)(n%21)-6;
  if(n==4998)x=INT32_MIN-10LL;if(n==4999)x=INT32_MAX+10LL;
  unsigned w=n%37,h=n%31,b=n%13,r=n%23;
  g_compositor_snapshot.damage_bounds=(Rect){2,3,38,29};
  control_border(x,y,w,h,r,b,1);
  if(b>w/2)b=w/2;if(b>h/2)b=h/2;
  if(r>w/2)r=w/2;if(r>h/2)r=h/2;
  for(int py=0;py<32;++py)for(int px=0;px<40;++px){
   bool expected=false;
   if(b&&px>=2&&px<38&&py>=3&&py<29&&px>=x&&px<x+w&&py>=y&&py<y+h){
    bool inside=shape(px,py,x,y,w,h,r);
    bool hole=shape(px,py,x+b,y+b,w-2*b,h-2*b,r>b?r-b:0);
    expected=inside&&!hole;
   }
   assert(pixels[1+py*40+px]==expected);++cases;
  }
  assert(pixels[0]==123&&pixels[1281]==123);
 }
 printf("Control borders: %u oracle pixels, four corners, widths, tiny sizes, clipping, extreme origins and no duplicate writes PASS\n",cases);
 count_only=true;calls=max_height=0;
 g_compositor_snapshot.damage_bounds=(Rect){0,0,3840,2160};
 control_border(0,0,3840,2160,8,1,1);
 assert(calls==32&&max_height==2144);
 unsigned rounded=calls;calls=max_height=0;
 control_border(0,0,3840,2160,0,1,1);
 assert(calls==4&&max_height==2158);
 printf("Border work: 4K rounded border %u submissions, rectangular border %u; straight edges use bulk rectangles PASS\n",rounded,calls);
}
'''
with tempfile.TemporaryDirectory(prefix='control-border-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-I'+str(root/'include'), '-x', 'c', '-',
                    '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
    if args.negative_controls:
        shared=(root/'include/kernel/control_raster.h').read_text().replace('#pragma once','')
        negative=code.replace('#include <kernel/control_raster.h>',shared.replace(
            'if (row>=corner && row<h-corner)', 'if (false && row>=corner && row<h-corner)'))
        assert negative != code
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-Wno-misleading-indentation','-I'+str(root/'include'),
                        '-x','c','-','-o',binary],input=negative,text=True,check=True)
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE,(0,0))
        result=subprocess.run([binary],capture_output=True,text=True,preexec_fn=no_core)
        assert result.returncode != 0 and 'calls==32' in result.stderr, result.stderr
        print('负面对照通过：恢复长边逐行提交会触发工作量回归')
