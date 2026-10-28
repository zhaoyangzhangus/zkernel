"""对照逐像素 source-over 结果，验证不透明短段只做线性探测。"""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/graphics/render.c').read_text()
start=source.index('static void compositor_surface_blend_span(')
end=source.index('static void compositor_window_fill_span(',start)
blend=source[start:end]
# 计数只用于测试，不改变生产循环；批量复制中的读取也计入。
blend=blend.replace('__builtin_memcpy(&pair, source + end, sizeof(pair));',
                    'pair=(uint64_t)read_pixel(source,end)|((uint64_t)read_pixel(source,end+1)<<32);')
blend=blend.replace('source[index]','read_pixel(source,index)').replace('source[end]','read_pixel(source,end)').replace('source[i]','read_pixel(source,i)')
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
static unsigned reads,copies,fills;
static uint32_t read_pixel(const uint32_t *source,unsigned i){++reads;return source[i];}
static void compositor_surface_copy_destination(uint32_t *wb,volatile uint32_t *dev,const uint32_t *src,uint32_t n){
 ++copies;for(unsigned i=0;i<n;++i){uint32_t v=read_pixel(src,i);if(wb)wb[i]=v;else dev[i]=v;}
}
static void compositor_surface_fill_destination(uint32_t *wb,volatile uint32_t *dev,uint32_t n,uint32_t color){
 ++fills;for(unsigned i=0;i<n;++i)if(wb)wb[i]=color;else dev[i]=color;
}
''' + blend + r'''
static uint32_t seed=91;
static unsigned next(void){seed=seed*1664525U+1013904223U;return seed;}
static uint32_t reference(uint32_t fg,uint32_t bg){
 unsigned a=fg>>24;if(!a)return bg;if(a==255)return fg;
 uint32_t result=0;
 for(unsigned c=0;c<3;++c){
  unsigned channel=((fg>>(c*8))&255)+(((bg>>(c*8))&255)*(255-a)+127)/255;
  result|=(channel>255?255:channel)<<(c*8);
 }
 return result;
}
int main(void){
 uint32_t source[1024],output[1030],expected[1030];
 for(unsigned sample=0;sample<4000;++sample){
  unsigned n=next()%1025;uint32_t color=next();bool constant=sample%4==0;
  if(sample%8==0)color&=0xffffff;
  if(sample%8==4)color|=0xff000000;
  for(unsigned i=0;i<1024;++i){
   uint32_t v=next();
   source[i]=sample%3==0?(i%16==15?v&0xffffff:v|0xff000000):v;
  }
  for(unsigned mode=0;mode<2;++mode){
   for(unsigned i=0;i<1030;++i)output[i]=expected[i]=next();
   for(unsigned i=0;i<n;++i)expected[i+3]=reference(constant?color:source[i],expected[i+3]);
   reads=copies=fills=0;
   compositor_surface_blend_span(mode?0:output+3,mode?output+3:0,constant?0:source,color,n);
   for(unsigned i=0;i<1030;++i)assert(output[i]==expected[i]);
   assert(reads<=3*n);
   if(constant&&(color>>24)==0)assert(!fills&&!copies&&!reads);
   if(constant&&(color>>24)==255)assert(fills==1&&!copies&&!reads);
  }
 }
 for(unsigned i=0;i<1024;++i)source[i]=i%16==15?0:0xffabcdef;
 reads=copies=fills=0;
 compositor_surface_blend_span(output,0,source,0,1024);
 unsigned optimized=reads;
 assert(reads<=3*1024&&copies==0);
 printf("Alpha runs: 8000 guarded WB/device pixel oracles; 15-pixel runs read %u values for 1024 pixels; constant alpha fast paths PASS\n",optimized);
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-alpha-runs-') as directory:
    base=Path(directory);fixture,binary=base/'test.c',base/'test'
    fixture.write_text(program)
    command=['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-pie','-no-pie',str(fixture),'-o',str(binary)]
    subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True)
    # 恢复逐像素重复探测，像素结果仍正确，但读取次数上限必须拒绝它。
    short_start=program.index('            } else {\n                /* 短段')
    short_end=program.index('        uint32_t result = foreground;',short_start)
    broken=program[:short_start]+"""                index = end - 1U;
                continue;
            }
        }
"""+program[short_end:]
    fixture.write_text(broken)
    subprocess.run(command,check=True)
    assert subprocess.run([str(binary)],stderr=subprocess.DEVNULL).returncode!=0
print('负面对照通过：恢复短段重复扫描时，线性读取次数断言失败')

# 直接运行未插桩的生产实现，让末像素紧邻不可读页，覆盖奇数长度及未对齐的成对读取。
guarded = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static void compositor_surface_copy_destination(uint32_t *wb,volatile uint32_t *dev,const uint32_t *src,uint32_t n){
 for(unsigned i=0;i<n;++i)if(wb)wb[i]=src[i];else dev[i]=src[i];
}
static void compositor_surface_fill_destination(uint32_t *wb,volatile uint32_t *dev,uint32_t n,uint32_t color){
 for(unsigned i=0;i<n;++i)if(wb)wb[i]=color;else dev[i]=color;
}
''' + source[start:end] + r'''
int main(void){
 size_t page=(size_t)sysconf(_SC_PAGESIZE);
 uint8_t *mapping=mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(mapping!=MAP_FAILED&&mprotect(mapping+page,page,PROT_NONE)==0);
 uint32_t out[1026];
 for(unsigned n=0;n<=1024;++n)for(unsigned tail=0;tail<2;++tail){
  uint32_t *src=(uint32_t*)(mapping+page)-n;
  for(unsigned i=0;i<n;++i)src[i]=0xff123456;
  if(n&&tail)src[n-1]=0;
  for(unsigned mode=0;mode<2;++mode){
   for(unsigned i=0;i<1026;++i)out[i]=0x00112233;
   compositor_surface_blend_span(mode?0:out+1,mode?out+1:0,src,0,n);
   assert(out[0]==0x00112233&&out[n+1]==0x00112233);
   for(unsigned i=0;i<n;++i)assert(out[i+1]==(tail&&i+1==n?0x00112233:0xff123456));
  }
 }
 assert(munmap(mapping,page*2)==0);
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-alpha-guard-') as directory:
    base=Path(directory);fixture,binary=base/'test.c',base/'test'
    fixture.write_text(guarded)
    subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(fixture),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)
print('透明度成对扫描：4100 组不可读页边界、奇偶长度和 WB/device 输出检查通过')
