"""Run production root-stripe locks with concurrent owners and TLB delivery."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/arch/x86_64/mm/page_table.c').read_text()
begin=source.index('#define PAGE_TABLE_USER_GUARDS')
end=source.index('void x86_sync_kernel_half(')
code=source[begin:end].replace('static uint64_t *table_from_physical(uint64_t physical);','')
code=code.replace('__asm__ volatile ("pushfq; popq %0" : "=r"(saved_flags) : : "memory");','saved_flags=local_flags;')
code=code.replace('__asm__ volatile ("sti" : : : "memory");','local_flags=512;')
code=code.replace('__asm__ volatile ("cli" : : : "memory");','local_flags=0;')
code=code.replace('atomic_load_explicit(&guard->lock.state,\n                                       memory_order_relaxed)',
                  'checked_poll(&guard->lock.state, memory_order_relaxed)')
# 检查所有生产入口中的保护路由，包括共享的高半地址区域。
routes={'x86_sync_kernel_half':'kernel_root, UINT64_MAX','x86_map_page':'root, va',
        'x86_translate_page':'root, va','x86_unmap_page':'root, va',
        'x86_unmap_user_page_batch':'root, 0U','x86_page_range_has_present_pages':'root, begin',
        'x86_next_present_page':'root, start','x86_protect_user_page_deferred':'root, va',
        'x86_protect_next_user_page_deferred':'root, start','x86_protect_page':'root, va'}
for name,args in routes.items():
    start=source.index(' '+name+'(')
    brace=source.index('{',start);depth=1;finish=brace+1
    while depth:
        depth+=(source[finish]=='{')-(source[finish]=='}');finish+=1
    body=source[brace:finish]
    assert body.count('table_lock(')==1 and 'table_lock('+args+');' in body,name
program=r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <kernel/base.h>
#define X86_64_USER_TOP 0x7fffffffffffULL
static _Thread_local unsigned cpu,depth;
static _Thread_local uint64_t local_flags=512;
static _Thread_local bool waiting;
static atomic_uint attempts,polls;
static bool checked_cas(atomic_uint *p,unsigned *expected,unsigned desired,
                        memory_order success,memory_order failure) {
 assert(depth);atomic_fetch_add(&attempts,1);
 return atomic_compare_exchange_strong_explicit(p,expected,desired,success,failure);
}
static unsigned checked_poll(atomic_uint *p,memory_order order) {
 assert(depth && waiting && (local_flags&512));atomic_fetch_add(&polls,1);
 return atomic_load_explicit(p,order);
}
#pragma push_macro("atomic_compare_exchange_strong_explicit")
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit checked_cas
#include <kernel/spinlock.h>
#pragma pop_macro("atomic_compare_exchange_strong_explicit")
static unsigned x86_current_cpu_index(void) { return cpu; }
static void sched_preempt_disable(void) { ++depth; }
static void sched_preempt_enable(void) { assert(depth); --depth; }
static void x86_tlb_wait_begin(void) { assert(depth && !waiting);waiting=true; }
static void x86_tlb_wait_end(void) { assert(depth && waiting);waiting=false; }
''' + code + r'''
static atomic_uint entered,started;
static paddr_t contender_root;
static uint64_t contender_address;
static unsigned contender_nesting;
static uint64_t contender_flags;
static void *one_lock(void *arg) {
 (void)arg;cpu=1;local_flags=contender_flags;depth=contender_nesting;
 atomic_store(&started,1);table_lock(contender_root,contender_address);
 assert(depth==contender_nesting+1 && !waiting && local_flags==contender_flags);
 atomic_store(&entered,1);table_unlock();assert(depth==contender_nesting);depth=0;return 0;
}
static void bounded_wait(atomic_uint *value) {
 struct timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);
 while(!atomic_load(value)) {
  sched_yield();clock_gettime(CLOCK_MONOTONIC,&now);
  assert(now.tv_sec-start.tv_sec<3);
 }
}
static void paired(paddr_t left,paddr_t right,uint64_t address,bool independent) {
 atomic_store(&entered,0);atomic_store(&started,0);
 uint64_t previous=atomic_load(&g_page_table_wait_count);
 table_lock(left,address);assert(depth==1);
 contender_root=right;contender_address=address;pthread_t thread;
 assert(!pthread_create(&thread,0,one_lock,0));bounded_wait(&started);
 if(independent) bounded_wait(&entered);
 else {
  struct timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);
  while(atomic_load(&g_page_table_wait_count)==previous) {
   sched_yield();clock_gettime(CLOCK_MONOTONIC,&now);assert(now.tv_sec-start.tv_sec<3);
  }
  assert(!atomic_load(&entered));
 }
 table_unlock();assert(!depth);assert(!pthread_join(thread,0));assert(atomic_load(&entered));
}
static unsigned visits[PAGE_TABLE_USER_GUARDS+1];
static atomic_uint active[PAGE_TABLE_USER_GUARDS+1];
static void *stress(void *arg) {
 cpu=(unsigned)(uintptr_t)arg;
 for(unsigned i=0;i<5000;++i) {
  paddr_t root={(1U+(i%80))*4096ULL};uint64_t va=i%7==0?UINT64_MAX:0x1000;
  unsigned slot=table_guard_index(root,va);table_lock(root,va);
  assert(atomic_fetch_add(&active[slot],1)==0);++visits[slot];
  assert(atomic_fetch_sub(&active[slot],1)==1);table_unlock();assert(!depth && !waiting);
 }
 return 0;
}
int main(void) {
 x86_page_table_arch_init();paddr_t left={4096},right={8192},collision={0};
 assert(table_guard_index(left,0)!=table_guard_index(right,0));
 for(unsigned i=2;i<8192;++i) {
  paddr_t candidate={i*4096ULL};
  if(table_guard_index(candidate,0)==table_guard_index(left,0)) {collision=candidate;break;}
 }
 assert(collision.value);paired(left,right,0,true);
 paired(left,left,0,false);paired(left,collision,0,false);
 paired(left,right,UINT64_MAX,false); // Shared kernel half across roots.
 for(contender_nesting=0;contender_nesting<32;++contender_nesting) {
  for(unsigned enabled=0;enabled<2;++enabled) {
   contender_flags=enabled?512:0;paired(left,left,0,false);
  }
 }
 contender_nesting=0;contender_flags=0;
 atomic_store(&attempts,0);atomic_store(&polls,0);
 uint64_t before=atomic_load(&g_page_table_wait_count);
 table_lock(left,0);contender_root=left;contender_address=0;
 atomic_store(&entered,0);atomic_store(&started,0);pthread_t contender;
 assert(!pthread_create(&contender,0,one_lock,0));
 struct timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);
 while(atomic_load(&polls)<20000) {
  sched_yield();clock_gettime(CLOCK_MONOTONIC,&now);assert(now.tv_sec-start.tv_sec<3);
 }
 assert(atomic_load(&attempts)==2 && !atomic_load(&entered));
 assert(atomic_load(&g_page_table_wait_count)==before+1);
 table_unlock();assert(!pthread_join(contender,0));assert(atomic_load(&attempts)==3);
 sched_preempt_disable();table_lock(left,0);assert(depth==2);
 table_unlock();assert(depth==1);sched_preempt_enable();
 pthread_t threads[4];
 for(unsigned i=0;i<4;++i) assert(!pthread_create(&threads[i],0,stress,(void*)(uintptr_t)i));
 for(unsigned i=0;i<4;++i) assert(!pthread_join(threads[i],0));
 unsigned total=0;for(unsigned i=0;i<=PAGE_TABLE_USER_GUARDS;++i) {
  total+=visits[i];assert(!atomic_load(&active[i]));
  assert(!atomic_load(&g_page_table_guards[i].lock.state));
  assert(atomic_load(&g_page_table_guards[i].owner)==UINT32_MAX);
 }
 assert(total==20000);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-page-table-guards-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)
print('Production page-table guards: all entry routing, independent-root progress, same-root/collision/kernel exclusion, 32 nested depths/both IRQ states, 20000 protected visits and 20000 held-lock reads with one failed acquisition passed')
