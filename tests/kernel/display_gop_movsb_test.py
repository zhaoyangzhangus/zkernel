"""Execute the real default GOP span copier, including protected-page edges."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/display/core.c').read_text()
body = source[source.index('static void display_copy_native_xrgb8888_span('):
              source.index('void display_core_publish_xrgb8888_pixel(')]
conversion = source[source.index('static void display_prepare_pixel_format('):
                    source.index('static void display_copy_native_xrgb8888_span(')]
conversion = conversion.replace('pixel_format_component(', 'counted_component(')
console = (root/'kernel/console.c').read_text()
console = console[console.index('static UINT32 gop_debug_color('):
                  console.index('static UINT32 gop_debug_blend(')]
pixel = source[source.index('void display_core_publish_xrgb8888_pixel('):
               source.index('void display_core_publish_xrgb8888_span(')]
span = source[source.index('void display_core_publish_xrgb8888_span('):
              source.index('static bool display_commit_shape_valid(')]
assert 'cld; rep movsb' in body
program = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <kernel/pixel_format.h>
#include <kernel/bootinfo.h>
static struct {uint32_t format;} g_display;
static struct {uint32_t format,masks[4];} g_gop_debug_console;
static uint32_t g_display_components[3][256];
static unsigned component_calls;
static uint32_t counted_component(uint8_t value,uint32_t mask) {
 ++component_calls;return pixel_format_component(value,mask);
}
''' + conversion + body + pixel + span + console + r'''
static void prepare(unsigned format,const uint32_t masks[4]) {
 display_prepare_pixel_format(format,masks);
 g_gop_debug_console.format=format;
 memcpy(g_gop_debug_console.masks,masks,sizeof(g_gop_debug_console.masks));
}
static uint32_t reference_component(unsigned value,uint32_t mask) {
 if(!mask)return 0;
 unsigned shift=__builtin_ctz(mask);
 return (uint32_t)(((value*(uint64_t)(mask>>shift)+127)/255)<<shift)&mask;
}
int main(void) {
 /* 独立单像素参考检查两像素顺序、保留字节及任意位组合。 */
 uint64_t pair_seed=0x0123456789abcdefULL;
 for(unsigned n=0;n<1000064U;++n) {
  pair_seed=pair_seed*6364136223846793005ULL+1442695040888963407ULL;
  uint64_t pair=n<64U?1ULL<<n:pair_seed;
  uint64_t expected=(uint64_t)pixel_xrgb_to_rgbr((uint32_t)pair)|
    ((uint64_t)pixel_xrgb_to_rgbr((uint32_t)(pair>>32U))<<32U);
  assert(pixel_xrgb_pair_to_rgbr(pair)==expected);
 }
 /* 最终像素只写指定位置；检查默认、位掩码和后备格式，前后哨兵不可变化。 */
 for(unsigned format=0;format<4;++format){
  const uint32_t masks[4]={0xf800,0x7e0,0x1f,0};
  prepare(format,masks);
  unsigned prepared=component_calls;
  for(uint32_t p=0;p<65536;++p){
   uint32_t color=(p<<8)|(p&255),output[3]={0xdeadbeef,0,0xdeadbeef};
   display_core_publish_xrgb8888_pixel(output+1,color);
   uint32_t expected=color&0xffffff;
   if(format==0)expected=((color&255)<<16)|(color&0xff00)|((color>>16)&255);
   if(format==2)expected=((((color>>16)&255)*31+127)/255<<11)|
      ((((color>>8)&255)*63+127)/255<<5)|(((color&255)*31+127)/255);
   assert(output[0]==0xdeadbeef&&output[2]==0xdeadbeef&&output[1]==expected);
   assert(gop_debug_color(color)==expected);
  }
  assert(component_calls==prepared);
 }
 /* 覆盖全部连续掩码宽度和偏移，包括全 32 位，防止初始化乘法溢出。 */
 for(unsigned bits=1;bits<=32;++bits)for(unsigned shift=0;shift+bits<=32;++shift) {
  uint32_t mask=(uint32_t)(((1ULL<<bits)-1)<<shift);
  uint32_t masks[4]={mask,0,0,0};
  for(unsigned channel=0;channel<3;++channel) {
   memset(masks,0,sizeof(masks));masks[channel]=mask;
   prepare(2,masks);
   for(unsigned value=0;value<256;++value) {
    uint32_t color=value<<(16-channel*8);
    uint32_t out[3]={0x12345678,0,0x87654321};
    display_core_publish_xrgb8888_span(out+1,&color,1);
    assert(out[0]==0x12345678&&out[2]==0x87654321);
    assert(out[1]==reference_component(value,mask));
    assert(gop_debug_color(color)==out[1]);
   }
  }
 }
 const uint32_t layouts[][4]={{0xff0000,0xff00,255,0},{0x3ff00000,0xffc00,0x3ff,0},
                             {0x1f,0x7e0,0xf800,0},{0,0,0,0}};
 uint32_t seed=123;
 for(unsigned layout=0;layout<4;++layout) {
  prepare(2,layouts[layout]);
  for(unsigned n=0;n<100000;++n) {
   seed=seed*1664525U+1013904223U;
   uint32_t result=0;
   display_core_publish_xrgb8888_span(&result,&seed,1);
   uint32_t expected=reference_component((seed>>16)&255,layouts[layout][0])|
    reference_component((seed>>8)&255,layouts[layout][1])|reference_component(seed&255,layouts[layout][2]);
   assert(result==expected);
   assert(gop_debug_color(seed)==expected);
  }
 }
 display_core_publish_xrgb8888_pixel(NULL,0x123456);
 _Alignas(64) uint8_t input[16384],output[16384];
 for(size_t i=0;i<sizeof(input);++i)input[i]=(uint8_t)(i*37+(i>>8));
 const unsigned lengths[]={0,1,2,3,7,8,15,16,17,31,32,63,64,127,255,800,1024,1920,3840};
 for(unsigned si=0;si<16;++si)for(unsigned di=0;di<16;++di)
 for(unsigned k=0;k<sizeof(lengths)/sizeof(lengths[0]);++k) {
  size_t n=lengths[k]*4U,start=64+di*4U;
  memset(output,0xa5,sizeof(output));
  display_copy_native_xrgb8888_span((volatile uint32_t*)(output+start),
                                  (const uint32_t*)(input+64+si*4),lengths[k]);
  assert(!memcmp(output+start,input+64+si*4,n));
  for(size_t i=0;i<start;++i)assert(output[i]==0xa5);
  for(size_t i=start+n;i<sizeof(output);++i)assert(output[i]==0xa5);
 }
 /* RGB 路径覆盖奇偶长度及源/目标四字节错位；高字节必须清零，哨兵保持不变。 */
 const uint32_t unused_masks[4]={0};
 prepare(0,unused_masks);
 for(unsigned si=0;si<16;++si)for(unsigned di=0;di<16;++di)
 for(unsigned k=0;k<sizeof(lengths)/sizeof(lengths[0]);++k) {
  unsigned n=lengths[k];size_t start=64+di*4U;
  memset(output,0xa5,sizeof(output));
  const uint32_t *src=(const uint32_t*)(input+64+si*4U);
  const uint8_t *before=input+64+si*4U;
  display_core_publish_xrgb8888_span((volatile uint32_t*)(output+start),src,n);
  for(unsigned i=0;i<n;++i){
   const uint8_t *out=output+start+i*4U;
   assert(out[0]==before[i*4U+2]&&out[1]==before[i*4U+1]&&
          out[2]==before[i*4U]&&out[3]==0);
  }
  for(size_t i=0;i<start;++i)assert(output[i]==0xa5);
  for(size_t i=start+n*4U;i<sizeof(output);++i)assert(output[i]==0xa5);
 }
 /* 格式选择在跨度外，整段每个像素仍须匹配独立参考，且不能写入边界之外。 */
 for(unsigned format=0;format<5;++format) {
  const uint32_t masks[4]={0x3ff00000,0xffc00,0x3ff,0};
  prepare(format,masks);unsigned prepared=component_calls;
  for(unsigned si=0;si<16;++si)for(unsigned di=0;di<16;++di)
  for(unsigned k=0;k<sizeof(lengths)/sizeof(lengths[0]);++k) {
   unsigned n=lengths[k];size_t start=64+di*4U;
   memset(output,0xa5,sizeof(output));
   const uint32_t *src=(const uint32_t*)(input+64+si*4U);
   display_core_publish_xrgb8888_span((volatile uint32_t*)(output+start),src,n);
   for(unsigned i=0;i<n;++i) {
    uint32_t color;memcpy(&color,src+i,4);
    uint32_t expected=color&0xffffff;
    if(format==1)expected=color;
    if(format==0)expected=((color&255)<<16)|(color&0xff00)|((color>>16)&255);
    if(format==2)expected=reference_component((color>>16)&255,masks[0])|
       reference_component((color>>8)&255,masks[1])|reference_component(color&255,masks[2]);
    uint32_t actual;memcpy(&actual,output+start+i*4U,4);assert(actual==expected);
   }
   for(size_t i=0;i<start;++i)assert(output[i]==0xa5);
   for(size_t i=start+n*4U;i<sizeof(output);++i)assert(output[i]==0xa5);
  }
  assert(component_calls==prepared);
 }
 prepare(0,unused_masks);
 display_core_publish_xrgb8888_span(NULL,(const uint32_t*)input,1);
 display_core_publish_xrgb8888_span((volatile uint32_t*)output,NULL,1);
 display_copy_native_xrgb8888_span(NULL,(const uint32_t*)input,1);
 display_copy_native_xrgb8888_span((volatile uint32_t*)output,NULL,1);
 size_t page=(size_t)sysconf(_SC_PAGESIZE);assert(page>=4096);
 uint8_t* s=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 uint8_t* d=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(s!=MAP_FAILED && d!=MAP_FAILED);
 assert(!mprotect(s+page,page,PROT_READ|PROT_WRITE));
 assert(!mprotect(d+page,page,PROT_READ|PROT_WRITE));
 for(size_t i=0;i<page;++i)s[page+i]=(uint8_t)(i*19+3);
 assert(!mprotect(s+page,page,PROT_READ));
 for(unsigned pixels=0;pixels<=1024;++pixels) {
  size_t n=(size_t)pixels*4;
  memset(d+page,0xa5,page);
  // 首尾两个字节都必须位于唯一可访问的页面内。
  for(unsigned end=0;end<2;++end) {
   uint8_t* to=d+page+(end?page-n:0);
   const uint8_t* from=s+page+(end?page-n:0);
   display_copy_native_xrgb8888_span((volatile uint32_t*)to,
                                   (const uint32_t*)from,pixels);
   assert(!memcmp(to,from,n));
   display_core_publish_xrgb8888_span((volatile uint32_t*)to,
                                    (const uint32_t*)from,pixels);
   for(unsigned i=0;i<pixels;++i){
    assert(to[i*4U]==from[i*4U+2]&&to[i*4U+1]==from[i*4U+1]&&
           to[i*4U+2]==from[i*4U]&&to[i*4U+3]==0);
   }
  }
 }
 assert(!munmap(s,page*3) && !munmap(d,page*3));
}
'''
with tempfile.TemporaryDirectory(prefix='gop-movsb-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-mno-sse','-mno-mmx','-fno-tree-vectorize',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)
print('GOP movsb: actual default copier, 4864 alignment/size cases, '
      '2050 guarded-page copies, read-only source, canaries and null/zero passed')
print('GOP colors: RGB byte order, 405504 full-width mask cases, 400000 mixed-color oracles and zero per-pixel mask decoding passed')
print('Early console and display conversion produce identical colors across every oracle')

print('RGB pair conversion: 1000064 bit/random oracles against independent single-pixel conversion passed')
print('RGB GOP: 4864 alignment/size cases, 2050 protected-page spans and cleared reserved bytes passed')

print('GOP format dispatch: 24320 multi-pixel format/alignment/length cases, independent colors, X byte preservation and canaries passed')
