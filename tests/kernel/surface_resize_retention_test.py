"""真实读取路径：缩放未提交时仍读取固定旧帧，增长区域不能跨行或读到后台槽位。"""
from pathlib import Path
import argparse
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-controls', action='store_true')
args = parser.parse_args()
source = (root/'kernel/graphics/render.c').read_text()
body = source[source.index('void compositor_surface_source_prepare('):]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <kernel/base.h>
#include <uapi/window.h>
#define VM_OBJECT_SHARED 1
typedef struct {unsigned type;} vm_object_t;
typedef struct {vm_object_t *vm_object;} section_t;
typedef struct {
 unsigned width,height,surface_width,surface_height,flags,background;
 uint64_t owner_address,buffer_size,surface_offset;
 bool resize_pending,surface_lease;
 section_t *section;
} compositor_window_view_t;
typedef struct {vm_object_t *object;void *cache;uint64_t row_bytes;bool readable;} compositor_surface_source_context_t;
static uint32_t pages[4096],out[66];
static void *compositor_surface_cache_get(const compositor_window_view_t *w){(void)w;return 0;}
static uint8_t *compositor_surface_page_resolve(const compositor_surface_source_context_t *s,uint64_t index){
 assert(s->readable&&index<4);return (uint8_t*)pages+index*PAGE_SIZE;
}
static void compositor_surface_copy_destination(uint32_t *wb,volatile uint32_t *dev,const uint32_t *src,unsigned n){
 for(unsigned i=0;i<n;++i)if(wb)wb[i]=src[i];else dev[i]=src[i];
}
static void compositor_surface_fill_destination(uint32_t *wb,volatile uint32_t *dev,unsigned n,unsigned color){
 for(unsigned i=0;i<n;++i)if(wb)wb[i]=color;else dev[i]=color;
}
''' + body + r'''
int main(void){
 vm_object_t object={VM_OBJECT_SHARED};section_t section={&object};
 unsigned long checks=0;
 for(unsigned n=0;n<1000;++n){
  unsigned ow=8+n%29,oh=8+n%25,nw=8+(n*7)%45,nh=8+(n*11)%43;
  compositor_window_view_t w={.width=nw,.height=nh,.surface_width=ow,.surface_height=oh,
   .resize_pending=true,.surface_lease=true,.owner_address=0x10000,.section=&section,
   .surface_offset=(n&1)*8192U,.buffer_size=8192,.background=0x00112233};
  for(unsigned i=0;i<4096;++i)pages[i]=0x00badbad;
  unsigned start=w.surface_offset/4;
  for(unsigned y=0;y<oh;++y)for(unsigned x=0;x<ow;++x)pages[start+y*ow+x]=0x800000|(y*256+x);
  compositor_surface_source_context_t s;compositor_surface_source_prepare(&w,&s);
  assert(s.readable&&s.row_bytes==ow*4&&s.object==&object);
  for(unsigned y=0;y<nh;++y)for(unsigned device=0;device<2;++device){
   for(unsigned i=0;i<66;++i)out[i]=0x00abcdef;
   uint64_t page=UINT64_MAX;uint8_t *base=0;
   compositor_copy_surface_span(&w,&s,(uint64_t)y*s.row_bytes,0,nw,
    device?0:out+1,device?out+1:0,&page,&base);
   assert(out[0]==0x00abcdef&&out[nw+1]==0x00abcdef);
   for(unsigned x=0;x<nw;++x){
    unsigned expected=x<ow&&y<oh?(0x800000|(y*256+x)):w.background;
    assert(out[x+1]==expected);++checks;
   }
  }
  w.surface_lease=false;compositor_surface_source_prepare(&w,&s);assert(!s.readable);
  w.surface_lease=true;w.surface_width=0;compositor_surface_source_prepare(&w,&s);assert(!s.readable);
 }
 printf("Surface resize retention: %lu oracle pixels, old committed stride, grow/shrink, both slots, WB/device, no cross-row/back-slot reads and legacy safety PASS\n",checks);
}
'''
with tempfile.TemporaryDirectory(prefix='surface-resize-retention-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
    if args.negative_controls:
        variants = {
            'blank-on-resize': program.replace(
                '(window->resize_pending && !window->surface_lease)', 'window->resize_pending'),
            'new-stride-for-old-frame': program.replace(
                'window->surface_lease ? window->surface_width : window->width', 'window->width'),
            'cross-row-copy': program.replace('if (window->surface_lease) {', 'if (false) {'),
        }
        for name, negative in variants.items():
            assert negative != program
            (path/'test.c').write_text(negative)
            subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                            '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
            def no_core():
                resource.setrlimit(resource.RLIMIT_CORE, (0,0))
            result=subprocess.run([str(path/'test')],capture_output=True,text=True,preexec_fn=no_core)
            assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
            print('Negative control rejected: '+name)
