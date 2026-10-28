"""真实 PNG 行反滤波与独立逐字节算法对照，并测量宿主侧内循环。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/png.c').read_text()
start = source.index('static uint8_t png_paeth(')
end = source.index('static void png_write_pixel(', start)
functions = source[start:end]
program = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
''' + functions + r'''
static bool reference(uint8_t *data,unsigned height,size_t width,unsigned bpp) {
 for(unsigned row=0;row<height;++row) {
  uint8_t *p=data+row*(width+1)+1;
  uint8_t *prior=row?p-width-1:0;unsigned filter=p[-1];
  if(filter>4)return false;
  for(size_t x=0;x<width;++x) {
   unsigned left=x>=bpp?p[x-bpp]:0,up=prior?prior[x]:0;
   unsigned corner=prior&&x>=bpp?prior[x-bpp]:0,predict=0;
   switch(filter) {
   case 1:predict=left;break;
   case 2:predict=up;break;
   case 3:predict=(left+up)/2;break;
   case 4: {
    int estimate=(int)left+(int)up-(int)corner;
    int a=abs(estimate-(int)left),b=abs(estimate-(int)up),c=abs(estimate-(int)corner);
    predict=a<=b&&a<=c?left:b<=c?up:corner;break;
   }
   }
   p[x]=(uint8_t)(p[x]+predict);
  }
 }
 return true;
}
static unsigned random_state=0x59b17;
static uint8_t next_byte(void) {
 random_state=random_state*1664525U+1013904223U;return random_state>>24;
}
static unsigned cases;
static void compare(unsigned bpp,size_t row_bytes,unsigned height,unsigned filter,unsigned offset) {
 size_t size=(row_bytes+1)*height;
 uint8_t *a=malloc(size+offset+1),*b=malloc(size+offset+1);assert(a&&b);
 memset(a,0xa5,offset);a[offset+size]=0x7c;
 for(size_t i=0;i<size;++i)a[offset+i]=next_byte();
 for(unsigned y=0;y<height;++y)a[offset+y*(row_bytes+1)]=
  filter==5?(uint8_t)(y%5):filter==6?(y+1==height?255:(uint8_t)(y%5)):(uint8_t)filter;
 memcpy(b,a,size+offset+1);
 bool expected=reference(b+offset,height,row_bytes,bpp);
 assert(png_unfilter(a+offset,height,row_bytes,bpp)==expected);
 assert(!memcmp(a,b,size+offset+1));++cases;free(a);free(b);
}
static double seconds(void) {
 struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));
 return t.tv_sec+t.tv_nsec*1e-9;
}
int main(void) {
 for(unsigned bpp=1;bpp<=4;++bpp)for(unsigned width=0;width<=257;++width)
  for(unsigned filter=0;filter<=6;++filter)
   compare(bpp,(size_t)width*bpp,1+width%7,filter,width%16);
 /* 极短行仍须安全，错误过滤类型不能越过失败行继续改写。 */
 for(unsigned bpp=1;bpp<=4;++bpp)for(unsigned n=0;n<bpp;++n)
  for(unsigned filter=0;filter<=6;++filter)compare(bpp,n,3,filter,1);
 compare(3,7680,0,0,0);
 printf("PNG unfilter: %u byte-exact cases, five filters, mixed rows, first/short rows, malformed filters and bounds PASS\n",cases);
#ifndef SANITIZED
 size_t row=2560*3,size=(row+1)*1440;
 uint8_t *input=malloc(size),*a=malloc(size),*b=malloc(size);assert(input&&a&&b);
 for(size_t i=0;i<size;++i)input[i]=next_byte();
 for(unsigned filter=0;filter<5;++filter) {
  for(unsigned y=0;y<1440;++y)input[y*(row+1)]=(uint8_t)filter;
  double begin=seconds();
  for(unsigned i=0;i<3;++i){memcpy(b,input,size);assert(reference(b,1440,row,3));}
  double old=seconds()-begin;begin=seconds();
  for(unsigned i=0;i<3;++i){memcpy(a,input,size);assert(png_unfilter(a,1440,row,3));}
  double now=seconds()-begin;assert(!memcmp(a,b,size));
  printf("Host-only PNG row benchmark filter=%u: reference=%.4fs row=%.4fs ratio=%.2fx; not desktop speedup\n",filter,old,now,old/now);
 }
 free(input);free(a);free(b);
#else
 (void)seconds;
#endif
}
'''
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
with tempfile.TemporaryDirectory(prefix='png-unfilter-') as directory:
    path = Path(directory)
    test = path/'test.c'
    for sanitized in (True,False):
        test.write_text(program)
        command = ['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fno-pie','-no-pie']
        if sanitized:
            command += ['-fsanitize=address,undefined','-DSANITIZED']
        command += [str(test),'-o',str(path/'test')]
        subprocess.run(command,check=True)
        subprocess.run([str(path/'test')],check=True,timeout=60)
    # 改错 Paeth 的平局优先级或预测值必须被像素对照检出。
    assert 'return up_distance <= up_left_distance ? up : up_left;' in program
    test.write_text(program.replace('return up_distance <= up_left_distance ? up : up_left;', 'return left;'))
    subprocess.run(command,check=True)
    result = subprocess.run([str(path/'test')],capture_output=True,timeout=30)
    assert result.returncode and b'!memcmp' in result.stderr,result.stderr
    print('Negative control rejected: incorrect PNG Paeth predictor')
