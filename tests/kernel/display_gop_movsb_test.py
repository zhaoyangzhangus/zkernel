"""Execute the real default GOP span copier, including protected-page edges."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/display/core.c').read_text()
body = source[source.index('static void display_copy_native_xrgb8888_span('):
              source.index('void display_core_publish_xrgb8888_pixel(')]
conversion = source[source.index('static uint32_t display_component('):
                    source.index('static void display_copy_native_xrgb8888_span(')]
pixel = source[source.index('void display_core_publish_xrgb8888_pixel('):
               source.index('void display_core_publish_xrgb8888_span(')]
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
static struct {uint32_t format,masks[4];} g_display;
''' + conversion + body + pixel + r'''
int main(void) {
 /* 最终像素只写指定位置；检查默认、位掩码和后备格式，前后哨兵不可变化。 */
 for(unsigned format=0;format<4;++format){
  g_display.format=format;
  g_display.masks[0]=0xf800;g_display.masks[1]=0x7e0;g_display.masks[2]=0x1f;
  for(uint32_t p=0;p<65536;++p){
   uint32_t color=(p<<8)|(p&255),output[3]={0xdeadbeef,0,0xdeadbeef};
   display_core_publish_xrgb8888_pixel(output+1,color);
   uint32_t expected=color&0xffffff;
   if(format==2)expected=((((color>>16)&255)*31+127)/255<<11)|
      ((((color>>8)&255)*63+127)/255<<5)|(((color&255)*31+127)/255);
   assert(output[0]==0xdeadbeef&&output[2]==0xdeadbeef&&output[1]==expected);
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
  // Both the first and last byte must stay inside the one accessible page.
  for(unsigned end=0;end<2;++end) {
   uint8_t* to=d+page+(end?page-n:0);
   const uint8_t* from=s+page+(end?page-n:0);
   display_copy_native_xrgb8888_span((volatile uint32_t*)to,
                                   (const uint32_t*)from,pixels);
   assert(!memcmp(to,from,n));
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
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)
print('GOP movsb: actual default copier, 4864 alignment/size cases, '
      '2050 guarded-page copies, read-only source, canaries and null/zero passed')
