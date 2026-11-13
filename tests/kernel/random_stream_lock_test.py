"""Actual random blocks/syscall: short reservation, output compatibility and races."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/random.c').read_text()
rounds = source[source.index('static uint32_t random_rotate_left('):
                source.index('static bool random_rdrand_supported(')]
rounds = rounds.replace('uint32_t c, uint32_t d) {',
                        'uint32_t c, uint32_t d) { assert(!preempt_depth);')
body = source[source.index('static void random_lock('):]
assert 'volatile uint32_t *clear = words;' in body
assert 'random_clear_words(state);' in body and 'random_clear_words(working);' in body
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#define __user
#define K_OK 0
#define K_EACCES -13
#define K_ENOSYS -38
typedef int kstatus_t;
static atomic_flag g_random_lock=ATOMIC_FLAG_INIT;
static uint32_t g_random_state[16],reference[16];
static bool g_random_ready,g_random_strong,range_valid=true;
static atomic_uint pins;
static _Thread_local unsigned preempt_depth,copy_calls,fail_copy;
static _Thread_local bool reenter,in_nested;
static _Thread_local uint8_t nested_output[65];
static void sched_preempt_disable(void) { assert(!preempt_depth);++preempt_depth;++pins; }
static void sched_preempt_enable(void) { assert(preempt_depth==1);--preempt_depth; }
static bool x86_user_range_valid(const void *p,size_t n) { assert(p && n);return range_valid; }
int64_t syscall_random_get(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static kstatus_t copy_to_user(void *p,const void *s,size_t n) {
 assert(!preempt_depth && n && n<=64);++copy_calls;
 if(fail_copy && copy_calls==fail_copy)return -5;
 if(reenter && !in_nested) {
  reenter=false;in_nested=true;
  assert(syscall_random_get((uintptr_t)nested_output,65,0,0,0,0)==65);
  in_nested=false;
 }
 memcpy(p,s,n);return K_OK;
}
/* 独立的标量基准，保留重构前的流/进位规则。 */
static uint32_t rol(uint32_t x,unsigned n) { return (x<<n)|(x>>(32-n)); }
static void quarter(uint32_t *w,unsigned a,unsigned b,unsigned c,unsigned d) {
 w[a]+=w[b];w[d]=rol(w[d]^w[a],16);w[c]+=w[d];w[b]=rol(w[b]^w[c],12);
 w[a]+=w[b];w[d]=rol(w[d]^w[a],8);w[c]+=w[d];w[b]=rol(w[b]^w[c],7);
}
static void oracle(uint8_t out[64]) {
 uint32_t w[16];memcpy(w,reference,sizeof(w));
 for(unsigned i=0;i<10;++i) {
  for(unsigned j=0;j<4;++j)quarter(w,j,j+4,j+8,j+12);
  quarter(w,0,5,10,15);quarter(w,1,6,11,12);
  quarter(w,2,7,8,13);quarter(w,3,4,9,14);
 }
 for(unsigned i=0;i<16;++i) {
  uint32_t value=w[i]+reference[i];
  for(unsigned j=0;j<4;++j)out[i*4+j]=(uint8_t)(value>>(8*j));
 }
 if(++reference[12]==0)++reference[13];
}
static void seed(unsigned variant) {
 for(unsigned i=0;i<16;++i)g_random_state[i]=0x13579bdfU+i*0x1234567U+variant;
 g_random_state[0]=0x61707865;g_random_state[1]=0x3320646e;
 g_random_state[2]=0x79622d32;g_random_state[3]=0x6b206574;
 g_random_state[12]=variant?UINT32_MAX:1;g_random_state[13]=UINT32_MAX;
 memcpy(reference,g_random_state,sizeof(reference));
 atomic_flag_clear(&g_random_lock);atomic_store(&pins,0);
 g_random_ready=g_random_strong=range_valid=true;
 copy_calls=fail_copy=0;reenter=in_nested=false;
}
'''
checks = r'''
enum { THREADS=8, BLOCKS=128 };
static uint8_t concurrent[THREADS][BLOCKS][64];
static void *worker(void *opaque) {
 unsigned id=(unsigned)(uintptr_t)opaque;
 for(unsigned i=0;i<BLOCKS;++i)
  assert(syscall_random_get((uintptr_t)concurrent[id][i],64,0,0,0,0)==64);
 assert(!preempt_depth);return NULL;
}
int main(void) {
 (void)sched_preempt_disable;
 uint32_t clear_probe[18];for(unsigned i=0;i<18;++i)clear_probe[i]=0xa5a5a5a5;
 random_clear_words(clear_probe+1);
 assert(clear_probe[0]==0xa5a5a5a5 && clear_probe[17]==0xa5a5a5a5);
 for(unsigned i=1;i<17;++i)assert(!clear_probe[i]);
 unsigned cases=0;
 for(unsigned variant=0;variant<2;++variant)for(unsigned length=0;length<=257;++length) {
  unsigned blocks=(length+63)/64;
  for(unsigned failure=0;failure<=blocks+1;++failure) {
   seed(variant);uint8_t actual[300],expected[300];memset(actual,0xa5,sizeof(actual));memset(expected,0xa5,sizeof(expected));
   fail_copy=failure;
   bool failed=failure && failure<=blocks;unsigned consumed=failed?failure:blocks;
   for(unsigned i=0;i<consumed;++i) {
    uint8_t block[64];oracle(block);
    if(failed && i+1==failure)continue;
    unsigned count=length-i*64;if(count>64)count=64;memcpy(expected+i*64,block,count);
   }
   assert(syscall_random_get((uintptr_t)actual,length,0,0,0,0)==(failed?-5:(int64_t)length));
   assert(!memcmp(actual,expected,sizeof(actual)) && !memcmp(reference,g_random_state,sizeof(reference)));
   assert(!preempt_depth && copy_calls==consumed && atomic_load(&pins)==consumed);++cases;
  }
 }
 seed(1);uint8_t outer[128],expected_outer[128],expected_nested[65],block[64];
 oracle(expected_outer);oracle(expected_nested);oracle(block);expected_nested[64]=block[0];oracle(expected_outer+64);
 reenter=true;
 assert(syscall_random_get((uintptr_t)outer,128,0,0,0,0)==128);
 assert(!memcmp(outer,expected_outer,128) && !memcmp(nested_output,expected_nested,65));
 assert(!memcmp(reference,g_random_state,sizeof(reference)) && atomic_load(&pins)==4 && !preempt_depth);
 seed(0);pthread_t threads[THREADS];
 for(unsigned i=0;i<THREADS;++i)assert(!pthread_create(threads+i,NULL,worker,(void*)(uintptr_t)i));
 for(unsigned i=0;i<THREADS;++i)assert(!pthread_join(threads[i],NULL));
 bool seen[THREADS][BLOCKS]={0};
 for(unsigned i=0;i<THREADS*BLOCKS;++i) {
  oracle(block);unsigned matches=0;
  for(unsigned t=0;t<THREADS;++t)for(unsigned b=0;b<BLOCKS;++b)
   if(!memcmp(concurrent[t][b],block,64)){assert(!seen[t][b]);seen[t][b]=true;++matches;}
  assert(matches==1);
 }
 assert(!memcmp(reference,g_random_state,sizeof(reference)) && atomic_load(&pins)==THREADS*BLOCKS);
 seed(0);uint8_t output[64];
 assert(syscall_random_get(0,0,0,0,0,0)==0);
 assert(syscall_random_get(0,1,0,0,0,0)==K_EACCES);
 assert(syscall_random_get(UINT64_MAX,2,0,0,0,0)==K_EACCES);
 assert(syscall_random_get((uintptr_t)output,(uint64_t)INT64_MAX+1,0,0,0,0)==K_EACCES);
 range_valid=false;assert(syscall_random_get((uintptr_t)output,64,0,0,0,0)==K_EACCES);
 range_valid=true;g_random_ready=false;assert(syscall_random_get((uintptr_t)output,64,0,0,0,0)==K_ENOSYS);
 g_random_ready=true;g_random_strong=false;assert(syscall_random_get((uintptr_t)output,64,0,0,0,0)==K_ENOSYS);
 assert(!copy_calls && !atomic_load(&pins) && !preempt_depth);
 printf("%u length/error/carry cases, reentrant copy and 1024 blocks across eight threads: unchanged stream, unique reservations and unlocked computation/copy passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-random-lock-') as directory:
    path = Path(directory)
    def run(actual):
        (path / 'test.c').write_text(fixture+rounds+actual+checks)
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True, text=True,timeout=15)
    result=run(body)
    assert result.returncode==0,result.stdout+result.stderr
    print(result.stdout.strip())
    negative=body.replace('    random_unlock();\n    for (uint32_t index',
                          '    for (uint32_t index').replace(
                          '    for (uint32_t index = 0U; index < 16U; ++index) {\n        uint32_t value',
                          '    random_unlock();\n    for (uint32_t index = 0U; index < 16U; ++index) {\n        uint32_t value')
    result=run(negative)
    assert result.returncode!=0 and 'preempt_depth' in result.stderr,result.stderr
    print('Negative control rejected: global lock covers block computation')
    negative=body.replace('        kstatus_t status = copy_to_user(',
                          '        random_lock();\n        kstatus_t status = copy_to_user(')
    result=run(negative)
    assert result.returncode!=0 and 'preempt_depth' in result.stderr,result.stderr
    print('Negative control rejected: faultable copy holds the stream lock')
    negative=body.replace('working[index] + state[index]', 'working[index] + g_random_state[index]')
    result=run(negative)
    assert result.returncode!=0 and 'memcmp' in result.stderr,result.stderr
    print('Negative control rejected: rounds use the mutable global state after reservation')
    negative=body.replace('    sched_preempt_disable();','')
    result=run(negative)
    assert result.returncode!=0 and 'preempt_depth==1' in result.stderr,result.stderr
    print('Negative control rejected: reservation owner is not pinned through unlock')
