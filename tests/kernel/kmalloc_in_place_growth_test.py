"""Production large allocator, guard relocation and RAM-file growth together."""
from pathlib import Path
import re
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/kmalloc.c').read_text()
backend = (root / 'kernel/fs/vfs/backend.c').read_text()


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


definitions = '\n'.join(line for line in source.splitlines()
                        if re.match(r'#define KMEM_(MAGIC|COOKIE|PAYLOAD_OFFSET|'
                                    r'TRAILER_SIZE|CLASS_LARGE|STATE_ALLOCATED|'
                                    r'STATE_FREE|REDZONE_BYTE|FRONT_SIZE|POISON_ALLOC)\s', line))
header = source[source.index('typedef struct allocation_header {'):
                source.index('} allocation_header_t;') + len('} allocation_header_t;')]
helpers = '\n'.join(function(signature) for signature in (
    'static void bytes_fill(', 'static bool bytes_equal(',
    'static void *header_payload(', 'static allocation_header_t *payload_header(',
    'static uint64_t allocation_cookie(', 'static void write_trailer(',
    'static bool trailer_valid(', 'static bool header_redzones_valid(',
    'static void *allocate_large(', 'bool kmalloc_try_grow('))
ram = backend[backend.index('struct {\n    uint64_t failures;'):
              backend.index('kstatus_t vfs_fat32_backend_read(')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define PAGE_SHIFT 12U
#define BUDDY_MAX_ORDER 18U
#define PAGE_OWNER_SLAB 3U
@DEFINITIONS@
@HEADER@
typedef struct {unsigned owner;uint8_t order;void *memory;size_t bytes;} page_t;
static unsigned calls,live_pages;
static size_t live_bytes;
static bool fail_alloc;
static page_t *page_alloc(uint8_t order,unsigned flags){
 assert(!flags);++calls;if(fail_alloc)return NULL;
 page_t *p=calloc(1,sizeof(*p));assert(p);
 p->order=order;p->bytes=(size_t)PAGE_SIZE<<order;
 p->memory=malloc(p->bytes);assert(p->memory);
 memset(p->memory,0x35,p->bytes);++live_pages;live_bytes+=p->bytes;return p;
}
static uintptr_t page_to_phys(page_t *p){assert(p && p->memory);return (uintptr_t)p->memory;}
static void *phys_to_direct(uintptr_t p){return (void*)p;}
static void page_free(page_t *p){
 assert(p && live_pages && live_bytes>=p->bytes);
 --live_pages;live_bytes-=p->bytes;free(p->memory);free(p);
}
@HELPERS@
typedef int kstatus_t;
enum {K_OK=0,K_EINVAL=-22,K_EOVERFLOW=-75,K_ENOMEM=-12};
typedef struct {uint8_t *data;uint64_t size,data_capacity;bool directory;} vfs_backend_node_t;
static void *kmalloc(size_t n,unsigned flags){return allocate_large(n,flags,false);}
static void *kzalloc(size_t n,unsigned flags){return allocate_large(n,flags,true);}
static void kfree(void *p){
 if(!p)return;
 allocation_header_t *h=payload_header(p);
 assert(h->magic==KMEM_MAGIC && h->state==KMEM_STATE_ALLOCATED);
 assert(header_redzones_valid(h,p));page_t *page=(void*)h->owner;
 assert(page->memory==h && page->owner==PAGE_OWNER_SLAB);page_free(page);
}
static kstatus_t page_memory_get_stats(uint64_t *t,uint64_t *f){
 *t=1048576;*f=8192;return K_OK;
}
@RAM@
static unsigned cases;
static void rejection(void *p,size_t size,page_t *page){
 void *copy=malloc(page->bytes);assert(copy);memcpy(copy,page->memory,page->bytes);
 unsigned before=calls;
 assert(!kmalloc_try_grow(p,size) && calls==before);
 assert(!memcmp(copy,page->memory,page->bytes));free(copy);++cases;
}
int main(void){
 const size_t sizes[]={4096,4097,65536,131073,786432,2097152};
 for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);++i){
  void *p=kmalloc(sizes[i],0);assert(p);
  allocation_header_t *h=payload_header(p);page_t *page=(void*)h->owner;
  size_t capacity=page->bytes-KMEM_PAYLOAD_OFFSET-KMEM_TRAILER_SIZE;
  memset(p,0x6d,sizes[i]);unsigned before=calls;
  const size_t invalid[]={0,sizes[i]-1,capacity+1,(size_t)UINT32_MAX+1,SIZE_MAX};
  fail_alloc=true;
  for(unsigned j=0;j<sizeof(invalid)/sizeof(*invalid);++j)rejection(p,invalid[j],page);
  const size_t valid[]={sizes[i],sizes[i]+1,capacity-1,capacity};
  for(unsigned j=0;j<sizeof(valid)/sizeof(*valid);++j){
   size_t old=h->requested,n=valid[j];
   assert(kmalloc_try_grow(p,n) && h->requested==n && calls==before);
   assert(header_redzones_valid(h,p));
   for(size_t k=0;k<old;++k)
    assert(((uint8_t*)p)[k]==(k<sizes[i]?0x6d:KMEM_POISON_ALLOC));
   for(size_t k=old;k<n;++k)assert(((uint8_t*)p)[k]==KMEM_POISON_ALLOC);
   ++cases;
  }
  fail_alloc=false;kfree(p);assert(!live_pages && !live_bytes);
 }
 /* 任一验证失败都必须保持所有字节和元数据不变。 */
 void *p=kmalloc(4096,0);allocation_header_t *h=payload_header(p);
 page_t *page=(void*)h->owner;void *clean=malloc(page->bytes);assert(clean);
 memcpy(clean,page->memory,page->bytes);
 for(unsigned i=0;i<10;++i){
  memcpy(page->memory,clean,page->bytes);page->owner=PAGE_OWNER_SLAB;page->order=1;
  switch(i){
   case 0:h->magic=0;break;
   case 1:h->state=KMEM_STATE_FREE;break;
   case 2:h->class_index=0;break;
   case 3:h->owner=0;break;
   case 4:page->owner=0;break;
   case 5:page->order=BUDDY_MAX_ORDER+1;break;
   case 6:h->requested=UINT32_MAX;break;
   case 7:((uint8_t*)h)[sizeof(*h)]^=1;break;
   case 8:((uint8_t*)p)[h->requested]^=1;break;
   case 9:h->cookie^=1;break;
  }
  rejection(p,5000,page);
 }
 memcpy(page->memory,clean,page->bytes);page->owner=PAGE_OWNER_SLAB;page->order=1;
 free(clean);kfree(p);assert(!kmalloc_try_grow(NULL,4096));
 /* 真实共享文件扩展无需分配页面即可成功，即使之后的所有分配都失败也一样。
  * 重新暴露的字节仍必须为零。 */
 vfs_backend_node_t b={0};
 assert(vfs_memory_backend_truncate(&b,2097152)==K_OK);
 memset(b.data,0x57,(size_t)b.size);uint8_t *old=b.data;unsigned before=calls;
 fail_alloc=true;
 assert(vfs_memory_backend_truncate(&b,3145728)==K_OK);
 assert(b.data==old && b.data_capacity==3145728 && calls==before);
 assert(live_bytes==4194304 && live_pages==1);
 for(size_t j=0;j<b.size;++j)assert(b.data[j]==(j<2097152?0x57:0));
 assert(vfs_memory_backend_truncate(&b,0)==K_OK && !live_pages);
 fail_alloc=false;
 /* 追加操作的两倍目标容量无法放入，但精确扩展可以。清空相邻空洞前先读取别名源，
  * 且不能回收该源。 */
 assert(vfs_memory_backend_truncate(&b,32768)==K_OK);
 for(size_t j=0;j<b.size;++j)b.data[j]=(uint8_t)(j*19+7);
 uint8_t expected[64];memcpy(expected,b.data+b.size-64,sizeof(expected));
 old=b.data;before=calls;uint64_t bytes=0;fail_alloc=true;
 assert(vfs_memory_backend_write(&b,b.size+17,b.data+b.size-64,64,&bytes)==K_OK);
 assert(bytes==64 && b.data==old && calls==before && b.size==32849);
 assert(b.data_capacity==32849 && !memcmp(b.data+32785,expected,64));
 for(unsigned j=0;j<32768;++j)assert(b.data[j]==(uint8_t)(j*19+7));
 for(unsigned j=32768;j<32785;++j)assert(!b.data[j]);
 /* 越过物理边界时仍走原有 OOM 路径，并保留旧数据、EOF、保护信息和所有权。 */
 size_t capacity=65536-KMEM_PAYLOAD_OFFSET-KMEM_TRAILER_SIZE;
 assert(vfs_memory_backend_truncate(&b,capacity+1)==K_ENOMEM);
 assert(b.data==old && b.size==32849 && b.data_capacity==32849 && live_pages==1);
 assert(!memcmp(b.data+32785,expected,64));
 /* 超出块容量后，成功替换仍会复制旧前缀、仅回收一次旧所有者，并只暴露填零的新字节。 */
 fail_alloc=false;before=calls;
 assert(vfs_memory_backend_truncate(&b,65500)==K_OK);
 assert(calls==before+1 && b.data!=old && live_pages==1 && live_bytes==131072);
 assert(b.size==65500 && b.data_capacity==65500);
 for(unsigned j=0;j<32768;++j)assert(b.data[j]==(uint8_t)(j*19+7));
 assert(!memcmp(b.data+32785,expected,64));
 for(size_t j=32849;j<b.size;++j)assert(!b.data[j]);
 /* 此请求略低于半块边界。计入 kmalloc 元数据后，它上升了一个阶，因此两倍追加容量
  * 仍然放得下。 */
 old=b.data;before=calls;fail_alloc=true;memcpy(expected,b.data,64);
 assert(vfs_memory_backend_write(&b,b.size+17,b.data,64,&bytes)==K_OK);
 assert(bytes==64 && b.data==old && calls==before && b.data_capacity==131000);
 assert(b.size==65581 && !memcmp(b.data+65517,expected,64));
 for(unsigned j=65500;j<65517;++j)assert(!b.data[j]);
 assert(vfs_memory_backend_truncate(&b,0)==K_OK && !live_pages && !live_bytes);
 printf("In-place growth: %u boundary/guard/metadata cases; real RAM-file 2->3MiB with zero allocations, exact/amortized aliased append, replacement ownership and atomic boundary OOM passed\n",cases);
}
'''
for token, value in (('DEFINITIONS', definitions), ('HEADER', header),
                     ('HELPERS', helpers), ('RAM', ram)):
    program = program.replace('@' + token + '@', value)
with tempfile.TemporaryDirectory(prefix='kmalloc-in-place-growth-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-x', 'c', '-', '-o', binary]

    def run(code):
        subprocess.run(command, input=code, text=True, check=True)
        return subprocess.run([binary], capture_output=True, text=True, timeout=30,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))

    result = run(program)
    assert result.returncode == 0, result.stderr
    print(result.stdout, end='')
    for name, old, new in (
        ('new trailing guard not relocated',
         'write_trailer(pointer, header->requested, header->cookie);', '(void)pointer;'),
        ('growth exceeds physical block', 'size > capacity ||', 'false ||'),
        ('corrupt old guard ignored', '!header_redzones_valid(header, pointer)', 'false'),
        ('RAM growth always allocates replacement',
         'if (backend->data != 0) {\n        if (kmalloc_try_grow',
         'if (false && backend->data != 0) {\n        if (kmalloc_try_grow'),
        ('amortized target blocks exact in-place append',
         'if (capacity != required &&\n            kmalloc_try_grow',
         'if (false && capacity != required &&\n            kmalloc_try_grow'),
    ):
        negative = program.replace(old, new)
        assert negative != program, name
        result = run(negative)
        assert result.returncode != 0, (name, result)
        assert 'Assertion' in result.stderr or 'Sanitizer' in result.stderr, (name, result)
        print('Negative control rejected:', name)
