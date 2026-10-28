"""工具栏符号按目标尺寸抗锯齿，不能退回低分辨率点阵放大。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = r'''
#include <assert.h>
#include <stdio.h>
#include <kernel/control_icons.h>
/* 用浮点最近点距离作为独立线段基准，不调用生产覆盖率函数。 */
static int line(double x,double y,double ax,double ay,double bx,double by,double r) {
 double dx=bx-ax,dy=by-ay,len=dx*dx+dy*dy;
 double t=len?((x-ax)*dx+(y-ay)*dy)/len:0;
 if(t<0)t=0;
 if(t>1)t=1;
 dx=x-ax-t*dx;dy=y-ay-t*dy;
 return dx*dx+dy*dy<=r*r+1e-8;
}
static unsigned reference(unsigned symbol,unsigned x,unsigned y) {
 if(symbol==OS_CONTROL_SYMBOL_SEARCH) {
  double dx=(double)x-416,dy=(double)y-416,d=dx*dx+dy*dy;
  return ((d>=232*232&&d<=296*296)||line(x,y,608,608,864,864,36))*255;
 }
 if(symbol==OS_CONTROL_SYMBOL_FORWARD)x=1023-x;
 return (line(x,y,608,224,320,512,36)||line(x,y,320,512,608,800,36))*255;
}
int main(void) {
 unsigned sizes[]={12,16,20,24,32,48,64,96,128,256};
 for(unsigned s=0;s<sizeof(sizes)/sizeof(*sizes);++s) {
  unsigned size=sizes[s];
  for(unsigned symbol=OS_CONTROL_SYMBOL_BACK;symbol<=OS_CONTROL_SYMBOL_FILE;++symbol) {
   unsigned solid=0,partial=0;
   for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
    unsigned a=control_symbol_alpha(symbol,x,y,size);
    assert(a<=255);solid+=a==255;partial+=a>0&&a<255;
    if(symbol==OS_CONTROL_SYMBOL_SEARCH||symbol==OS_CONTROL_SYMBOL_BACK||symbol==OS_CONTROL_SYMBOL_FORWARD) {
     unsigned expected=0;
     for(unsigned sy=0;sy<4;++sy)for(unsigned sx=0;sx<4;++sx)
      expected+=reference(symbol,((uint64_t)x*8+sx*2+1)*128/size,
                                ((uint64_t)y*8+sy*2+1)*128/size);
     assert(a==(expected+8)/16);
    }
   }
   assert(partial);
   if(size>=48)assert(solid);
   assert(!control_symbol_alpha(symbol,size,0,size));
   assert(!control_symbol_alpha(symbol,0,size,size));
   assert(!control_symbol_alpha(symbol,0,0,0));
  }
 }
 /* 放大后的线条内部保持实色，而不是放大旧点阵的过渡色。 */
 assert(control_symbol_alpha(OS_CONTROL_SYMBOL_MENU,128,64,256)==255);
 assert(!control_symbol_alpha(OS_CONTROL_SYMBOL_MENU,128,54,256));
 assert(!control_symbol_alpha(OS_CONTROL_SYMBOL_NONE,4,4,16));
 assert(!control_symbol_alpha(999,4,4,16));
 puts("vector toolbar icons: 12 symbols, 10 sizes, independent geometry and antialiasing passed");
}
'''
with tempfile.TemporaryDirectory(prefix='control-symbols-') as directory:
    src = Path(directory) / 'test.c'
    exe = Path(directory) / 'test'
    src.write_text(source)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-I', str(root / 'include'),
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
