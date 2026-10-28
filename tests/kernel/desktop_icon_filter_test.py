"""Production HD icon filtering preserves coverage without transparent fringes."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <kernel/desktop_icon.h>
/* 独立浮点参考实现，既不调用生产轴计算，也不复用定点权重。 */
static long double axis_weight(unsigned source,unsigned target,unsigned position,unsigned sample) {
 long double low,high;
 if(target>source) {
  low=((long double)position+.5L)*source/target-.5L;
  if(low<0)low=0;
  if(low>source-1)low=source-1;
  high=low+1;
 } else {
  low=(long double)position*source/target;
  high=(long double)(position+1)*source/target;
 }
 return fmaxl(0,fminl(high,sample+1)-fmaxl(low,sample));
}
static uint32_t reference(const uint8_t *p,unsigned sw,unsigned sh,
 unsigned dw,unsigned dh,unsigned x,unsigned y) {
 long double a=0,r=0,g=0,b=0,total=0;
 for(unsigned j=0;j<sh;++j)for(unsigned i=0;i<sw;++i) {
  long double w=axis_weight(sw,dw,x,i)*axis_weight(sh,dh,y,j);
  const uint8_t *q=p+(j*sw+i)*4;
  total+=w;w*=q[3];a+=w;r+=w*q[0];g+=w*q[1];b+=w*q[2];
 }
 if(!a)return 0;
 return (uint32_t)floorl(a/total+.5L)<<24 |
  (uint32_t)floorl(r/a+.5L)<<16 | (uint32_t)floorl(g/a+.5L)<<8 |
  (uint32_t)floorl(b/a+.5L);
}
static void compare(uint32_t a,uint32_t b) {
 for(unsigned shift=0;shift<32;shift+=8) {
  int d=(int)((a>>shift)&255)-(int)((b>>shift)&255);
  assert(d>=-1&&d<=1);
 }
}
int main(void) {
 uint8_t p[]={255,0,0,255,0,0,255,0,255,0,0,255,0,255,0,0};
 assert(desktop_icon_area_pixel(p,8,0,0,2,2,1,1,0,0)==0x80ff0000);
 assert(desktop_icon_area_pixel(p,8,0,0,2,2,2,2,0,0)==0xffff0000);
 assert(desktop_icon_area_pixel(p,8,0,0,2,2,2,2,1,0)==0);
 /* 放大后边缘覆盖率必须连续，透明蓝绿像素不能产生彩边。 */
 uint32_t smooth[]={0xffff0000,0xbfff0000,0x40ff0000,0};
 for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)
  assert(desktop_icon_area_pixel(p,8,0,0,2,2,4,4,x,y)==smooth[x]);
 uint8_t gradient[]={0,0,0,255,240,240,240,255};
 uint32_t shades[]={0xff000000,0xff3c3c3c,0xffb4b4b4,0xfff0f0f0};
 for(unsigned x=0;x<4;++x)
  assert(desktop_icon_area_pixel(gradient,8,0,0,2,1,4,1,x,0)==shades[x]);
 /* 单像素裁剪区放大时不得取到相邻图标。 */
 for(unsigned y=0;y<9;++y)for(unsigned x=0;x<7;++x)
  assert(desktop_icon_area_pixel(gradient,8,1,0,1,1,7,9,x,y)==0xfff0f0f0);
 /* 横向放大、纵向缩小，以及相反方向都必须平滑。 */
 uint8_t mixed[]={0,0,0,255,240,240,240,255,
                  0,0,0,255,240,240,240,255,
                  0,0,0,255,240,240,240,255};
 for(unsigned x=0;x<4;++x)
  assert(desktop_icon_area_pixel(mixed,8,0,0,2,3,4,1,x,0)==shades[x]);
 uint8_t vertical[]={0,0,0,255,0,0,0,255,0,0,0,255,
                    240,240,240,255,240,240,240,255,240,240,240,255};
 for(unsigned y=0;y<4;++y)
  assert(desktop_icon_area_pixel(vertical,12,0,0,3,2,1,4,0,y)==shades[y]);
 unsigned samples=0;
 uint32_t seed=0x46534141;
 uint8_t random[7*7*4];
 for(unsigned sw=1;sw<=7;++sw)for(unsigned sh=1;sh<=7;++sh) {
  for(unsigned i=0;i<sw*sh*4;++i) {
   seed=seed*1664525U+1013904223U;random[i]=(uint8_t)(seed>>24);
  }
  for(unsigned dw=1;dw<=9;++dw)for(unsigned dh=1;dh<=9;++dh)
   for(unsigned y=0;y<dh;++y)for(unsigned x=0;x<dw;++x) {
    compare(desktop_icon_area_pixel(random,sw*4,0,0,sw,sh,dw,dh,x,y),
            reference(random,sw,sh,dw,dh,x,y));
    ++samples;
   }
 }
 printf("Icon scaling: %u independent reference pixels PASS\n",samples);
 uint8_t q[]={0,0,0,255,120,120,120,255,240,240,240,255};
 assert(desktop_icon_area_pixel(q,12,0,0,3,1,2,1,0,0)==0xff282828);
 assert(desktop_icon_area_pixel(q,12,0,0,3,1,2,1,1,0)==0xffc8c8c8);
 assert(desktop_icon_area_pixel(q,12,1,0,1,1,1,1,0,0)==0xff787878);
 assert(!desktop_icon_area_pixel(0,0,0,0,2,2,1,1,0,0));
 assert(!desktop_icon_area_pixel(p,8,0,0,2,2,1,1,1,0));
 for(unsigned kind=0;kind<2;++kind)for(unsigned size=24;size<=128;size+=8) {
  unsigned edges=0,opaque=0;
  for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
   uint32_t a=desktop_icon_vector_pixel(kind,size,x,y)>>24;
   edges+=a>0&&a<255;opaque+=a==255;
  }
  assert(edges>0 && opaque>size*size/3);
  assert(!desktop_icon_vector_pixel(kind,size,0,0));
 }
 assert(!desktop_icon_vector_pixel(0,0,0,0));
 assert(!desktop_icon_vector_pixel(0,64,64,0));
}
'''
with tempfile.TemporaryDirectory(prefix='desktop-hd-icons-') as directory:
    path=Path(directory); (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-I'+str(root/'include'),
        str(path/'test.c'),'-lm','-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('HD icons: exact weighted area, transparent fringe exclusion, crop/upscale and vector coverage PASS')
