"""验证文件图标按实际尺寸光栅化，边缘不会随放大变成模糊块。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <kernel/control_icons.h>
/* 独立浮点几何基准，不调用生产代码的轮廓判定。 */
static int rounded(double x,double y,double l,double t,double r,double b,double rad) {
 if(x<l||x>=r||y<t||y>=b)return 0;
 double cx=x<l+rad?l+rad:x>r-rad?r-rad:x;
 double cy=y<t+rad?t+rad:y>b-rad?b-rad:y;
 return (x-cx)*(x-cx)+(y-cy)*(y-cy)<=rad*rad;
}
int main(void) {
 unsigned sizes[]={1,2,7,15,16,17,31,32,33,63,64,65,95,96,97,127,128,129,255,256,257,511,512};
 for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);++i) {
  unsigned size=sizes[i];
  for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
   unsigned expected=0,file_expected=0;
   for(unsigned sy=0;sy<4;++sy)for(unsigned sx=0;sx<4;++sx) {
    double u=(double)(((uint64_t)x*8+sx*2+1)*128/size);
    double v=(double)(((uint64_t)y*8+sy*2+1)*128/size);
    if(rounded(u,v,128,192,512,448,48)||rounded(u,v,64,320,960,896,64))expected+=255;
    if(u>=192&&u<832&&v>=64&&v<960&&!(v<256&&u>v+576)) {
     if(u>=624&&v>=224&&v<272)file_expected+=96;
     else if(u>=320&&u<704&&((v>=448&&v<480)||(v>=608&&v<640)||(v>=768&&v<800)))
      file_expected+=48;
     else file_expected+=255;
    }
   }
   assert(control_symbol_alpha(OS_CONTROL_SYMBOL_FOLDER,x,y,size)==(expected+8)/16);
   assert(control_symbol_alpha(OS_CONTROL_SYMBOL_FILE,x,y,size)==(file_expected+8)/16);
  }
  assert(!control_symbol_alpha(OS_CONTROL_SYMBOL_FOLDER,size,0,size));
  assert(!control_symbol_alpha(OS_CONTROL_SYMBOL_FILE,0,size,size));
 }
 /* 直边仅在目标像素内抗锯齿；放大八倍也不产生八像素模糊带。 */
 for(unsigned size=64;size<=512;size*=2) {
  assert(control_symbol_alpha(OS_CONTROL_SYMBOL_FOLDER,size/16-1,size/2,size)==0);
  assert(control_symbol_alpha(OS_CONTROL_SYMBOL_FOLDER,size/16,size/2,size)==255);
  assert(control_symbol_alpha(OS_CONTROL_SYMBOL_FILE,size*3/16-1,size/2,size)==0);
  assert(control_symbol_alpha(OS_CONTROL_SYMBOL_FILE,size*3/16,size/2,size)==255);
 }
 assert(!control_symbol_alpha(OS_CONTROL_SYMBOL_FOLDER,0,0,0));
 assert(!control_symbol_alpha(OS_CONTROL_SYMBOL_FILE,UINT32_MAX-1,0,UINT32_MAX));
 puts("vector file icons: independent folder/file geometry, 23 sizes, sharp edges and bounds passed");
}
'''
with tempfile.TemporaryDirectory(prefix='control-icons-') as directory:
    src = Path(directory) / 'test.c'
    exe = Path(directory) / 'test'
    src.write_text(source)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-I', str(root / 'include'),
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
