"""执行生产共享页缓存的创建、失效和解析，检查失败后的可重试性。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/compose_cpu.c').read_text()
start = source.index('struct compositor_surface_page_cache {')
end = source.index('void compositor_surface_fill_destination(')
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define PAGE_SHIFT 12U
#define PAGE_SIZE 4096ULL
#define K_OK 0
#define K_ENOMEM -12
typedef struct {unsigned id;} vm_object_t;
typedef struct {vm_object_t *vm_object;} shared_section_t;
typedef struct {void *compositor_cache;uint64_t buffer_size;} window_server_window_t;
typedef struct {
 window_server_window_t *reference;shared_section_t *section;uint64_t buffer_size;
} compositor_window_view_t;
typedef struct compositor_surface_page_cache compositor_surface_page_cache_t;
typedef struct {
 vm_object_t *object;compositor_surface_page_cache_t *cache;bool readable;
} compositor_surface_source_context_t;
static unsigned allocations,frees,vm_calls;
static size_t allocated_bytes;
static bool fail_alloc,missing;
static int vm_status;
static uint64_t last_offset;
static unsigned char storage[4][4096];
static void *kzalloc(size_t bytes,unsigned flags){
 (void)flags;++allocations;allocated_bytes=bytes;
 if(fail_alloc)return 0;
 assert(bytes<1024*1024);return calloc(1,bytes);
}
static void kfree(void *p){assert(p);++frees;free(p);}
static int vm_object_shared_page_direct(vm_object_t *o,uint64_t offset,bool write,uint8_t **out){
 assert(o&&write);++vm_calls;last_offset=offset;
 *out=missing?0:storage[(offset>>PAGE_SHIFT)%4];return vm_status;
}
''' + source[start:end] + r'''
int main(void){
 vm_object_t objects[2]={{1},{2}};
 shared_section_t sections[2]={{objects},{objects+1}};
 window_server_window_t ref={0};
 compositor_window_view_t view={&ref,sections,1};
 assert(!compositor_surface_cache_get(0));
 assert(!compositor_surface_cache_get(&view));
 view.reference=0;assert(!compositor_surface_cache_get(&view));view.reference=&ref;
 view.section=0;assert(!compositor_surface_cache_get(&view));view.section=sections;
 view.buffer_size=0;assert(!compositor_surface_cache_get(&view));view.buffer_size=1;
 const uint64_t invalid[]={UINT64_MAX,UINT64_MAX-4095,((uint64_t)UINT32_MAX<<12)+1};
 for(unsigned i=0;i<3;++i){ref.buffer_size=invalid[i];assert(!compositor_surface_cache_get(&view));}
 assert(!allocations);
 const uint64_t sizes[]={1,4095,4096,4097,16384};
 for(unsigned i=0;i<5;++i){
  ref.buffer_size=sizes[i];
  compositor_surface_page_cache_t *c=compositor_surface_cache_get(&view);
  unsigned n=(sizes[i]-1)/4096+1;
  assert(c&&c==ref.compositor_cache&&c->section==sections&&c->page_count==n);
  assert(allocated_bytes==sizeof(*c)+n*sizeof(c->pages[0]));
  for(unsigned p=0;p<n;++p)assert(!c->pages[p]);
  unsigned before=allocations;
  assert(compositor_surface_cache_get(&view)==c&&allocations==before);
  kfree(c);ref.compositor_cache=0;
 }
 ref.buffer_size=8192;
 compositor_surface_page_cache_t *c=compositor_surface_cache_get(&view);
 compositor_surface_source_context_t ctx={objects,c,true};
 unsigned before=vm_calls;
 assert(compositor_surface_page_resolve(&ctx,0)==storage[0]&&vm_calls==before+1&&last_offset==0);
 assert(compositor_surface_page_resolve(&ctx,0)==storage[0]&&vm_calls==before+1);
 missing=true;
 assert(!compositor_surface_page_resolve(&ctx,1)&&!c->pages[1]);
 missing=false;vm_status=K_ENOMEM;
 assert(!compositor_surface_page_resolve(&ctx,1)&&!c->pages[1]);
 vm_status=0;
 assert(compositor_surface_page_resolve(&ctx,1)==storage[1]&&c->pages[1]==storage[1]);
 before=vm_calls;
 assert(compositor_surface_page_resolve(&ctx,1)==storage[1]&&vm_calls==before);
 /* 数组范围外仍可走 VM 回退，但不能写越页表。 */
 assert(compositor_surface_page_resolve(&ctx,2)==storage[2]&&last_offset==8192);
 before=vm_calls;
 assert(!compositor_surface_page_resolve(&ctx,UINT64_MAX)&&vm_calls==before);
 assert(!compositor_surface_page_resolve(0,0));
 ctx.readable=false;assert(!compositor_surface_page_resolve(&ctx,0));ctx.readable=true;
 ctx.object=0;assert(!compositor_surface_page_resolve(&ctx,0));ctx.object=objects;
 ctx.cache=0;
 assert(compositor_surface_page_resolve(&ctx,0)==storage[0]);
 /* 分区切换后必须清空旧页地址；分配失败不能留下旧缓存。 */
 view.section=sections+1;unsigned old_frees=frees;fail_alloc=true;
 assert(!compositor_surface_cache_get(&view)&&!ref.compositor_cache&&frees==old_frees+1);
 fail_alloc=false;c=compositor_surface_cache_get(&view);
 assert(c&&c->section==sections+1&&c->page_count==2&&!c->pages[0]&&!c->pages[1]);
 ctx=(compositor_surface_source_context_t){objects+1,c,true};
 before=vm_calls;
 assert(compositor_surface_page_resolve(&ctx,0)==storage[0]&&vm_calls==before+1);
 kfree(c);ref.compositor_cache=0;
 assert(allocations==frees+1); /* 一次分配被故意拒绝。 */
 puts("Surface cache: page rounding/overflow, hit reuse, section replacement, allocation rollback, VM miss retry, no-cache fallback and array bounds PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-surface-cache-') as directory:
    base=Path(directory)
    fixture,binary=base/'test.c',base/'test'
    fixture.write_text(program)
    command=['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-pie','-no-pie',str(fixture),'-o',str(binary)]
    subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True)
    fixture.write_text(program.replace('cache->section == view->section','true'))
    subprocess.run(command,check=True)
    assert subprocess.run([str(binary)],stderr=subprocess.DEVNULL).returncode!=0
print('负面对照通过：分区切换时错误复用旧页表被拒绝')
