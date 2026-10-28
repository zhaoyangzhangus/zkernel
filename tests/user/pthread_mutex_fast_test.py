"""Run actual one-word mutex code on host threads and Linux futexes."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'user/libc/pthread.c').read_text()
metadata = source[source.index('typedef struct pthread_mutex_metadata {'):
                  source.index('static volatile uint64_t g_key_bitmap;')]
functions = source[source.index('static void mutex_metadata_lock(void)'):
                   source.index('int pthread_condattr_init(')]
functions += source[source.index('static int mutex_release_for_condition('):
                    source.index('int pthread_once(')]
functions = functions.replace('static void mutex_metadata_lock(void) {',
    'static void mutex_metadata_lock(void) { atomic_fetch_add(&metadata_calls, 1);')
for name in sorted(set(re.findall(r'\bpthread_[a-z_]+', metadata+functions)), key=len, reverse=True):
    if name == 'pthread_self':
        continue
    metadata = re.sub(r'\b'+name+r'\b', name.replace('pthread_', 'fixture_'), metadata)
    functions = re.sub(r'\b'+name+r'\b', name.replace('pthread_', 'fixture_'), functions)
masks = '\n'.join(re.findall(r'^#define LIBC_MUTEXATTR_.*$', source, re.M))
program = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#define OS_WAIT_INFINITE UINT64_MAX
typedef struct { volatile uint32_t state; } fixture_mutex_t;
typedef struct { uint32_t value; } fixture_mutexattr_t;
typedef struct { volatile uint32_t sequence; uint32_t clock_id; } fixture_cond_t;
typedef pthread_t fixture_t;
static atomic_uint metadata_calls;
static int fixture_invalid(void) { return EINVAL; }
static int absolute_timeout(const struct timespec *target, clockid_t clock, uint64_t *ns) {
    struct timespec now; if (!target || clock_gettime(clock,&now)) return EINVAL;
    int64_t delta=(target->tv_sec-now.tv_sec)*1000000000LL+target->tv_nsec-now.tv_nsec;
    if (delta <= 0) return ETIMEDOUT;
    *ns=delta;return 0;
}
static int futex_wait_mode(volatile uint32_t *word, uint32_t value, uint64_t ns, bool shared) {
    struct timespec t={ns/1000000000ULL,ns%1000000000ULL};
    return syscall(SYS_futex,word,shared?FUTEX_WAIT:FUTEX_WAIT_PRIVATE,value,
        ns==OS_WAIT_INFINITE?0:&t,0,0)<0?errno:0;
}
static int futex_wait(volatile uint32_t *word, uint32_t value, uint64_t ns) {
    return futex_wait_mode(word,value,ns,false);
}
static int futex_wake_mode(volatile uint32_t *word, uint32_t count, bool shared) {
    return syscall(SYS_futex,word,shared?FUTEX_WAKE:FUTEX_WAKE_PRIVATE,count,0,0,0)<0?errno:0;
}
static int futex_wake(volatile uint32_t *word, uint32_t count) {
    return futex_wake_mode(word,count,false);
}
''' + metadata + masks + r'''
static volatile uint32_t g_mutex_metadata_lock;
static fixture_mutex_metadata_t *g_mutex_metadata_buckets[MUTEX_METADATA_BUCKET_COUNT];
''' + functions + r'''
static fixture_mutex_t normal, recursive, checked;
static fixture_cond_t condition={.clock_id=CLOCK_REALTIME};
static unsigned long counters[2];
static void *contender(void *arg) {
    bool recurse=(uintptr_t)arg;
    fixture_mutex_t *m=recurse?&recursive:&normal;
    for(unsigned i=0;i<20000;++i) {
        assert(!fixture_mutex_lock(m));
        if(recurse) assert(!fixture_mutex_lock(m));
        ++counters[recurse];
        if(recurse) assert(!fixture_mutex_unlock(m));
        assert(!fixture_mutex_unlock(m));
    }
    return 0;
}
static void *wrong_owner(void *unused) {
    (void)unused;
    assert(fixture_mutex_unlock(&checked)==EPERM);
    assert(fixture_mutex_trylock(&checked)==EBUSY);
    struct timespec limit;clock_gettime(CLOCK_REALTIME,&limit);
    limit.tv_nsec+=20000000;
    if(limit.tv_nsec>=1000000000){++limit.tv_sec;limit.tv_nsec-=1000000000;}
    assert(fixture_mutex_timedlock(&checked,&limit)==ETIMEDOUT);
    return 0;
}
static void *signal_condition(void *argument) {
    fixture_mutex_t *m=argument;
    assert(!fixture_mutex_lock(m));
    assert(!fixture_cond_signal(&condition));
    assert(!fixture_mutex_unlock(m));
    return 0;
}
static void condition_roundtrip(fixture_mutex_t *m, unsigned depth) {
    for(unsigned i=0;i<20;++i){
        for(unsigned d=0;d<depth;++d)assert(!fixture_mutex_lock(m));
        pthread_t notifier;
        assert(!pthread_create(&notifier,0,signal_condition,m));
        assert(!fixture_cond_wait(&condition,m));
        for(unsigned d=0;d<depth;++d)assert(!fixture_mutex_unlock(m));
        assert(!pthread_join(notifier,0));
    }
}
int main(void) {
    alarm(30);assert(sizeof(fixture_mutex_t)==4);
    fixture_mutex_t *shared=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(shared!=MAP_FAILED);
    fixture_mutexattr_t shared_attribute={.value=LIBC_MUTEXATTR_PSHARED_BIT};
    for(unsigned round=0;round<32;++round){
        assert(!fixture_mutex_init(shared,&shared_attribute));
        assert(!fixture_mutex_lock(shared));
        pid_t child=fork();assert(child>=0);
        if(!child){assert(!fixture_mutex_lock(shared));assert(!fixture_mutex_unlock(shared));_exit(0);}
        unsigned attempts=0;
        while((__atomic_load_n(&shared->state,__ATOMIC_ACQUIRE)&~LIBC_MUTEX_SHARED_BIT)!=LIBC_LOCK_STATE_CONTENDED && attempts++<1000){
            struct timespec pause={0,1000000};nanosleep(&pause,0);
        }
        assert(attempts<1000);
        struct timespec queued_pause={0,2000000};nanosleep(&queued_pause,0);
        assert(!fixture_mutex_unlock(shared));
        int status=0;pid_t reaped=0;
        for(unsigned i=0;i<1000 && !reaped;++i){
            reaped=waitpid(child,&status,WNOHANG);
            if(!reaped){struct timespec pause={0,1000000};nanosleep(&pause,0);}
        }
        if(!reaped){kill(child,SIGKILL);waitpid(child,&status,0);}
        assert(reaped==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
        assert(!fixture_mutex_destroy(shared));
    }
    assert(!munmap(shared,4096));
    fixture_mutex_t fresh={.state=0xdeadbeef};
    assert(!fixture_mutex_init(&fresh,0));assert(fresh.state==0);
    // Even a stalled side-table owner cannot stop ordinary mutex operations.
    g_mutex_metadata_lock=1;unsigned before=atomic_load(&metadata_calls);
    assert(!fixture_mutex_lock(&fresh));assert(fresh.state==1);
    assert(fixture_mutex_trylock(&fresh)==EBUSY);
    assert(!fixture_mutex_unlock(&fresh));assert(fresh.state==0);
    assert(!fixture_mutex_lock(&fresh));uint32_t depth=0;
    assert(!mutex_release_for_condition(&fresh,&depth) && depth==1);
    assert(!mutex_restore_after_condition(&fresh,depth));
    assert(!fixture_mutex_unlock(&fresh));
    assert(atomic_load(&metadata_calls)==before);g_mutex_metadata_lock=0;
    assert(!fixture_mutex_destroy(&fresh));
    assert(!fixture_mutex_lock(&normal));assert(fixture_mutex_destroy(&normal)==EBUSY);
    assert(!fixture_mutex_unlock(&normal));assert(fixture_mutex_unlock(&normal)==EPERM);
    fixture_mutexattr_t attr;assert(!fixture_mutexattr_init(&attr));
    assert(!fixture_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE));
    assert(!fixture_mutex_init(&recursive,&attr));
    assert(!fixture_mutex_lock(&recursive));assert(!fixture_mutex_trylock(&recursive));
    assert(fixture_mutex_destroy(&recursive)==EBUSY);
    assert(!fixture_mutex_unlock(&recursive));assert(!fixture_mutex_unlock(&recursive));
    assert(recursive.state==LIBC_MUTEX_METADATA_BIT);
    assert(!fixture_mutexattr_settype(&attr,PTHREAD_MUTEX_ERRORCHECK));
    assert(!fixture_mutex_init(&checked,&attr));assert(!fixture_mutex_lock(&checked));
    assert(fixture_mutex_lock(&checked)==EDEADLK);
    assert(fixture_mutex_trylock(&checked)==EBUSY);
    pthread_t other;assert(!pthread_create(&other,0,wrong_owner,0));assert(!pthread_join(other,0));
    assert(!fixture_mutex_unlock(&checked));assert(!fixture_mutex_destroy(&checked));
    pthread_t workers[8];
    before=atomic_load(&metadata_calls);
    for(int i=0;i<8;++i)assert(!pthread_create(&workers[i],0,contender,0));
    for(int i=0;i<8;++i)assert(!pthread_join(workers[i],0));
    assert(counters[0]==160000);assert(atomic_load(&metadata_calls)==before);
    for(int i=0;i<8;++i)assert(!pthread_create(&workers[i],0,contender,(void*)1));
    for(int i=0;i<8;++i)assert(!pthread_join(workers[i],0));
    assert(counters[1]==160000);assert(recursive.state==LIBC_MUTEX_METADATA_BIT);
    condition_roundtrip(&normal,1);
    condition_roundtrip(&recursive,2);
    // Complete release/restoration must preserve the marker and depth even
    // when the condition times out instead of receiving a signal.
    assert(!fixture_mutex_lock(&recursive));assert(!fixture_mutex_lock(&recursive));
    assert(condition_wait(&condition,&recursive,1000000)==ETIMEDOUT);
    assert(!fixture_mutex_unlock(&recursive));assert(!fixture_mutex_unlock(&recursive));
    assert(recursive.state==LIBC_MUTEX_METADATA_BIT);
    assert(!fixture_mutex_destroy(&recursive));assert(!fixture_mutex_init(&recursive,0));
    before=atomic_load(&metadata_calls);assert(!fixture_mutex_lock(&recursive));
    assert(!fixture_mutex_unlock(&recursive));assert(atomic_load(&metadata_calls)==before);
    assert(!fixture_mutexattr_setpshared(&attr,PTHREAD_PROCESS_SHARED));
    assert(fixture_mutex_init(&fresh,&attr)==ENOTSUP);
    assert(!fixture_mutex_destroy(&normal));assert(!fixture_mutex_destroy(&recursive));
}
'''
with tempfile.TemporaryDirectory(prefix='system-mutex-fast-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=35)
print('Mutex: 32 contended fork/shared-memory handoffs; private fast path, 320000 threaded updates, recursive/errorcheck, condition restore, timeout, wake chain and 4-byte ABI PASS (ASan/UBSan)')
