"""执行桌面缓存恢复及统一复制分派，检查裁剪、行距和设备回退。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]

def function(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <kernel/rect.h>
static struct {
 volatile uint32_t *composite_framebuffer,*framebuffer;
 uint32_t display_width,display_height,display_stride;
} g_window_server;
static struct {Rect damage_bounds;} g_compositor_snapshot;
static uint32_t *g_desktop_cache;
static uint32_t g_desktop_cache_stride;
static unsigned wb_calls,device_calls;
static bool expected_private;
static void compositor_copy_wb_pixels(uint32_t *dst,const uint32_t *src,uint32_t n){
 assert(expected_private);++wb_calls;memcpy(dst,src,n*4);
}
static void display_core_publish_xrgb8888_span(volatile uint32_t *dst,const uint32_t *src,uint32_t n){
 assert(!expected_private);++device_calls;for(unsigned i=0;i<n;++i)dst[i]=src[i];
}
''' + function('kernel/graphics/compose_cpu.c', 'void compositor_surface_copy_destination(') + '\n' + function('kernel/graphics/shell.c', 'static void desktop_copy_cached_region(void)') + r'''
static uint32_t seed=103;
static unsigned next(void){seed=seed*1664525U+1013904223U;return seed;}
int main(void){
 uint32_t source[13*11],target[13*11],front[13*11];
 for(unsigned i=0;i<143;++i)source[i]=i+100;
 const Rect edges[]={
  {0,0,9,7},{2,3,8,6},{-4,-5,3,2},{7,5,INT32_MAX,INT32_MAX},
  {INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX},{0,0,0,7},
  {7,6,2,3},{INT32_MIN,0,-1,5},{10,0,20,5},
 };
 for(unsigned mode=0;mode<2;++mode)for(unsigned trial=0;trial<2009;++trial){
  Rect rect=trial<9?edges[trial]:(Rect){(int)(next()%31)-10,(int)(next()%25)-8,
                                                  (int)(next()%31)-10,(int)(next()%25)-8};
  expected_private=mode!=0;wb_calls=device_calls=0;
  for(unsigned i=0;i<143;++i){target[i]=0xdeadbeef;front[i]=0xcafebabe;}
  g_window_server.composite_framebuffer=target;
  g_window_server.framebuffer=mode?front:target;
  g_window_server.display_width=9;g_window_server.display_height=7;
  g_window_server.display_stride=13;g_desktop_cache_stride=11;
  g_desktop_cache=source;g_compositor_snapshot.damage_bounds=rect;
  desktop_copy_cached_region();
  unsigned rows=0;
  for(int y=0;y<11;++y){
   bool copied_row=false;
   for(int x=0;x<13;++x){
    bool hit=x<9&&y<7&&x>=rect.x0&&x<rect.x1&&y>=rect.y0&&y<rect.y1;
    assert(target[y*13+x]==(hit?source[y*11+x]:0xdeadbeef));
    assert(front[y*13+x]==0xcafebabe);copied_row|=hit;
   }
   rows+=copied_row;
  }
  assert(wb_calls==(mode?rows:0)&&device_calls==(mode?0:rows));
  assert(!memcmp(&rect,&g_compositor_snapshot.damage_bounds,sizeof(rect)));
 }
 wb_calls=device_calls=0;g_desktop_cache=0;desktop_copy_cached_region();
 g_desktop_cache=source;g_window_server.composite_framebuffer=0;desktop_copy_cached_region();
 assert(!wb_calls&&!device_calls);
 puts("Desktop cache copy: 4018 clipped/padded pixel oracles, WB/device routing, unchanged bounds and null targets PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='desktop-cache-copy-') as directory:
    base = Path(directory)
    fixture, binary = base/'test.c', base/'test'
    fixture.write_text(program)
    command = ['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I'+str(root/'include'), str(fixture), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
    fixture.write_text(program.replace('bool private_wb = target != g_window_server.framebuffer;',
                                       'bool private_wb = true;'))
    subprocess.run(command, check=True)
    assert subprocess.run([str(binary)], stderr=subprocess.DEVNULL).returncode != 0
print('负面对照通过：把设备 framebuffer 当作普通内存时分派断言失败')
