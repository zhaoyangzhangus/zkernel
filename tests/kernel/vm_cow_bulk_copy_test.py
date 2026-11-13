"""Production shadow/COW page paths linked to actual kernel bulk memcpy."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"
typedef struct {page_t page;unsigned char bytes[PAGE_SIZE+32];} test_page_t;
static atomic_uint allocations,frees,nodes,node_frees,node_attempts;
static _Thread_local unsigned depth;
static bool fail_page,fail_node,fail_direct;
static bool shadow_phase=true,race,cow_race;
static _Thread_local bool slow_creator;
static atomic_bool winner_claimed,winner_written;
static pthread_barrier_t barrier;
static void (*allocation_hook)(void);
extern void *liteos_memcpy(void *,const void *,size_t);
void *cow_test_memcpy(void *dst,const void *src,size_t length) {
 assert(length==PAGE_SIZE);
 if(shadow_phase)assert(!depth);else assert(depth);
 return liteos_memcpy(dst,src,length);
}
void sched_preempt_disable(void) {++depth;}
void sched_preempt_enable(void) {assert(depth);--depth;}
page_t *page_alloc(uint8_t order,page_alloc_flags_t flags) {
 assert((flags&PAGE_ALLOC_ZERO)==0);
 assert(!depth);
 if(allocation_hook){void (*hook)(void)=allocation_hook;allocation_hook=0;hook();}
 if(race){
  int rc=pthread_barrier_wait(&barrier);assert(rc==0 || rc==PTHREAD_BARRIER_SERIAL_THREAD);
  if(slow_creator)while(!atomic_load_explicit(&winner_written,memory_order_acquire))sched_yield();
 }
 assert(!order);if(fail_page)return 0;
 test_page_t *p=calloc(1,sizeof(*p));assert(p);atomic_init(&p->page.refs,1);
 memset(p->bytes,0x75,sizeof(p->bytes));memset(p->bytes+16,(flags&PAGE_ALLOC_ZERO)?0:0xcc,PAGE_SIZE);
 atomic_fetch_add(&allocations,1);return &p->page;
}
void page_free(page_t *page) {
 assert(!depth);
 test_page_t *p=(void*)page;
 for(unsigned i=0;i<16;++i)assert(p->bytes[i]==0x75 && p->bytes[PAGE_SIZE+16+i]==0x75);
 atomic_fetch_add(&frees,1);free(p);
}
paddr_t page_to_phys(const page_t *page) {const test_page_t *p=(const void*)page;return (paddr_t){(uintptr_t)(p->bytes+16)};}
void *phys_to_direct(paddr_t address) {return fail_direct?0:(void*)(uintptr_t)address.value;}
void *kmalloc(size_t n,uint32_t flags) {if(shadow_phase)assert(!depth);(void)flags;atomic_fetch_add(&node_attempts,1);if(fail_node)return 0;void *p=calloc(1,n);assert(p);atomic_fetch_add(&nodes,1);return p;}
void kfree(void *p) {if(shadow_phase)assert(!depth);if(p)atomic_fetch_add(&node_frees,1);free(p);}
int kernel_serial_printf_serial_only(const char *format,...) {(void)format;return 0;}
static anon_store_t store;
static vm_object_t object;
static vm_area_t area;
static paddr_t source_address;
static page_t *results[8];
static page_t *hook_page;
static void publish_during_failed_candidate(void) {
 assert(!depth);
 bool page_failure=fail_page,node_failure=fail_node,direct_failure=fail_direct;
 fail_page=fail_node=fail_direct=false;
 assert(private_file_shadow_page(&area,77,source_address,&hook_page)==K_OK);
 ((unsigned char*)phys_to_direct(page_to_phys(hook_page)))[123]=0x91;
 fail_page=page_failure;fail_node=node_failure;fail_direct=direct_failure;
}
static void become_exclusive(void) {assert(!depth);assert(atomic_fetch_sub(&hook_page->refs,1)==2);}
static void discard_during_allocation(void) {assert(!depth);assert(anon_page_discard_range(&object,33,34)==K_OK);}
static void *worker(void *opaque) {
 unsigned i=*(unsigned*)opaque;
 slow_creator=i==7;
 if(cow_race)assert(anon_page_cow(&object,2,&results[i])==K_OK && !depth);
 else assert(private_file_shadow_page(&area,2,source_address,&results[i])==K_OK && !depth);
 if(!atomic_exchange(&winner_claimed,true)) {
  ((unsigned char*)phys_to_direct(page_to_phys(results[i])))[123]=cow_race?0x24:0x42;
  atomic_store_explicit(&winner_written,true,memory_order_release);
 }
 return 0;
}
int main(void) {
 spinlock_init(&store.lock);object.type=VM_OBJECT_ANON;object.u.anon.anon_root=&store;
 vm_object_stats_init(&object);area.private_object=&object;
 page_t *source=page_alloc(0,0);source_address=page_to_phys(source);
 unsigned char expected[PAGE_SIZE];
 for(unsigned i=0;i<PAGE_SIZE;++i)expected[i]=(unsigned char)(i*17+3);
 memcpy(phys_to_direct(source_address),expected,PAGE_SIZE);
 page_t *out=(void*)1;
 unsigned attempts=atomic_load(&node_attempts);
 fail_page=true;assert(private_file_shadow_page(&area,17,source_address,&out)==K_ENOMEM && out==(void*)1);fail_page=false;
 assert(atomic_load(&node_attempts)==attempts);
 fail_node=true;assert(private_file_shadow_page(&area,17,source_address,&out)==K_ENOMEM);fail_node=false;
 fail_direct=true;assert(private_file_shadow_page(&area,17,source_address,&out)==K_EIO);fail_direct=false;
 assert(!store.pages && !depth);
 /* 候选分配失败时仍必须发现并发成功的发布者；页面 OOM 后不能再分配节点，也不能覆盖
  * 成功者的数据。 */
 for(unsigned failure=0;failure<3;++failure) {
  attempts=atomic_load(&node_attempts);
  fail_page=failure==0;fail_node=failure==1;fail_direct=failure==2;
  allocation_hook=publish_during_failed_candidate;out=(void*)1;
  assert(private_file_shadow_page(&area,77,source_address,&out)==K_OK && out==hook_page);
  fail_page=fail_node=fail_direct=false;
  assert(!allocation_hook && !depth && vm_object_populated_pages(&object)==1);
  assert(atomic_load(&node_attempts)==attempts+1+(failure!=0));
  unsigned char *bytes=phys_to_direct(page_to_phys(out));
  for(unsigned i=0;i<PAGE_SIZE;++i)assert(bytes[i]==(i==123?0x91:expected[i]));
  assert(anon_page_discard_range(&object,77,78)==K_OK);
  assert(!store.pages && !store.page_index.root && vm_object_populated_pages(&object)==0);
 }
 assert(private_file_shadow_page(&area,17,source_address,&out)==K_OK);
 assert(vm_object_populated_pages(&object)==1);
 page_t *original=out;assert(!memcmp(phys_to_direct(page_to_phys(out)),expected,PAGE_SIZE));
 unsigned before=atomic_load(&allocations);
 assert(private_file_shadow_page(&area,17,source_address,&out)==K_OK && out==original && atomic_load(&allocations)==before);
 shadow_phase=false;
 assert(anon_page_cow(&object,17,&out)==K_OK && out==original && atomic_load(&allocations)==before);
 atomic_store(&original->refs,2);out=(void*)1;
 fail_page=true;assert(anon_page_cow(&object,17,&out)==K_ENOMEM && out==(void*)1);fail_page=false;
 fail_direct=true;assert(anon_page_cow(&object,17,&out)==K_EIO && out==(void*)1);fail_direct=false;
 assert(atomic_load(&original->refs)==2);
 assert(anon_page_cow(&object,17,&out)==K_OK && out!=original && atomic_load(&original->refs)==1);
 assert(out->owner==PAGE_OWNER_ANON && !memcmp(phys_to_direct(page_to_phys(out)),expected,PAGE_SIZE));
 ((unsigned char*)phys_to_direct(page_to_phys(out)))[123]^=1;
 assert(!memcmp(phys_to_direct(page_to_phys(original)),expected,PAGE_SIZE));
 assert(!memcmp(phys_to_direct(source_address),expected,PAGE_SIZE));
 /* 第二次查找必须容忍节点已消失或页面现已独占的情况；页面独占时即使分配失败也无妨，
  * 因为根本不需要分配。 */
 hook_page=out;atomic_store(&hook_page->refs,2);
 allocation_hook=become_exclusive;fail_page=true;
 assert(anon_page_cow(&object,17,&out)==K_OK && out==hook_page);
 fail_page=false;assert(atomic_load(&hook_page->refs)==1);
 shadow_phase=true;
 assert(private_file_shadow_page(&area,33,source_address,&out)==K_OK);
 hook_page=out;atomic_store(&hook_page->refs,2);
 allocation_hook=discard_during_allocation;shadow_phase=false;out=(void*)1;
 assert(anon_page_cow(&object,33,&out)==K_ENOENT && out==(void*)1);
 assert(atomic_load(&hook_page->refs)==1);page_free(hook_page);
 assert(vm_object_populated_pages(&object)==1);
 assert(anon_page_cow(0,17,&out)==K_EINVAL);
 assert(anon_page_cow(&object,17,0)==K_EINVAL);
 /* 保留文件尾部的零填充区域，不必先清除候选页面。 */
 shadow_phase=true;
 memset(expected+PAGE_SIZE-37,0,37);
 memcpy(phys_to_direct(source_address),expected,PAGE_SIZE);
 assert(private_file_shadow_page(&area,99,source_address,&out)==K_OK);
 assert(!memcmp(phys_to_direct(page_to_phys(out)),expected,PAGE_SIZE));
 assert(vm_object_populated_pages(&object)==2);
 assert(anon_page_discard_range(&object,99,100)==K_OK);
 assert(vm_object_populated_pages(&object)==1);
 shadow_phase=true;race=true;assert(!pthread_barrier_init(&barrier,0,8));
 pthread_t threads[8];unsigned indexes[8];
 for(unsigned i=0;i<8;++i){indexes[i]=i;assert(!pthread_create(&threads[i],0,worker,&indexes[i]));}
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 race=false;assert(!pthread_barrier_destroy(&barrier));
 assert(vm_object_populated_pages(&object)==2);
 for(unsigned i=0;i<8;++i)assert(results[i]==results[0]);
 expected[123]=0x42;
 assert(!memcmp(phys_to_direct(page_to_phys(results[0])),expected,PAGE_SIZE));
 page_t *cow_original=results[0];
 atomic_fetch_add(&cow_original->refs,1);
 shadow_phase=false;cow_race=race=true;winner_claimed=winner_written=false;
 assert(!pthread_barrier_init(&barrier,0,8));
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,worker,&indexes[i]));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 race=cow_race=false;assert(!pthread_barrier_destroy(&barrier));
 for(unsigned i=0;i<8;++i)assert(results[i]==results[0] && results[i]!=cow_original);
 assert(atomic_load(&cow_original->refs)==1 && vm_object_populated_pages(&object)==2);
 assert(((unsigned char*)phys_to_direct(page_to_phys(cow_original)))[123]==0x42);
 expected[123]=0x24;
 assert(!memcmp(phys_to_direct(page_to_phys(results[0])),expected,PAGE_SIZE));
 assert(anon_page_discard_range(&object,0,UINT64_MAX)==K_OK && !store.pages && !store.page_index.root);
 assert(vm_object_populated_pages(&object)==0);
 page_free(original);page_free(cow_original);page_free(source);
 assert(atomic_load(&allocations)==atomic_load(&frees) && atomic_load(&nodes)==atomic_load(&node_frees) && !depth);
 puts("VM bulk COW: off-store-lock shadow allocation/copy/free, complete 4096-byte copy, no-copy hits/exclusive pages, OOM/direct-map failures, original-page isolation, eight-way publication with delayed loser preserving winner writes, exact populated/discard counters and balanced lifetime/canaries passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-cow-bulk-') as directory:
    path = Path(directory)
    objects = []
    rename = ['-Dmemcpy=liteos_memcpy','-Dmemmove=liteos_memmove',
              '-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp']
    for name, source in [('object','kernel/mm/object.c'),('string','kernel/string.c')]:
        obj = str(path/(name+'.o'));objects.append(obj)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-ffunction-sections','-fdata-sections','-fno-builtin',
                        '-mno-sse','-mno-mmx','-fsanitize=address,undefined',
                        '-fno-pie','-I'+str(root/'include'),
                        *( ['-Dmemcpy=cow_test_memcpy',*rename[1:]] if name=='object' else rename),
                        '-c',str(root/source),'-o',obj],check=True)
    binary = str(path/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/mm'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-Wl,--gc-sections','-x','c','-','-x','none',*objects,'-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
