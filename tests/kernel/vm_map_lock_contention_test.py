"""Exercise production VM locking, with only privileged CPU operations mocked."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/vm_space.c').read_text()
code=source[source.index('void map_lock('):source.index('static void vm_global_initialize(')]
replacements={
 '__asm__ volatile ("pushfq; popq %0" : "=r"(saved_flags) : : "memory");':'saved_flags = irq_flags;',
 '__asm__ volatile ("sti" : : : "memory");':'irq_flags |= 512;',
 '__asm__ volatile ("cli" : : : "memory");':'irq_flags &= ~512ULL;',
 '__asm__ volatile ("pause");':'test_pause();',
 'atomic_compare_exchange_strong_explicit(':'test_acquire(',
}
for before,after in replacements.items():
    assert before in code
    code=code.replace(before,after)
program=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
typedef struct { uint64_t tid; } thread_t;
typedef struct {struct {atomic_uint state;} map_lock;struct {uint64_t value;} root_table;uint64_t map_lock_started,map_lock_caller,map_lock_tid;} vm_space_t;
static _Thread_local int preempt,waiting,begins,ends;
static _Thread_local uint64_t irq_flags;
static _Thread_local thread_t thread;
static atomic_uint attempts,pauses,entered;
static vm_space_t space;
static uint64_t protected_value;
static bool test_acquire(atomic_uint* state,unsigned* expected,unsigned desired,memory_order a,memory_order b){
 atomic_fetch_add(&attempts,1);return atomic_compare_exchange_strong_explicit(state,expected,desired,a,b);
}
static void test_pause(void){assert(preempt>0 && waiting && (irq_flags&512));atomic_fetch_add(&pauses,1);sched_yield();}
static void sched_preempt_disable(void){++preempt;}
static void sched_preempt_enable(void){assert(preempt>0);--preempt;}
static void x86_tlb_wait_begin(void){assert(preempt>0 && !waiting);waiting=1;++begins;atomic_fetch_add(&entered,1);}
static void x86_tlb_wait_end(void){assert(preempt>0 && waiting);waiting=0;++ends;}
static uint64_t x86_read_tsc(void){return 0;}
static thread_t* sched_current_thread(void){return &thread;}
static void vm_lock_record(unsigned a,uint64_t b,uint64_t c,uint64_t d,uint64_t e){(void)a;(void)b;(void)c;(void)d;(void)e;}
'''+code+r'''
static void* contender(void* arg){
 uint64_t flags=(uintptr_t)arg;irq_flags=flags;preempt=1;thread.tid=flags+1;
 map_lock(&space);assert(preempt==2 && !waiting && irq_flags==flags);
 ++protected_value;map_unlock(&space);assert(preempt==1 && begins==ends);
 for(int i=0;i<4000;i++){
   map_lock(&space);assert(preempt==2 && !waiting && irq_flags==flags);
   ++protected_value;map_unlock(&space);assert(preempt==1);
 }
 assert(begins==ends && !waiting && irq_flags==flags);return 0;
}
int main(void){
 irq_flags=0;map_lock(&space);assert(preempt==1 && !waiting && begins==0);
 map_unlock(&space);assert(preempt==0 && irq_flags==0);
 map_lock(&space);pthread_t workers[4];
 for(int i=0;i<4;i++)assert(!pthread_create(&workers[i],0,contender,(void*)(uintptr_t)((i&1)?512:0)));
 while(atomic_load(&entered)<4)sched_yield();
 unsigned start=atomic_load(&pauses);
 while(atomic_load(&pauses)<start+10000)sched_yield();
 /* 每个等待 CPU 只进行一次比较失败，而不是每次暂停都交换。 */
 assert(atomic_load(&attempts)==6);
 map_unlock(&space);
 for(int i=0;i<4;i++)assert(!pthread_join(workers[i],0));
 assert(protected_value==16004 && preempt==0 && atomic_load(&space.map_lock.state)==0);
 printf("VM lock: 16004 protected updates, IF restoration, nested preemption/TLB pairing; 10000+ held-lock polls with 4 failed acquire attempts passed\n");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)
