"""Actual font cache export: copy before replacement, fewer locks, exact cleanup."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/ascii_font.c').read_text()
body = source[source.index('static void font_user_cache_lock('):]
body = body.replace('memcpy(destination, g_ascii_font_user_scaled, cache_bytes);',
                    'test_copy(destination, g_ascii_font_user_scaled, cache_bytes);')
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#define ASCII_FONT_FIRST 0x20U
#define ASCII_FONT_LAST 0x7eU
#define ASCII_FONT_MAX_DIMENSION 64U
#define ASCII_FONT_MAX_CACHE_BYTES (512U*1024U)
typedef pthread_mutex_t spinlock_t;
static spinlock_t g_ascii_font_user_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { void *file; } g_ascii_font_parser={(void*)1};
static uint8_t *g_ascii_font_user_scaled;
static uint32_t g_ascii_font_user_width,g_ascii_font_user_height;
static atomic_uint allocations,frees;
static _Thread_local unsigned depth,held,locks,builds,copies,hook_at_enable,hook_calls;
static _Thread_local bool fail_build,build_same;
static _Thread_local uint8_t nested[64*64*95];
static _Thread_local uint8_t hook_pixels[64*64*95];
bool ascii_font_copy_cache(uint32_t,uint32_t,uint8_t*,size_t);
static uint8_t pixel(unsigned w,unsigned h,size_t i) { return (uint8_t)(w*17+h*31+i*13); }
static void verify(unsigned w,unsigned h,const uint8_t *out) {
 for(size_t i=0;i<(size_t)w*h*95;++i)assert(out[i]==pixel(w,h,i));
}
static void sched_preempt_disable(void) { assert(!depth && !held);++depth; }
static void sched_preempt_enable(void) {
 assert(depth==1 && !held);--depth;
 if(hook_at_enable && --hook_at_enable==0) {
  ++hook_calls;
  /* 此调用者释放锁后，另一个尺寸的请求会立即完成。 */
  assert(ascii_font_copy_cache(63,62,hook_pixels,sizeof(hook_pixels)));verify(63,62,hook_pixels);
 }
}
static void spinlock_lock(spinlock_t *l) {
 assert(depth==1 && !held);assert(!pthread_mutex_lock(l));held=1;++locks;
}
static void spinlock_unlock(spinlock_t *l) {
 assert(depth==1 && held);held=0;assert(!pthread_mutex_unlock(l));
}
static void kfree(void *p) { assert(!held && !depth);if(p){++frees;free(p);} }
static bool font_build_cache(uint32_t w,uint32_t h,uint8_t **out,size_t *size) {
 assert(!held && !depth);++builds;if(fail_build)return false;
 *size=(size_t)w*h*95;*out=malloc(*size);assert(*out);++allocations;
 for(size_t i=0;i<*size;++i)(*out)[i]=pixel(w,h,i);
 if(build_same) {
  build_same=false;
  assert(ascii_font_copy_cache(w,h,hook_pixels,sizeof(hook_pixels)));verify(w,h,hook_pixels);
 }
 return true;
}
static void test_copy(void *destination,const void *src,size_t bytes) {
 assert(depth==1 && held && src==g_ascii_font_user_scaled);++copies;
 assert(bytes==(size_t)g_ascii_font_user_width*g_ascii_font_user_height*95);
 memcpy(destination,src,bytes);
}
'''
checks = r'''
static void drop(void) {
 kfree(g_ascii_font_user_scaled);g_ascii_font_user_scaled=NULL;
 g_ascii_font_user_width=g_ascii_font_user_height=0;
 assert(atomic_load(&allocations)==atomic_load(&frees));
}
static void *worker(void *opaque) {
 unsigned id=(unsigned)(uintptr_t)opaque;
 for(unsigned i=0;i<200;++i) {
  unsigned w=1+(i+id)%32,h=1+(i*3+id)%24;size_t n=(size_t)w*h*95;
  uint8_t *out=malloc(n+2);assert(out);out[0]=out[n+1]=0xa5;
  assert(ascii_font_copy_cache(w,h,out+1,n));verify(w,h,out+1);
  assert(out[0]==0xa5 && out[n+1]==0xa5 && !depth && !held);free(out);
 }
 return NULL;
}
int main(void) {
 unsigned sizes[]={1,2,7,8,16,31,32,64},cases=0;
 for(unsigned wi=0;wi<8;++wi)for(unsigned hi=0;hi<8;++hi) {
  drop();unsigned w=sizes[wi],h=sizes[hi];size_t n=(size_t)w*h*95;
  uint8_t *out=malloc(n+2);assert(out);memset(out,0xa5,n+2);
  locks=builds=copies=0;
  assert(ascii_font_copy_cache(w,h,out+1,n));verify(w,h,out+1);
  assert(locks==2 && builds==1 && copies==1 && !depth && !held);
  locks=builds=copies=0;
  assert(ascii_font_copy_cache(w,h,out+1,n));verify(w,h,out+1);
  assert(locks==1 && !builds && copies==1 && !depth && !held);
  assert(out[0]==0xa5 && out[n+1]==0xa5);
  uint8_t *old=g_ascii_font_user_scaled;
  fail_build=true;locks=builds=copies=0;
  unsigned other=w==64?63:w+1;
  assert(!ascii_font_copy_cache(other,h,nested,sizeof(nested)));
  assert(locks==1 && builds==1 && !copies && g_ascii_font_user_scaled==old);
  fail_build=false;free(out);++cases;
 }
 drop();locks=builds=copies=0;build_same=true;
 assert(ascii_font_copy_cache(8,16,nested,sizeof(nested)));verify(8,16,nested);
 assert(locks==4 && builds==2 && copies==2);
 assert(atomic_load(&allocations)==atomic_load(&frees)+1);drop();
 for(unsigned position=1;position<=2;++position) {
  hook_calls=0;hook_at_enable=position;
  assert(ascii_font_copy_cache(8,16,nested,sizeof(nested)));verify(8,16,nested);
  assert(hook_calls==1 && !hook_at_enable && !depth && !held);drop();
 }
 assert(ascii_font_copy_cache(8,16,nested,sizeof(nested)));
 hook_calls=0;hook_at_enable=1;
 uint8_t warm[8*16*95];assert(ascii_font_copy_cache(8,16,warm,sizeof(warm)));
 verify(8,16,warm);assert(hook_calls==1);drop();
 locks=builds=copies=0;
 assert(!ascii_font_copy_cache(0,8,nested,sizeof(nested)));
 assert(!ascii_font_copy_cache(8,0,nested,sizeof(nested)));
 assert(!ascii_font_copy_cache(65,8,nested,sizeof(nested)));
 assert(!ascii_font_copy_cache(8,65,nested,sizeof(nested)));
 assert(!ascii_font_copy_cache(8,16,NULL,sizeof(nested)));
 assert(!ascii_font_copy_cache(8,16,nested,8*16*95-1));
 g_ascii_font_parser.file=NULL;assert(!ascii_font_copy_cache(8,16,nested,sizeof(nested)));
 g_ascii_font_parser.file=(void*)1;assert(!locks && !builds && !copies);
 pthread_t threads[8];
 for(unsigned i=0;i<8;++i)assert(!pthread_create(threads+i,NULL,worker,(void*)(uintptr_t)i));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
 drop();
 printf("%u cold/warm/OOM sizes, same-size builders, three replacement boundaries and 1600 eight-thread exports: exact pixels, 1/2 locks, guards and unlocked cleanup passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-font-publication-') as directory:
    path = Path(directory)
    def run(actual, assertions=checks):
        (path / 'test.c').write_text(fixture+actual+assertions)
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True, text=True,timeout=20)
    result=run(body)
    assert result.returncode==0,result.stdout+result.stderr
    print(result.stdout.strip())
    negative=body.replace('    test_copy(destination, g_ascii_font_user_scaled, cache_bytes);\n    font_user_cache_unlock();',
                          '    font_user_cache_unlock();\n    font_user_cache_lock();\n    test_copy(destination, g_ascii_font_user_scaled, cache_bytes);\n    font_user_cache_unlock();')
    result=run(negative)
    assert result.returncode!=0 and 'locks==2' in result.stderr,result.stderr
    print('Negative control rejected: redundant publication/copy lock cycle')
    # 独立复现旧的发布后检查，不依赖锁计数断言：发布完成后，其他字体也可能立即胜出。
    deferred='''    font_user_cache_unlock();
    font_user_cache_lock();
    if (g_ascii_font_user_width != width || g_ascii_font_user_height != height) {
        font_user_cache_unlock();
        if (old != 0) kfree(old);
        if (candidate != 0) kfree(candidate);
        return false;
    }
    test_copy(destination, g_ascii_font_user_scaled, cache_bytes);
    font_user_cache_unlock();'''
    negative=body.replace('    test_copy(destination, g_ascii_font_user_scaled, cache_bytes);\n    font_user_cache_unlock();', deferred)
    relaxed=checks.replace('locks==2 &&', 'true &&').replace('locks==1 &&', 'true &&').replace('locks==4 &&', 'true &&')
    result=run(negative, relaxed)
    assert result.returncode!=0 and 'ascii_font_copy_cache(8,16' in result.stderr,result.stderr
    print('Negative control rejected: a different face replaces the cache between publication and copying')
    negative=body.replace('    test_copy(destination, g_ascii_font_user_scaled, cache_bytes);\n    font_user_cache_unlock();',
                          '    font_user_cache_unlock();\n    test_copy(destination, g_ascii_font_user_scaled, cache_bytes);')
    result=run(negative)
    assert result.returncode!=0 and 'depth==1 && held' in result.stderr,result.stderr
    print('Negative control rejected: copy can race cache replacement/free')
