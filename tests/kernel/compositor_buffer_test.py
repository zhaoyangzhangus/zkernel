"""验证真实场景缓冲分配入口的几何检查、映射归属和失败后备。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/buffer.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static struct {
 uint32_t display_width,display_stride,display_height;
 volatile uint32_t *framebuffer,*composite_framebuffer;
} g_window_server;
static unsigned allocations;
static size_t requested;
static void *owned;
static int fail;
static void *vmalloc(size_t bytes) {
 ++allocations;requested=bytes;
 if(fail)return 0;
 assert(!owned && !posix_memalign(&owned,4096,bytes));
 memset(owned,0,bytes);return owned;
}
''' + source + r'''
static void setup(uint32_t width,uint32_t stride,uint32_t height) {
 free(owned);owned=0;allocations=0;requested=0;fail=0;
 g_window_server.display_width=width;g_window_server.display_stride=stride;
 g_window_server.display_height=height;
 g_window_server.framebuffer=(volatile uint32_t *)(uintptr_t)0x1000;
 window_buffer_reset_locked();
}
int main(void) {
 const unsigned modes[][3]={{1920,1920,1080},{800,832,600},{1,1,1}};
 for(unsigned m=0;m<3;++m) {
  setup(modes[m][0],modes[m][1],modes[m][2]);
  window_buffer_prepare();
  assert(allocations==1&&requested==(size_t)modes[m][1]*modes[m][2]*4);
  assert(owned&&g_window_server.composite_framebuffer==owned&&((uintptr_t)owned%4096)==0);
  window_buffer_prepare();assert(allocations==1);
 }
 setup(1920,1920,1080);fail=1;window_buffer_prepare();
 assert(allocations==1&&g_window_server.composite_framebuffer==g_window_server.framebuffer);
 window_buffer_prepare();assert(allocations==1);
 const unsigned invalid[][3]={{1920,0,1080},{1920,1920,0},{1920,1000,1080},
                             {1,UINT32_MAX,UINT32_MAX}};
 for(unsigned i=0;i<4;++i) {
  setup(invalid[i][0],invalid[i][1],invalid[i][2]);window_buffer_prepare();
  assert(!allocations&&g_window_server.composite_framebuffer==g_window_server.framebuffer);
 }
 setup(1,1,1);window_buffer_set_target_locked(g_window_server.framebuffer);
 window_buffer_prepare();assert(!allocations);
 free(owned);
 puts("Scene buffer: exact stride sizing, mapped target, reuse, OOM fallback and invalid/overflow geometry PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-buffer-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-D_POSIX_C_SOURCE=200112L', '-std=c11', '-O2',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
