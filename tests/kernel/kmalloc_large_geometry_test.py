"""Characterize actual large-allocation orders, including fragmented failures.

This does not assert that kmalloc must support scattered pages: it records
why callers requiring only virtual contiguity cannot rely on free-byte totals.
"""
from pathlib import Path
import re
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/kmalloc.c').read_text()
start = source.index('static void *allocate_large(')
body = source[start:source.index('\n}', start) + 2]
start = source.index('typedef struct allocation_header {')
header = source[start:source.index('} allocation_header_t;', start) + len('} allocation_header_t;')]
definitions = '\n'.join(line for line in source.splitlines()
                        if re.match(r'#define KMEM_(MAGIC|PAYLOAD_OFFSET|TRAILER_SIZE|CLASS_LARGE|STATE_ALLOCATED|REDZONE_BYTE|FRONT_SIZE|POISON_ALLOC)\s', line))
backend = (root / 'kernel/fs/vfs/backend.c').read_text()
backend = backend[backend.index('struct {\n    uint64_t failures;'):
                  backend.index('kstatus_t vfs_fat32_backend_read(')]
backend = backend.replace('memset(', 'ram_memset(')
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SHIFT 12
#define PAGE_SIZE (1U<<PAGE_SHIFT)
#define BUDDY_MAX_ORDER 18
#define PAGE_OWNER_SLAB 1
@DEFINITIONS@
@HEADER@
typedef struct {unsigned owner;void *memory;size_t bytes;} page_t;
static unsigned calls,observed_order,max_order;
static size_t payload_zero_bytes,payload_poison_bytes,ram_zero_bytes;
static size_t requested_size,live_bytes;
static unsigned live_pages;
static void *storage;
static page_t *page_alloc(unsigned order,unsigned flags){
 assert(!flags);++calls;observed_order=order;
 if(order>max_order)return 0;
 page_t *p=malloc(sizeof(*p));assert(p);
 p->bytes=(size_t)PAGE_SIZE<<order;
 p->memory=malloc(p->bytes);assert(p->memory);
 storage=p->memory;++live_pages;live_bytes+=p->bytes;return p;
}
static uintptr_t page_to_phys(page_t *p){assert(p && p->memory);return (uintptr_t)p->memory;}
static void *phys_to_direct(uintptr_t address){return (void*)address;}
static void page_free(page_t *p){
 assert(p && p->memory && live_pages && live_bytes>=p->bytes);
 if(storage==p->memory)storage=0;
 --live_pages;live_bytes-=p->bytes;free(p->memory);free(p);
}
static void bytes_fill(void *p,uint8_t c,size_t n){
 if(!c)payload_zero_bytes+=n;
 if(c==KMEM_POISON_ALLOC)payload_poison_bytes+=n;
 memset(p,c,n);
}
static void *ram_memset(void *p,int c,size_t n){
 if(!c)ram_zero_bytes+=n;return memset(p,c,n);
}
static void *header_payload(allocation_header_t *h){return (char*)h+KMEM_PAYLOAD_OFFSET;}
static uint64_t allocation_cookie(allocation_header_t *h,void *p){assert(h && p);return 42;}
static void write_trailer(void *p,size_t n,uint64_t cookie){
 assert(n==requested_size);memcpy((char*)p+n,&cookie,sizeof(cookie));
}
@BODY@
/* Combine the real VFS policy with the real large allocator rather than a
 * malloc-only fixture that hides physical-order inflation. */
