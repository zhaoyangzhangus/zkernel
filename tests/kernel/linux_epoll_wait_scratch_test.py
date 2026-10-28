"""Exercise the production epoll wait loop with deterministic wake/growth races."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
start = source.index('static int64_t linux_epoll_wait(')
end = source.index('\nstatic bool linux_user_pointer(', start)
function = source[start:end]
diagnostics = source[source.index('enum { LINUX_EPOLL_FAILURE_SCRATCH,'):
                     start]
assert not any(call in diagnostics for call in
               ('printf(', 'kmalloc(', 'kzalloc(', 'spinlock_lock(', 'x86_read_tsc('))
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <kernel/base.h>
#define __user
#define K_OK 0
#define K_EINVAL -22
#define K_EBADF -9
#define K_ENOMEM -12
#define K_ENOSPC -28
#define K_ETIMEDOUT -110
#define LINUX_EFAULT -14
#define LINUX_FD_EPOLL 7
#define WAIT_INLINE_QUEUE_COUNT 4
#define LINUX_MAX_EPOLL_WATCHES 1024
#define LINUX_EPOLL_WAIT_SOURCES 3073
typedef int process_t;
typedef struct { unsigned lock, watch_limit; } linux_epoll_object_t;
typedef struct { unsigned events; uint64_t data; } linux_epoll_event_t;
typedef struct { int kind; void *private_data; } linux_fd_entry_t;
typedef struct { linux_epoll_object_t *epoll; uint64_t control_sequence; } linux_epoll_wait_context_t;
static linux_epoll_object_t ep;
static unsigned allocations, frees, puts, captures, waits, releases, collects;
static unsigned ready_at, grow_on_capture, grow_on_wait, fail_at, timeout_after;
static int collect_error, capture_error, wait_error, copy_error;
static uint64_t now, previous_timeout;
static size_t last_bytes, pins;
static unsigned stats_reads;
static int stats_error;
static kstatus_t page_memory_get_stats(uint64_t *total, uint64_t *free) {
    assert(!ep.lock); __atomic_fetch_add(&stats_reads,1U,__ATOMIC_RELAXED);
    if (stats_error) return stats_error;
    *total=100000; *free=25000; return 0;
}
static bool linux_user_pointer(uint64_t p, size_t n) { return p && n; }
static int linux_fd_lookup(process_t *p, int64_t fd, linux_fd_entry_t *e) {
    (void)p; (void)fd; *e=(linux_fd_entry_t){LINUX_FD_EPOLL,&ep}; return 0;
}
static void linux_fd_entry_put(linux_fd_entry_t *e) { (void)e; ++puts; }
static void spinlock_lock(unsigned *l) { assert(!*l); *l=1; }
static void spinlock_unlock(unsigned *l) { assert(*l); *l=0; }
static uint64_t x86_read_tsc(void) { return now; }
static uint64_t x86_timeout_ns_to_tsc(uint64_t n) { return n; }
static uint64_t x86_tsc_to_ns(uint64_t n) { return n; }
static void *kmalloc(size_t n, unsigned flags) {
    assert(!ep.lock && !flags); ++allocations; last_bytes=n;
    if (fail_at==allocations) return NULL;
    /* Production may allocate replacement before releasing the old block. */
    return malloc(n);
}
static void kfree(void *p) { assert(p && !ep.lock); ++frees; free(p); }
static int64_t linux_epoll_collect(linux_epoll_object_t *e,
                                   linux_epoll_event_t *events, unsigned cap) {
    assert(e==&ep && cap); ++collects;
    if (collect_error) return collect_error;
    if (ready_at && collects>=ready_at) { events[0].data=42; return 1; }
    return 0;
}
static int copy_to_user(void *d, const void *s, size_t n) {
    if (copy_error) return copy_error;
    memcpy(d,s,n); return 0;
}
static int linux_epoll_capture_wait_set(linux_epoll_object_t *e, void **out,
                                        size_t cap, size_t *count, uint64_t *seq) {
    assert(!pins && e==&ep); ++captures;
    if (grow_on_capture) { ep.watch_limit=grow_on_capture; grow_on_capture=0; }
    if (capture_error) return capture_error;
    size_t needed=ep.watch_limit*3U+1U;
    /* Model capture's documented partial-pin unwind on overflow. */
    if (cap<needed) { assert(*count==0); return K_ENOSPC; }
    *count=needed; pins=needed-1; *seq=1; out[0]=e; return 0;
}
static void linux_epoll_release_wait_set(void **out, size_t count) {
    assert(out[0]==&ep && count==pins+1); pins=0; ++releases;
}
static bool linux_epoll_wait_ready(void *context) { (void)context; return false; }
static int object_wait_on_signals(void **out, size_t count,
                                  bool (*ready)(void *), void *context, uint64_t ns) {
    (void)ready; (void)context; assert(out[0]==&ep && count==pins+1);
    assert(ns<=previous_timeout); previous_timeout=ns; ++waits;
    now+=1000000;
    if (grow_on_wait) { ep.watch_limit=grow_on_wait; grow_on_wait=0; }
    if (wait_error) return wait_error;
    return timeout_after && waits>=timeout_after ? K_ETIMEDOUT : K_OK;
}
''' + diagnostics
checks = r'''
static void reset(unsigned watches) {
    assert(!pins); ep=(linux_epoll_object_t){0,watches};
    allocations=frees=puts=captures=waits=releases=collects=0;
    ready_at=grow_on_capture=grow_on_wait=fail_at=timeout_after=0;
    collect_error=capture_error=wait_error=copy_error=0;
    now=0; previous_timeout=UINT64_MAX; last_bytes=0;
    memset(g_linux_epoll_wait_failures,0,sizeof(g_linux_epoll_wait_failures));
    stats_reads=0;
    stats_error=0;
}
static int64_t run(int64_t timeout) {
    linux_epoll_event_t events[2];
    int64_t r=linux_epoll_wait(NULL,3,(uintptr_t)events,2,timeout);
    assert(puts==1 && !pins && !ep.lock);
    return r;
}
static void *failure_worker(void *unused) {
    (void)unused;
    for (unsigned i=0;i<1000;++i) linux_epoll_note_wait_failure(1,17,11);
    return NULL;
}
int main(void) {
    assert(sizeof(linux_epoll_wait_failure_t)==56);
    reset(1024); ready_at=1; assert(run(-1)==1 && !allocations);
    reset(1024); assert(run(0)==0 && !allocations && !captures);
    reset(1); ready_at=4; assert(run(-1)==1 && waits==3 && !allocations);
    reset(2); ready_at=4; assert(run(-1)==1 && waits==3);
    assert(allocations==1 && frees==1 && last_bytes==7*sizeof(void *));
    reset(1); grow_on_capture=9; ready_at=4; assert(run(20)==1);
    assert(captures==3 && waits==2 && allocations==1 && frees==1);
    reset(2); grow_on_wait=20; ready_at=4; assert(run(20)==1);
    assert(allocations==2 && frees==2 && last_bytes==61*sizeof(void *));
    reset(1024); timeout_after=1; assert(run(-1)==0);
    assert(last_bytes==3073*sizeof(void *) && frees==1);
    reset(2); fail_at=1; assert(run(-1)==K_ENOMEM && !frees && !captures);
    assert(g_linux_epoll_wait_failures[0].failures==1 && stats_reads==1);
    assert(g_linux_epoll_wait_failures[0].capacity==7);
    assert(g_linux_epoll_wait_failures[0].captured==0);
    reset(2); grow_on_wait=20; fail_at=2; assert(run(-1)==K_ENOMEM);
    assert(frees==1 && releases==1);
    reset(2); assert(run(3)==0 && waits==3 && allocations==1 && frees==1);
    reset(1); wait_error=-4; assert(run(-1)==-4 && releases==1);
    assert(!stats_reads);
    reset(1); wait_error=K_ENOMEM; assert(run(-1)==K_ENOMEM && releases==1);
    assert(g_linux_epoll_wait_failures[1].failures==1 && stats_reads==1);
    assert(g_linux_epoll_wait_failures[1].capacity==4);
    assert(g_linux_epoll_wait_failures[1].captured==4);
    reset(2); capture_error=-9; assert(run(-1)==-9 && frees==1);
    reset(2); collect_error=-9; assert(run(-1)==-9 && !allocations);
    reset(1); ready_at=1; copy_error=-1; assert(run(-1)==LINUX_EFAULT);
    reset(1025); assert(run(-1)==K_EINVAL && !allocations);
    reset(0);
    linux_epoll_note_wait_failure(2,1,1);
    assert(!stats_reads);
    pthread_t workers[8];
    for(unsigned i=0;i<8;++i) assert(!pthread_create(&workers[i],NULL,failure_worker,NULL));
    for(unsigned i=0;i<8;++i) assert(!pthread_join(workers[i],NULL));
    linux_epoll_wait_failure_t *f=&g_linux_epoll_wait_failures[1];
    assert(f->failures==8000 && f->state==2 && stats_reads==1);
    assert(f->capacity==17 && f->captured==11 && f->stats_status==0);
    assert(f->total_pages==100000 && f->free_pages==25000);
    linux_epoll_note_wait_failure(1,100,50);
    assert(f->failures==8001 && f->capacity==17 && f->captured==11);
    assert(stats_reads==1);
    reset(0); stats_error=-1;
    linux_epoll_note_wait_failure(0,7,0);
    f=&g_linux_epoll_wait_failures[0];
    assert(f->state==2 && f->stats_status==-1);
    assert(!f->total_pages && !f->free_pages);
    return 0;
}
'''
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
# Also exercise real capture/unwind rather than relying on the loop mock's
# contract: a watch can grow to three sources after the size hint was read.
capture_start = source.index('static bool linux_epoll_watch_has_wait_source(')
capture_end = source.index('static bool linux_epoll_wait_ready(', capture_start)
capture = source[capture_start:capture_end]
capture_fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define K_OK 0
#define K_EINVAL -22
#define K_ENOSPC -28
#define K_ENOENT -2
typedef int kstatus_t;
enum { LINUX_FD_NVIDIA, LINUX_FD_PIPE, LINUX_FD_SOCKET,
       LINUX_FD_UNIX_SOCKET, LINUX_FD_WINDOW, LINUX_FD_EVENTFD };
typedef struct { unsigned refs; } object_t;
typedef struct { object_t object; unsigned lock; void *read_end, *write_end; } linux_unix_socket_t;
typedef struct { bool used, disabled; int kind; void *private_data; } linux_epoll_watch_t;
typedef struct { unsigned lock, watch_limit; uint64_t control_sequence;
                 linux_epoll_watch_t watches[2]; } linux_epoll_object_t;
static void spinlock_lock(unsigned *l) { assert(!*l); *l=1; }
static void spinlock_unlock(unsigned *l) { assert(*l); *l=0; }
static bool object_try_get(void *p) {
    object_t *o=p; if (!o->refs) return false; ++o->refs; return true;
}
static void object_put(void *p) { object_t *o=p; assert(o->refs>1); --o->refs; }
static void *nv_frontend_file_wait_object(void *p) { return p; }
'''
capture_checks = r'''
int main(void) {
    object_t a={1},b={1};
    linux_unix_socket_t s={.object={1},.read_end=&a,.write_end=&b};
    linux_epoll_object_t e={.watch_limit=2,.control_sequence=17,
        .watches={{true,false,LINUX_FD_UNIX_SOCKET,&s},
                  {true,false,LINUX_FD_SOCKET,&a}}};
    void *objects[8]; uint64_t seq=0;
    for (size_t capacity=1;capacity<=8;++capacity) {
        size_t count=0;
        int status=linux_epoll_capture_wait_set(&e,objects,capacity,&count,&seq);
        assert(seq==17 && !e.lock && !s.lock);
        if (capacity<4) assert(status==K_ENOSPC && count==0);
        else {
            assert(status==K_OK && count==4);
            assert(s.object.refs==2 && a.refs==2 && b.refs==2);
            linux_epoll_release_wait_set(objects,count);
        }
        assert(s.object.refs==1 && a.refs==1 && b.refs==1);
    }
    b.refs=0;
    size_t count=0;
    assert(linux_epoll_capture_wait_set(&e,objects,8,&count,&seq)==K_ENOENT);
    assert(!count && s.object.refs==1 && a.refs==1 && !e.lock && !s.lock);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    for negative in (False, True):
        body = function
        if negative:
            old = '(size_t)epoll->watch_limit * 3U + 1U'
            assert old in body
            body = body.replace(old, 'LINUX_EPOLL_WAIT_SOURCES')
        (path / 'test.c').write_text(fixture + body + checks)
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(root / 'include'),
                        '-fsanitize=address,undefined', '-pthread', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True)
        assert (result.returncode != 0) == negative, result.stderr.decode()
    (path / 'capture.c').write_text(capture_fixture + capture + capture_checks)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'capture.c'), '-o', str(path / 'capture')], check=True)
    subprocess.run([str(path / 'capture')], check=True)
print('Epoll wait scratch: production loop, capture/unwind, inline/growth, deadline, errors and 8000 racing failure snapshots PASS; negative control rejected')
