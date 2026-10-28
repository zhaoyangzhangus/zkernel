"""Exercise actual deferred callback attribution, including concurrent consumers."""
from pathlib import Path
import resource
import subprocess
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/irq/deferred.c').read_text()
state = source[source.index('#define DEFERRED_SLOW_WORK_SLOTS'):source.index('\nstatic struct {')]
start = source.index('static void deferred_record_work(')
helper = source[start:source.index('\nuint32_t deferred_run(', start)]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>
typedef void (*deferred_work_fn_t)(void *);
''' + state + r'''
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local uint64_t now;
static _Thread_local unsigned depth,locks;
static uint64_t x86_read_tsc(void) {return now;}
static uint64_t x86_tsc_to_ns(uint64_t cycles) {return cycles;}
static uint64_t deferred_lock(void) {
    assert(!depth);assert(!pthread_mutex_lock(&mutex));++depth;++locks;return 7;
}
static void deferred_unlock(uint64_t flags) {
    assert(flags==7&&depth==1);--depth;assert(!pthread_mutex_unlock(&mutex));
}
''' + helper + r'''
static void a(void *arg) {(void)arg;}
static void b(void *arg) {(void)arg;}
static void c(void *arg) {(void)arg;}
static void d(void *arg) {(void)arg;}
static void e(void *arg) {(void)arg;}
static void f(void *arg) {(void)arg;}
static void g(void *arg) {(void)arg;}
static void h(void *arg) {(void)arg;}
static void extra(void *arg) {(void)arg;}
static void *consumer(void *arg) {
    (void)arg;now=2000001;
    for(unsigned i=0;i<10000;++i)deferred_record_work(a,1);
    assert(!depth);return 0;
}
int main(void) {
    now=1000000;deferred_record_work(a,1);
    assert(!locks&&!g_deferred_slow_work[0].function);
    now=1000001;deferred_record_work(a,1);
    assert(locks==1&&g_deferred_slow_work[0].function==a);
    assert(g_deferred_slow_work[0].count==1);
    assert(g_deferred_slow_work[0].total_ns==1000000);
    assert(g_deferred_slow_work[0].max_ns==1000000);
    now=5000002;deferred_record_work(a,2);
    assert(g_deferred_slow_work[0].count==2);
    assert(g_deferred_slow_work[0].total_ns==6000000);
    assert(g_deferred_slow_work[0].max_ns==5000000);
    assert(g_deferred_slow_work[0].last_start_tsc==2);
    assert(g_deferred_slow_work[0].last_end_tsc==now);
    deferred_work_fn_t rest[]={b,c,d,e,f,g,h};
    now=2000001;
    for(unsigned i=0;i<7;++i)deferred_record_work(rest[i],1);
    assert(g_deferred_slow_work[7].function==h);
    deferred_record_work(extra,1);assert(g_deferred_slow_work_untracked==1);
    deferred_record_work(h,1);assert(g_deferred_slow_work[7].count==2);
    assert(g_deferred_slow_work[0].function==a);
    now=999999;deferred_record_work(h,UINT64_MAX);
    assert(g_deferred_slow_work[7].count==3);
    assert(g_deferred_slow_work[7].total_ns==5000000);
    memset(g_deferred_slow_work,0,sizeof(g_deferred_slow_work));
    pthread_t threads[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,consumer,0));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    assert(g_deferred_slow_work[0].count==40000);
    assert(g_deferred_slow_work[0].total_ns==80000000000ULL);
    assert(g_deferred_slow_work[0].max_ns==2000000);
    assert(!g_deferred_slow_work[1].function);
}
'''
with tempfile.TemporaryDirectory(prefix='deferred-slow-work-') as directory:
    path = Path(directory)
    for name, flags in [('optimized', ['-O2']),
                        ('sanitized', ['-fsanitize=address,undefined', '-fno-pie', '-no-pie'])]:
        unit = path / (name + '.c')
        unit.write_text(program)
        output = path / name
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        *flags, str(unit), '-o', str(output)], check=True)
        subprocess.run([str(output)], check=True)
print('Deferred slow work: threshold, fast path, identity, totals, overflow, wrap and concurrent attribution passed')
