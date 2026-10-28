"""Force a TLS reader across deletion and paused key publication."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'user/libc/pthread.c').read_text()
locks = source[source.index('static void key_lock(void)'):
               source.index('static void run_key_destructors(')]
keys = source[source.index('int pthread_key_create('):
              source.index('int pthread_rwlockattr_init(')]
prefix = r'''
#include <pthread.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#undef PTHREAD_KEYS_MAX
#define PTHREAD_KEYS_MAX 64U
#define PTHREAD_DESTRUCTOR_ITERATIONS 4U
typedef unsigned fixture_key_t;
struct fixture_thread {void *specific[64];uint32_t specific_generation[64];};
typedef struct fixture_thread *fixture_thread_t;
static _Thread_local struct fixture_thread local_thread;
static struct fixture_thread g_main_thread;
static fixture_thread_t fixture_self(void){return &local_thread;}
static volatile uint64_t g_key_bitmap;
static uint32_t g_key_generation[64];
static void (*g_key_destructors[64])(void *);
static volatile uint32_t g_key_lock;
static void *g_main_specific[64];static uint32_t g_main_specific_generation[64];
static int pthread_invalid(void){return EINVAL;}
static atomic_uint phase;
static _Thread_local bool intercept_snapshot,intercept_publication;
static void snapshot_hook(void){
 if(!intercept_snapshot)return;
 intercept_snapshot=false;atomic_store(&phase,1);
 while(atomic_load(&phase)!=2)sched_yield();
}
static void publication_hook(void){
 if(!intercept_publication)return;
 intercept_publication=false;atomic_store(&phase,2);
 while(atomic_load(&phase)!=3)sched_yield();
}
'''
suffix = r'''
static fixture_key_t key;
static unsigned destructor_calls;
static void rearming_destructor(void *value){
 assert(value==(destructor_calls? (void *)0x555:(void *)0x444));
 if(++destructor_calls==1)assert(!fixture_setspecific(key,(void *)0x555));
}
static void *reader(void *unused){
 (void)unused;assert(!fixture_setspecific(key,(void *)0x111));
 intercept_snapshot=true;
 assert(fixture_setspecific(key,(void *)0x222)==EINVAL);
 atomic_store(&phase,3);
 while(atomic_load(&phase)!=4)sched_yield();
 assert(!fixture_getspecific(key));
 assert(!fixture_setspecific(key,(void *)0x333));
 assert(fixture_getspecific(key)==(void *)0x333);
 return 0;
}
static void *writer(void *unused){
 (void)unused;while(atomic_load(&phase)!=1)sched_yield();
 assert(!fixture_key_delete(key));
 intercept_publication=true;fixture_key_t reused;
 assert(!fixture_key_create(&reused,0) && reused==key);
 atomic_store(&phase,4);return 0;
}
int main(void){
 alarm(10);
 for(unsigned i=0;i<128;++i){
  atomic_store(&phase,0);assert(!fixture_key_create(&key,0));
  pthread_t r,w;assert(!pthread_create(&r,0,reader,0));assert(!pthread_create(&w,0,writer,0));
  assert(!pthread_join(r,0));assert(!pthread_join(w,0));assert(!fixture_key_delete(key));
 }
 // The invalid even token at wrap must not resurrect an old TLS slot.
 g_key_generation[0]=UINT32_MAX-1U;assert(!fixture_key_create(&key,0));
 assert(g_key_generation[0]==UINT32_MAX);
 assert(!fixture_setspecific(key,(void *)1));assert(!fixture_key_delete(key));
 assert(g_key_generation[0]==0 && !fixture_getspecific(key));
 assert(!fixture_key_create(&key,0) && g_key_generation[0]==1);
 assert(!fixture_getspecific(key));
 assert(!fixture_key_delete(key));
 assert(!fixture_key_create(&key,rearming_destructor));
 assert(!fixture_setspecific(key,(void *)0x444));
 run_key_destructors(fixture_self());
 assert(destructor_calls==2 && !fixture_getspecific(key));
 assert(!fixture_setspecific(key,(void *)0x999));
 assert(!fixture_key_delete(key));
 run_key_destructors(fixture_self());assert(destructor_calls==2);
 assert(!fixture_key_create(&key,rearming_destructor));
 run_key_destructors(fixture_self());assert(destructor_calls==2);
 puts("TLS incarnation: 128 forced delete/publication interleavings, failed write isolation, deleted-token wrap, destructor rearm and deleted/reused-key exclusion PASS");
}
'''
old_snapshot = '''static bool key_generation_snapshot(pthread_key_t key, uint32_t *generation) {
    if (key >= PTHREAD_KEYS_MAX ||
        ((__atomic_load_n(&g_key_bitmap, __ATOMIC_ACQUIRE) >> key) & 1U) == 0U)
        return false;
    *generation = __atomic_load_n(&g_key_generation[key], __ATOMIC_ACQUIRE);
    return true;
}'''
with tempfile.TemporaryDirectory(prefix='tls-incarnation-') as directory:
    path = Path(directory)
    for negative in (False, True):
        candidate = keys
        if negative:
            start = candidate.index('static bool key_generation_snapshot(')
            end = candidate.index('\n}',start)+2
            candidate = candidate[:start]+old_snapshot+candidate[end:]
            candidate = candidate.replace(
                '    uint32_t generation = __atomic_load_n(&g_key_generation[key], __ATOMIC_RELAXED);\n'
                '    __atomic_store_n(&g_key_generation[key], generation + 1U, __ATOMIC_RELEASE);\n','')
            candidate = candidate.replace(
                '            g_key_destructors[index] = destructor;\n'
                '            __atomic_fetch_or(&g_key_bitmap, bit, __ATOMIC_RELEASE);\n'
                '            __atomic_store_n(&g_key_generation[index], generation, __ATOMIC_RELEASE);',
                '            __atomic_store_n(&g_key_generation[index], generation, __ATOMIC_RELEASE);\n'
                '            g_key_destructors[index] = destructor;\n'
                '            publication_hook();\n'
                '            __atomic_fetch_or(&g_key_bitmap, bit, __ATOMIC_RELEASE);')
        else:
            candidate = candidate.replace(
                '            __atomic_store_n(&g_key_generation[index], generation, __ATOMIC_RELEASE);',
                '            publication_hook();\n'
                '            __atomic_store_n(&g_key_generation[index], generation, __ATOMIC_RELEASE);')
        candidate = candidate.replace('    *generation = __atomic_load_n(',
                                      '    snapshot_hook();\n    *generation = __atomic_load_n(')
        destructors = source[source.index('static void run_key_destructors('):
                             source.index('static size_t page_round(')]
        functions = locks+candidate+destructors
        for old,new in [('pthread_key_t','fixture_key_t'),('pthread_t','fixture_thread_t'),
                        ('pthread_key_create','fixture_key_create'),('pthread_key_delete','fixture_key_delete'),
                        ('pthread_getspecific','fixture_getspecific'),('pthread_setspecific','fixture_setspecific'),
                        ('pthread_self()','fixture_self()')]:
            functions = functions.replace(old,new)
        (path/'test.c').write_text(prefix+functions+suffix)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        result = subprocess.run([str(path/'test')],capture_output=True,text=True,timeout=15)
        if negative:
            assert result.returncode != 0, 'Old split publication must fail'
            assert 'fixture_getspecific(key)' in result.stderr, result.stderr
            print('Negative control rejected: old bitmap/generation publication exposes failed-write value')
        else:
            assert result.returncode == 0,result.stderr
            print(result.stdout,end='')
