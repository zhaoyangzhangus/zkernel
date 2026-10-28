"""Large RAM-file truncates release capacity without making shrink fallible."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/backend.c').read_text()
code = source[source.index('struct {\n    uint64_t failures;'):
              source.index('kstatus_t vfs_fat32_backend_read(')]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int kstatus_t;
enum {K_OK=0,K_EINVAL=-1,K_EOVERFLOW=-2,K_ENOMEM=-3};
typedef struct {uint8_t *data;uint64_t size,data_capacity;bool directory;} vfs_backend_node_t;
static size_t live_bytes;static unsigned live,allocs,frees,stats;
static bool fail;
typedef struct {size_t size;} allocation_t;
static void *kmalloc(size_t n,int flags){
 assert(!flags);++allocs;if(fail)return NULL;
 allocation_t *a=malloc(sizeof(*a)+n);assert(a);a->size=n;
 ++live;live_bytes+=n;return a+1;
}
static void *kzalloc(size_t n,int flags){void *p=kmalloc(n,flags);if(p)memset(p,0,n);return p;}
static void kfree(void *p){
 if(!p)return;allocation_t *a=(allocation_t*)p-1;
 assert(live && live_bytes>=a->size);--live;live_bytes-=a->size;++frees;free(a);
}
static bool kmalloc_try_grow(void *p,size_t n){(void)p;(void)n;return false;}
static kstatus_t page_memory_get_stats(uint64_t *t,uint64_t *f){
 ++stats;*t=1000;*f=500;return K_OK;
}
''' + code + r'''
int main(void){
 /* Explicit small EOFs must be exact too, including the smallest payload.
  * OOM must not publish EOF/capacity, and regrowth must clear stale bytes. */
 const unsigned small_sizes[]={1,17,40,41,255,1984,1992,1993,2048,4040,4095,4096};
 for(unsigned i=0;i<sizeof(small_sizes)/sizeof(*small_sizes);++i){
  unsigned size=small_sizes[i];vfs_backend_node_t small={0};
  unsigned before=allocs;fail=true;
  assert(vfs_memory_backend_truncate(&small,size)==K_ENOMEM);
  assert(!small.data && !small.size && !small.data_capacity && !live);
  assert(allocs==before+1);
  fail=false;
  assert(vfs_memory_backend_truncate(&small,size)==K_OK);
  assert(small.size==size && small.data_capacity==size && live_bytes==size);
  for(unsigned j=0;j<size;++j)assert(small.data[j]==0);
  memset(small.data,0x65,size);
  if(size>1){
   before=allocs;
   assert(vfs_memory_backend_truncate(&small,1)==K_OK);
   assert(vfs_memory_backend_truncate(&small,size)==K_OK && allocs==before);
   assert(small.data[0]==0x65);
   for(unsigned j=1;j<size;++j)assert(small.data[j]==0);
  }
  assert(vfs_memory_backend_truncate(&small,0)==K_OK && !live && !live_bytes);
 }
 assert(stats==1 && g_vfs_memory_allocation_failure.failures==12);
 assert(g_vfs_memory_allocation_failure.state==2 &&
        g_vfs_memory_allocation_failure.required==1 &&
        g_vfs_memory_allocation_failure.attempted_capacity==1);
 memset(&g_vfs_memory_allocation_failure,0,sizeof(g_vfs_memory_allocation_failure));
 stats=0;
 const unsigned capacities[]={4096,8192,16384,1048576,3145728};
 unsigned cases=0;
 for(unsigned c=0;c<sizeof(capacities)/sizeof(*capacities);++c)
 for(unsigned allocation_failure=0;allocation_failure<2;++allocation_failure){
  unsigned capacity=capacities[c];
  const unsigned sizes[]={0,1,17,4095,4096,capacity/4-1,capacity/4,capacity/4+1,capacity/2,capacity-1,capacity};
  for(unsigned s=0;s<sizeof(sizes)/sizeof(*sizes);++s){
   unsigned size=sizes[s];fail=false;
   vfs_backend_node_t b={.size=capacity,.data_capacity=capacity};
   b.data=kmalloc(capacity,0);
   for(unsigned i=0;i<capacity;++i)b.data[i]=(uint8_t)(i*19+7);
   uint8_t *old=b.data;unsigned before=allocs,releases=frees;
   bool attempt=size>0 && size<capacity && capacity>4096 && size<=capacity/4;
   fail=allocation_failure;
   assert(vfs_memory_backend_truncate(&b,size)==K_OK && b.size==size);
   assert(allocs==before+attempt);
   unsigned expected_capacity=capacity;
   if(!size){assert(!b.data && !b.data_capacity);expected_capacity=0;}
   else if(attempt && !fail){
    expected_capacity=size;
    assert(b.data!=old);
   }else assert(b.data==old);
   assert(b.data_capacity==expected_capacity && live_bytes==expected_capacity);
   assert(frees==releases+(!size || (attempt&&!fail)));
   for(unsigned i=0;i<size;++i)assert(b.data[i]==(uint8_t)(i*19+7));
   assert(!stats && !g_vfs_memory_allocation_failure.failures);
   // Identical EOF and small decrements must not churn the allocation.
   before=allocs;
   assert(vfs_memory_backend_truncate(&b,size)==K_OK && allocs==before);
   fail=false;
   if(size && size<capacity){
    assert(vfs_memory_backend_truncate(&b,capacity)==K_OK);
    for(unsigned i=0;i<size;++i)assert(b.data[i]==(uint8_t)(i*19+7));
    for(unsigned i=size;i<capacity;++i)assert(b.data[i]==0);
   }
   assert(vfs_memory_backend_truncate(&b,0)==K_OK);
   assert(!live && !live_bytes);++cases;
  }
 }
 // Repeated journal-like grow/shrink cycles must release the large storage.
 vfs_backend_node_t b={0};
 for(unsigned i=0;i<64;++i){
  assert(vfs_memory_backend_truncate(&b,1048576)==K_OK && live_bytes==1048576);
  assert(vfs_memory_backend_truncate(&b,17)==K_OK && live_bytes==17);
 }
 assert(vfs_memory_backend_truncate(&b,0)==K_OK && !live_bytes && !live);
 printf("RAM shrink: 12 small exact-size/OOM/regrow cases, %u boundary/OOM/regrow cases, 64 cycles, preserved prefix, zero tail and balanced ownership passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-memory-shrink-') as directory:
    path = Path(directory)
    def run(text):
        (path/'test.c').write_text(text)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-Wno-misleading-indentation','-fsanitize=address,undefined',
                        '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
        return subprocess.run([str(path/'test')],capture_output=True,text=True,
                              timeout=20,preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    result = run(program)
    assert result.returncode == 0, result.stderr
    print(result.stdout,end='')
    negative = program.replace('size <= backend->data_capacity / 4U)',
                               'false && size <= backend->data_capacity / 4U)')
    assert negative != program
    result = run(negative)
    assert result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: large truncated files retain their old capacity')
    negative = program.replace('uint64_t capacity = size;',
                               'uint64_t capacity = 4096U; while (capacity < size) capacity *= 2U;')
    assert negative != program
    result = run(negative)
    assert result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: shrink unnecessarily rounds capacity up')
