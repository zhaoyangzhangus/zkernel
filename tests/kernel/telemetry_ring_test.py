"""Production telemetry ring: concurrent initialization, writers and snapshots."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/trace.c').read_text()
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>
static atomic_uint init_attempts;
static _Bool parallel_init;
static _Thread_local unsigned preempt_depth, caller_depth;
void sched_preempt_disable(void) { ++preempt_depth; }
void sched_preempt_enable(void) { assert(preempt_depth); --preempt_depth; }
static _Bool checked_acquire(atomic_uint *p,unsigned *expected,unsigned value,
                            memory_order success,memory_order failure) {
 assert(preempt_depth==caller_depth+1);
 return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
#pragma push_macro("atomic_compare_exchange_strong_explicit")
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit checked_acquire
#include <kernel/spinlock.h>
#pragma pop_macro("atomic_compare_exchange_strong_explicit")
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
 atomic_fetch_add(&init_attempts,1);
 if(parallel_init)while(atomic_load(&init_attempts)<8)sched_yield();
 return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source.replace('atomic_compare_exchange_strong_explicit(&g_telemetry.init_state',
                     'counted_cas(&g_telemetry.init_state') + r'''
x86_cpu_features_t x86_boot_cpu_features={.tsc_hz=1000000000};
static _Thread_local unsigned cpu;
static atomic_uint_fast64_t clock_value;
uint64_t x86_read_tsc(void) {return atomic_fetch_add(&clock_value,1);}
uint64_t x86_tsc_to_ns(uint64_t ticks) {return ticks;}
uint32_t x86_current_cpu_index(void) {
 assert(preempt_depth==caller_depth+1 && atomic_load(&g_telemetry.lock.state)); return cpu;
}
static void *initializer(void *unused) {
 (void)unused;assert(telemetry_init());
 assert(!g_telemetry.next_sequence);
 for(unsigned i=0;i<TELEMETRY_RING_CAPACITY;++i)assert(!g_telemetry.records[i].sequence);
 return 0;
}
static atomic_uint writers_done;
static void *writer(void *argument) {
 cpu=*(unsigned*)argument;
 for(unsigned i=0;i<20000;++i) {
  uint64_t id=(uint64_t)cpu*20000+i;
  assert(telemetry_record(7,id,id*17,~id)==K_OK);
  assert(!preempt_depth);
 }
 atomic_fetch_add(&writers_done,1);return 0;
}
static void check_records(telemetry_record_t *records,size_t count) {
 for(size_t i=0;i<count;++i) {
  assert(records[i].category==7 && records[i].value0==records[i].object_id*17);
  assert(records[i].value1==~records[i].object_id && records[i].cpu==records[i].object_id/20000);
  if(i)assert(records[i].sequence==records[i-1].sequence+1);
 }
}
int main(void) {
 pthread_t threads[8];parallel_init=true;
 g_telemetry.next_sequence=123;
 for(unsigned i=0;i<TELEMETRY_RING_CAPACITY;++i)g_telemetry.records[i].sequence=123;
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 parallel_init=false;assert(atomic_load(&init_attempts)==8 && telemetry_self_test());
 telemetry_record_t records[TELEMETRY_RING_CAPACITY];uint64_t next=123;
 assert(telemetry_record(0,0,0,0)==K_EINVAL);
 assert(!telemetry_snapshot(0,0,1,&next) && next==123);
 assert(!telemetry_snapshot(0,records,0,&next) && next==123);
 unsigned ids[4]={0,1,2,3};
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,writer,&ids[i]));
 while(atomic_load(&writers_done)<4) {
  size_t count=telemetry_snapshot(0,records,TELEMETRY_RING_CAPACITY,&next);
  /* 较早的快照中可能仍包含生产自测生成的记录。 */
  size_t start=0;while(start<count && records[start].category!=7)++start;
  check_records(records+start,count-start);
 }
 for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 assert(g_telemetry.next_sequence==80002);
 size_t count=telemetry_snapshot(0,records,TELEMETRY_RING_CAPACITY,&next);
 assert(count==TELEMETRY_RING_CAPACITY && next==80002 && records[0].sequence==80003-TELEMETRY_RING_CAPACITY);
 check_records(records,count);
 for(unsigned capacity=1;capacity<=TELEMETRY_RING_CAPACITY;++capacity) {
  assert(telemetry_snapshot(next-TELEMETRY_RING_CAPACITY,records,capacity,0)==capacity);
  check_records(records,capacity);
 }
 assert(!telemetry_snapshot(UINT64_MAX,records,1,&next) && next==80002);
 assert(!telemetry_snapshot(next,records,1,0));
 assert(!telemetry_snapshot(next+1,records,1,0));
 for(caller_depth=0;caller_depth<32;++caller_depth) {
  preempt_depth=caller_depth;
  uint64_t before=g_telemetry.next_sequence, untouched=123;
  assert(telemetry_record(0,0,0,0)==K_EINVAL && preempt_depth==caller_depth);
  assert(!telemetry_snapshot(0,0,1,&untouched) && untouched==123 && preempt_depth==caller_depth);
  assert(telemetry_record(7,0,0,UINT64_MAX)==K_OK && preempt_depth==caller_depth);
  assert(telemetry_snapshot(before,records,1,&next)==1 && next==before+1 && preempt_depth==caller_depth);
  assert(!telemetry_snapshot(UINT64_MAX,records,1,&next) && preempt_depth==caller_depth);
  g_telemetry.next_sequence=UINT64_MAX-1;
  assert(telemetry_record(7,0,0,UINT64_MAX)==K_OK && preempt_depth==caller_depth);
  assert(telemetry_record(7,0,0,UINT64_MAX)==K_EIO && preempt_depth==caller_depth);
  assert(telemetry_snapshot(UINT64_MAX-1,records,2,&next)==1 && preempt_depth==caller_depth);
  assert(!atomic_load(&g_telemetry.lock.state));
  g_telemetry.next_sequence=before+1;
 }
 caller_depth=preempt_depth=0;
 for(unsigned i=0;i<10000;++i)assert(telemetry_init());
 assert(atomic_load(&init_attempts)==8 && !atomic_load(&g_telemetry.lock.state));
 /* 最大序列号有效；递增越过它时必须终止快照，不能持有环锁并回绕到零。 */
 g_telemetry.next_sequence=UINT64_MAX-1;
 assert(telemetry_record(7,0,0,UINT64_MAX)==K_OK);
 assert(telemetry_record(7,0,0,UINT64_MAX)==K_EIO && !atomic_load(&g_telemetry.lock.state));
 alarm(2);
 assert(telemetry_snapshot(UINT64_MAX-1,records,2,&next)==1 && next==UINT64_MAX);
 alarm(0);assert(records[0].sequence==UINT64_MAX && !atomic_load(&g_telemetry.lock.state));
 assert(!preempt_depth);
 puts("Telemetry: eight concurrent initializers/publication, production self-test, 80000 concurrent writes with live snapshots, ring wrap/256 capacities, 32 nested guard/overflow/invalid/snapshot exits and initialized path without CAS passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-telemetry-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    negative = code.replace('    sched_preempt_disable();', '').replace('    sched_preempt_enable();', '')
    assert negative != code
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=negative,text=True,check=True)
    failure = subprocess.run([binary],capture_output=True,text=True,timeout=30)
    assert failure.returncode < 0 and 'preempt_depth' in failure.stderr
    print('Regression control: removing telemetry preemption guard fails protected acquisition')
