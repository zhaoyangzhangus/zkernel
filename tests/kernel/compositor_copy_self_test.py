"""运行真实合成器复制自检，验证复用堆内容、分配失败回收及尺寸边界。"""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/compose_cpu.c').read_text()
source = source[source.index('void compositor_copy_wb_pixels('):source.index('void compositor_fill_wb_pixels(')]
source = source.replace('    compositor_copy_wb_pixels(destination, source, pixel_count);',
                        '#ifndef OMIT_TEST_COPY\n    compositor_copy_wb_pixels(destination, source, pixel_count);\n#endif', 1)
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
static struct {uint32_t display_width,display_height;} g_window_server;
static unsigned allocations,frees,fail_at;static size_t allocated_bytes;
static void *kmalloc(size_t bytes,unsigned flags){
 assert(!flags);++allocations;if(allocations==fail_at)return NULL;
 void *p=malloc(bytes);assert(p);/* 模拟堆复用：旧目标恰好与本次源数据相同，也不能掩盖复制失败。 */
 for(size_t i=0;i<bytes/4;++i)((uint32_t *)p)[i]=0x00100000U|(i&0xffffU);
 allocated_bytes+=bytes;return p;
}
static void kfree(void *p){assert(p);++frees;free(p);}
static uint64_t telemetry_timestamp(void){return 1;}
static void kernel_perf_emit_scope(const char *name,uint64_t start){(void)name;assert(start==1);}
static void kernel_perf_emit_value(const char *name,uint64_t bytes){
 (void)name;assert(bytes==(uint64_t)g_window_server.display_width*g_window_server.display_height*4);
}
''' + source + r'''
int main(void){
 unsigned sizes[][2]={{1,1},{3,5},{63,65},{1920,1080}};
 for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i)for(unsigned failure=0;failure<=2;++failure){
  g_window_server.display_width=sizes[i][0];g_window_server.display_height=sizes[i][1];
  allocations=frees=0;allocated_bytes=0;fail_at=failure;
  bool expected=failure==0;
#ifdef OMIT_TEST_COPY
  expected=false;
#endif
  assert(compositor_copy_self_test()==expected);
  assert(allocations==2 && frees==(failure?1:2));
  if(!failure)assert(allocated_bytes==(size_t)sizes[i][0]*sizes[i][1]*8);
 }
 unsigned invalid[][2]={{0,1},{1,0},{UINT32_MAX,UINT32_MAX},{UINT32_MAX,2}};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i){
  g_window_server.display_width=invalid[i][0];g_window_server.display_height=invalid[i][1];
  allocations=frees=0;assert(!compositor_copy_self_test());assert(!allocations&&!frees);
 }
 /* 任意像素跨度和源/目标对齐下，实际汇编不得触碰保护区。 */
 uint32_t src[1030],dst[1030];for(unsigned i=0;i<1030;++i)src[i]=i*76543;
 for(unsigned n=0;n<=1024;++n)for(unsigned a=0;a<2;++a)for(unsigned b=0;b<2;++b){
  for(unsigned i=0;i<1030;++i)dst[i]=0xA5A5A5A5;
  compositor_copy_wb_pixels(dst+2+a,src+2+b,n);
  for(unsigned i=0;i<1030;++i){
   if(i>=2+a && i<2+a+n)assert(dst[i]==src[i-a+b]);
   else assert(dst[i]==0xA5A5A5A5);
  }
 }
 puts("Compositor copy self-test: matching stale heap buffers, 1080p, allocation failures/cleanup, invalid geometry and 4100 guarded copy spans passed");
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-copy-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True)
    subprocess.run(['cc','-std=c11','-O2','-DOMIT_TEST_COPY','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True)
    print('Negative control: matching stale heap data cannot hide an omitted self-test copy')