typedef int kstatus_t;
enum {K_OK=0,K_EINVAL=-1,K_EOVERFLOW=-2,K_ENOMEM=-3};
typedef struct {uint8_t *data;uint64_t size,data_capacity;bool directory;} vfs_backend_node_t;
static void *kmalloc(size_t n,unsigned flags){
 assert(!flags);requested_size=n;return allocate_large(n,flags,false);
}
static __attribute__((unused)) void *kzalloc(size_t n,unsigned flags){
 assert(!flags);requested_size=n;return allocate_large(n,flags,true);
}
static void kfree(void *p){
 if(!p)return;
 allocation_header_t *header=(void*)((char*)p-KMEM_PAYLOAD_OFFSET);
 assert(header->magic==KMEM_MAGIC);
 page_t *owner=(void*)header->owner;assert(owner->memory==header);page_free(owner);
}
static bool kmalloc_try_grow(void *p,size_t n){(void)p;(void)n;return false;}
static kstatus_t page_memory_get_stats(uint64_t *t,uint64_t *f){
 *t=1048576;*f=8192;return K_OK;
}
@BACKEND@
int main(void){
 const size_t sizes[]={2048,4096,65536,1048576,2097152,4194304};
 for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i){
  requested_size=sizes[i];max_order=18;calls=0;
  void *p=allocate_large(requested_size,0,true);assert(p && calls==1);
  size_t physical=(size_t)PAGE_SIZE<<observed_order;
  size_t needed=requested_size+KMEM_PAYLOAD_OFFSET+KMEM_TRAILER_SIZE;
  assert(physical>=needed && (observed_order==0 || physical/2<needed));
  assert(((allocation_header_t*)storage)->requested==requested_size);
  for(size_t n=0;n<requested_size;++n)assert(((unsigned char*)p)[n]==0);
  printf("request=%zu physical=%zu order=%u\n",requested_size,physical,observed_order);
  kfree(p);assert(!live_pages && !live_bytes);
 }
 /* A fragmented allocator can have hundreds of MiB in <=2MiB blocks.
  * The real allocation function still requests order10 (4MiB) for 2MiB. */
 max_order=9;calls=0;requested_size=2097152;
 assert(!allocate_large(requested_size,0,false));
 assert(calls==1 && observed_order==10);
 calls=0;requested_size=UINT64_MAX;
 assert(!allocate_large(requested_size,0,false) && !calls);
 const size_t file_sizes[]={17,255,1984,2048,4040,4095,4096,4097,65537,1048577,2097153,3145728,
                           4194304-KMEM_PAYLOAD_OFFSET-KMEM_TRAILER_SIZE};
 unsigned file_cases=0;
 for(unsigned i=0;i<sizeof(file_sizes)/sizeof(*file_sizes);++i){
  size_t size=file_sizes[i],needed=size+KMEM_PAYLOAD_OFFSET+KMEM_TRAILER_SIZE;
  unsigned order=0;while(((size_t)PAGE_SIZE<<order)<needed)++order;
  max_order=order;calls=0;vfs_backend_node_t b={0};
  payload_zero_bytes=payload_poison_bytes=ram_zero_bytes=0;
  assert(vfs_memory_backend_truncate(&b,size)==K_OK);
  assert(calls==1 && observed_order==order && b.data_capacity==size);
  assert(payload_zero_bytes==size && !payload_poison_bytes && !ram_zero_bytes);
  for(size_t n=0;n<size;++n)assert(b.data[n]==0);
  printf("RAM-file=%zu physical=%zu order=%u\n",size,(size_t)PAGE_SIZE<<order,order);
  /* In-capacity changes reuse backing and re-exposed bytes must be zero. */
  b.data[size-1]=0x5a;unsigned before=calls;
  assert(vfs_memory_backend_truncate(&b,size-1)==K_OK);
  assert(vfs_memory_backend_truncate(&b,size)==K_OK && calls==before);
  assert(b.data[size-1]==0);
  assert(vfs_memory_backend_truncate(&b,0)==K_OK && !storage);
  ++file_cases;
 }
 /* A large old backing and its smaller replacement coexist during shrink.
  * Limit the allocator to the minimal new order: geometric rounding would
  * fail and silently retain the entire old allocation in these cases. */
 const size_t shrinks[]={17,255,1984,2048,4040,4095,4096,131073,262145,524289,786432};
 for(unsigned i=0;i<sizeof(shrinks)/sizeof(*shrinks);++i){
  vfs_backend_node_t b={0};max_order=18;
  assert(vfs_memory_backend_truncate(&b,3145728)==K_OK);
  memset(b.data,0x6d,(size_t)b.size);
  size_t size=shrinks[i],needed=size+KMEM_PAYLOAD_OFFSET+KMEM_TRAILER_SIZE;
  unsigned order=0;while(((size_t)PAGE_SIZE<<order)<needed)++order;
  uint8_t *old=b.data;max_order=order;calls=0;
  assert(vfs_memory_backend_truncate(&b,size)==K_OK);
  assert(calls==1 && observed_order==order && b.data!=old);
  assert(b.size==size && b.data_capacity==size && live_pages==1);
  assert(live_bytes==((size_t)PAGE_SIZE<<order));
  for(size_t j=0;j<size;++j)assert(b.data[j]==0x6d);
  printf("RAM-shrink=%zu physical=%zu order=%u\n",size,live_bytes,order);
  assert(vfs_memory_backend_truncate(&b,0)==K_OK && !live_pages && !live_bytes);
 }
 /* No speculative larger allocation or duplicate exact-size retry on OOM. */
 vfs_backend_node_t b={0};max_order=9;calls=0;
 assert(vfs_memory_backend_truncate(&b,3145728)==K_ENOMEM);
 assert(calls==1 && observed_order==10 && !b.data && !b.size && !b.data_capacity);
 assert(g_vfs_memory_allocation_failure.required==3145728);
 assert(g_vfs_memory_allocation_failure.attempted_capacity==3145728);
 printf("RAM-file exact growth: %u order/zero-tail/reuse cases and atomic OOM passed\n",file_cases);
}
'''
for token, value in (('DEFINITIONS', definitions), ('HEADER', header),
                     ('BODY', body), ('BACKEND', backend)):
    program = program.replace('@' + token + '@', value)
with tempfile.TemporaryDirectory(prefix='kmalloc-large-geometry-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=15)
    for name, old, new in (
        ('geometric truncate needs an unnecessarily larger buddy block',
         'if (!amortize) capacity = required;',
         'if (!amortize) capacity = required < 4096U ? required : 4096U;'),
        ('geometric shrink retains old backing under fragmentation',
         'uint64_t capacity = size;',
         'uint64_t capacity = 4096U; while (capacity < size) capacity *= 2U;'),
        ('small explicit truncate must not force a 4 KiB payload',
         'if (amortize && capacity < 4096U)',
         'if (capacity < 4096U)'),
        ('fresh backing must not be poisoned before zero initialization',
         ': kzalloc((size_t)capacity, 0)', ': kmalloc((size_t)capacity, 0)'),
        ('fresh backing must not be zeroed twice',
         'size > backend->size && !zeroed', 'size > backend->size'),
    ):
        negative = program.replace(old, new)
        assert negative != program, name
        subprocess.run(command, input=negative, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, text=True, timeout=15,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert result.returncode != 0 and 'Assertion' in result.stderr, (name, result)
        print('Negative control rejected:', name)
print('PASS: production allocation geometry, initialized payloads, fragmented order failure and overflow rejection')
