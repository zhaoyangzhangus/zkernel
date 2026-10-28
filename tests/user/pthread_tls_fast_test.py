"""Exercise production TLS key APIs with real host threads, without interposition."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--repeat', type=int, default=1)
args = parser.parse_args()
if args.repeat < 1:
    parser.error('--repeat must be positive')
root = Path(__file__).resolve().parents[2]
source = (root / 'user/libc/pthread.c').read_text()
locks = source[source.index('static void key_lock(void)'):
               source.index('static void run_key_destructors(')]
keys = source[source.index('int pthread_key_create('):
              source.index('int pthread_rwlockattr_init(')]
for old, new in [('pthread_key_t', 'fixture_key_t'), ('pthread_t', 'fixture_thread_t'),
                 ('pthread_key_create', 'fixture_key_create'),
                 ('pthread_key_delete', 'fixture_key_delete'),
                 ('pthread_getspecific', 'fixture_getspecific'),
                 ('pthread_setspecific', 'fixture_setspecific'),
                 ('pthread_self()', 'fixture_self()')]:
    keys = keys.replace(old, new)
    locks = locks.replace(old, new)
assert 'key_lock();' not in keys[keys.index('void *fixture_getspecific('):]
locks = locks.replace('static void key_lock(void) {',
                      'static void key_lock(void) { atomic_fetch_add(&lock_calls,1);')
program = r'''
#include <pthread.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#undef PTHREAD_KEYS_MAX
#define PTHREAD_KEYS_MAX 64U
typedef unsigned fixture_key_t;
struct fixture_thread { void *specific[64]; uint32_t specific_generation[64]; };
typedef struct fixture_thread *fixture_thread_t;
static struct fixture_thread g_main_thread;
static _Thread_local struct fixture_thread local_thread;
static _Thread_local unsigned self_mode=1;
static fixture_thread_t fixture_self(void) {
    return self_mode==0 ? 0 : self_mode==2 ? &g_main_thread : &local_thread;
}
static volatile uint64_t g_key_bitmap;
static void (*g_key_destructors[64])(void *);
static uint32_t g_key_generation[64];
static volatile uint32_t g_key_lock;
static void *g_main_specific[64];
static uint32_t g_main_specific_generation[64];
static atomic_uint lock_calls;
static int pthread_invalid(void) { return EINVAL; }
''' + locks + keys + r'''
static fixture_key_t stable, changing;
static atomic_bool stop;
static void *worker(void *argument) {
    uintptr_t id=(uintptr_t)argument;
    assert(fixture_getspecific(stable)==0);
    for(unsigned i=0;i<100000;++i) {
        void *value=(void *)(id*1000000+i+1);
        assert(fixture_setspecific(stable,value)==0);
        assert(fixture_getspecific(stable)==value);
        int error=fixture_setspecific(changing,value);
        assert(error==0 || error==EINVAL);
        void *raced=fixture_getspecific(changing);
        if(raced && raced!=value){
            fprintf(stderr,"TLS mismatch worker=%lu iteration=%u set_error=%d expected=%p got=%p slot_generation=%u key_generation=%u bitmap=%llx\n",
                (unsigned long)id,i,error,value,raced,
                __atomic_load_n(&local_thread.specific_generation[changing],__ATOMIC_ACQUIRE),
                __atomic_load_n(&g_key_generation[changing],__ATOMIC_ACQUIRE),
                (unsigned long long)__atomic_load_n(&g_key_bitmap,__ATOMIC_ACQUIRE));
            assert(!raced || raced==value);
        }
    }
    return 0;
}
static void *mutator(void *unused) {
    (void)unused;
    for(unsigned i=0;i<10000;++i) {
        assert(fixture_key_delete(changing)==0);
        fixture_key_t reused;
        assert(fixture_key_create(&reused,0)==0 && reused==changing);
    }
    atomic_store(&stop,true); return 0;
}
int main(void) {
    assert(fixture_key_create(&stable,0)==0 && stable==0);
    assert(fixture_setspecific(stable,(void *)123)==0);
    assert(fixture_getspecific(stable)==(void *)123);
    assert(fixture_setspecific(stable,0)==0 && !fixture_getspecific(stable));
    assert(fixture_getspecific(64)==0 && fixture_setspecific(UINT32_MAX,(void *)1)==EINVAL);
    assert(fixture_setspecific(stable,(void *)456)==0);
    assert(fixture_key_delete(stable)==0 && !fixture_getspecific(stable));
    assert(fixture_key_create(&stable,0)==0 && stable==0);
    assert(!fixture_getspecific(stable)); /* Reuse never exposes an old slot. */
    self_mode=0;
    assert(fixture_setspecific(stable,(void *)789)==0 && fixture_getspecific(stable)==(void *)789);
    assert(fixture_key_delete(stable)==0 && !g_main_specific[stable]);
    assert(fixture_key_create(&stable,0)==0);
    self_mode=2;
    assert(fixture_setspecific(stable,(void *)987)==0);
    assert(fixture_key_delete(stable)==0 && !g_main_thread.specific[stable]);
    assert(fixture_key_create(&stable,0)==0 && !fixture_getspecific(stable));
    self_mode=1;
    /* A metadata owner delayed on another CPU must not stop TLS access. */
    unsigned before=atomic_load(&lock_calls); g_key_lock=1;
    assert(fixture_setspecific(stable,(void *)654)==0);
    assert(fixture_getspecific(stable)==(void *)654);
    assert(atomic_load(&lock_calls)==before); g_key_lock=0;
    assert(fixture_key_create(&changing,0)==0 && changing==1);
    pthread_t readers[8],writer;
    for(uintptr_t i=0;i<8;++i) assert(!pthread_create(&readers[i],0,worker,(void *)(i+1)));
    assert(!pthread_create(&writer,0,mutator,0));
    for(unsigned i=0;i<8;++i) assert(!pthread_join(readers[i],0));
    assert(!pthread_join(writer,0)); assert(atomic_load(&stop));
    assert(fixture_getspecific(stable)==(void *)654);
    puts("TLS fast path: 8 threads, 800000 slot updates, 10000 delete/reuse races, generations, fallback and no hot-path key lock passed");
}
'''
with tempfile.TemporaryDirectory(prefix='system-tls-fast-') as directory:
    binary = str(Path(directory) / 'test')
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                    '-pthread', '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    for _ in range(args.repeat):
        subprocess.run([binary], check=True, timeout=15)
