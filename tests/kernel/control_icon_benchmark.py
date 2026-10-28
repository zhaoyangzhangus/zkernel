"""比较单点采样和扫描递推；性能结果不作为跨机器硬门槛。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <kernel/control_icons.h>
__attribute__((noinline)) static uint32_t previous(uint32_t symbol,uint32_t x,uint32_t y,uint32_t size) {
 uint32_t coverage=0;
 for(uint32_t sy=0;sy<4;++sy)for(uint32_t sx=0;sx<4;++sx) {
  uint32_t u=(uint32_t)(((uint64_t)x*8+sx*2+1)*128/size);
  uint32_t v=(uint32_t)(((uint64_t)y*8+sy*2+1)*128/size);
  coverage+=control_icon_vector_sample(symbol,u,v);
 }
 return (coverage+8)/16;
}
__attribute__((noinline)) static uint32_t current(uint32_t symbol,uint32_t x,uint32_t y,uint32_t size) {
 return control_icon_vector_alpha(symbol,x,y,size);
}
static uint64_t monotonic(void) {
 struct timespec value;assert(!clock_gettime(CLOCK_MONOTONIC,&value));
 return (uint64_t)value.tv_sec*1000000000ULL+value.tv_nsec;
}
static uint64_t measure(uint32_t(*function)(uint32_t,uint32_t,uint32_t,uint32_t),
                       unsigned size,uint64_t *sum) {
 uint64_t begin=monotonic(),result=0;
 for(unsigned i=0;i<300000;++i) {
  unsigned x=i%size,y=(i/size)%size;
  result+=function(i&1?OS_CONTROL_SYMBOL_FOLDER:OS_CONTROL_SYMBOL_FILE,x,y,size);
 }
 uint64_t duration=monotonic()-begin;*sum=result;return duration;
}
static uint64_t median(uint64_t a,uint64_t b,uint64_t c) {
 return a>b?(b>c?b:a>c?c:a):(a>c?a:b>c?c:b);
}
/* 两条路径绘制相同的十二种完整图标，保留相同的覆盖率累计副作用。 */
__attribute__((noinline)) static uint64_t frame_scalar(unsigned symbol,unsigned size) {
 uint64_t sum=0;
 for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)
  sum+=control_symbol_alpha(symbol,x,y,size);
 return sum;
}
__attribute__((noinline)) static uint64_t frame_scan(unsigned symbol,unsigned size) {
 control_icon_axis_t columns,rows;
 assert(control_icon_axis_start(&columns,0,size));
 assert(control_icon_axis_start(&rows,0,size));
 uint64_t sum=0;
 for(unsigned y=0;y<size;++y) {
  control_icon_axis_t cursor=columns;
  for(unsigned x=0;x<size;++x) {
   sum+=control_icon_samples_alpha(symbol,cursor.samples,rows.samples);
   control_icon_axis_next(&cursor);
  }
  control_icon_axis_next(&rows);
 }
 return sum;
}
static uint64_t frames(uint64_t(*draw)(unsigned,unsigned),unsigned size,uint64_t *sum) {
 uint64_t start=monotonic(),result=0;
 for(unsigned n=0;n<24;++n)result+=draw(OS_CONTROL_SYMBOL_BACK+n%12,size);
 *sum=result;return monotonic()-start;
}
int main(void) {
 unsigned sizes[]={16,64,128};
 for(unsigned i=0;i<3;++i) {
  uint64_t before[3],after[3],a,b;
  for(unsigned j=0;j<3;++j) {
   /* 交替顺序，减小频率变化与冷热缓存对一方的系统性偏差。 */
   if(j&1){after[j]=measure(current,sizes[i],&b);before[j]=measure(previous,sizes[i],&a);}
   else{before[j]=measure(previous,sizes[i],&a);after[j]=measure(current,sizes[i],&b);}
   assert(a==b);
  }
  printf("icon %upx: previous_median_ns=%llu current_median_ns=%llu samples=300000 checksum=%llu\n",
   sizes[i],(unsigned long long)median(before[0],before[1],before[2]),
   (unsigned long long)median(after[0],after[1],after[2]),(unsigned long long)a);
 }
 for(unsigned i=0;i<3;++i) {
  uint64_t scalar[3],scan[3],a,b;
  for(unsigned j=0;j<3;++j) {
   if(j&1){scan[j]=frames(frame_scan,sizes[i],&b);scalar[j]=frames(frame_scalar,sizes[i],&a);}
   else{scalar[j]=frames(frame_scalar,sizes[i],&a);scan[j]=frames(frame_scan,sizes[i],&b);}
   assert(a==b);
  }
  printf("scan %upx: scalar_median_ns=%llu scan_median_ns=%llu frames=24 checksum=%llu\n",
   sizes[i],(unsigned long long)median(scalar[0],scalar[1],scalar[2]),
   (unsigned long long)median(scan[0],scan[1],scan[2]),(unsigned long long)a);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='control-icon-benchmark-') as directory:
    path = Path(directory)
    (path / 'bench.c').write_text(source)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-mno-sse', '-mno-sse2',
                    '-I' + str(root / 'include'), str(path / 'bench.c'),
                    '-o', str(path / 'bench')], check=True)
    subprocess.run([str(path / 'bench')], check=True, timeout=30)
