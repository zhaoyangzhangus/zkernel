"""Production poll snapshot/scan: single-lock pins, fd reuse and error unwinding."""
from pathlib import Path
import subprocess
import tempfile
import re
import resource

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
body = source[source.index('static uint64_t linux_poll_timeout_ns('):
              source.index('static int64_t linux_read(process_t *process, int64_t descriptor,\n',
                           source.index('static uint64_t linux_poll_timeout_ns('))]
wait_type = source[source.index('typedef struct linux_poll_wait_set {'):
                   source.index('static void linux_poll_wait_set_release(')]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define __user
#define K_OK 0
#define K_EINVAL -22
#define K_ENOSPC -28
#define K_ENOMEM -12
#define K_ETIMEDOUT -110
#define LINUX_EFAULT -14
#define LINUX_FD_UNUSED 0
#define LINUX_FD_NVIDIA 99
#define LINUX_POLLIN 1
#define LINUX_POLLPRI 2
#define LINUX_POLLNVAL 32
#define LINUX_MAX_POLL_FDS 256
typedef int kstatus_t;
typedef struct { unsigned refs; bool ready; } object_t;
typedef struct { object_t *private_data; unsigned char kind; } linux_fd_entry_t;
typedef struct { bool held; } spinlock_t;
typedef struct { spinlock_t lock; linux_fd_entry_t entries[256]; } linux_fd_table_t;
typedef struct { linux_fd_table_t linux_fds; } process_t;
typedef struct { int32_t descriptor; int16_t events, revents; } linux_pollfd_t;
typedef struct { const linux_pollfd_t *fds; const linux_fd_entry_t *entries; uint64_t count; } linux_poll_wait_context_t;
POLL_WAIT_SET_TYPE
static spinlock_t g_linux_poll_waitq;
static process_t process;
static object_t objects[256], replacement;
static unsigned locks, pins, releases, waits, mode, captures, set_releases;
static uint64_t now;
static uint64_t previous_timeout;
static bool input_fail, output_fail;
static unsigned allocation_calls, allocation_fail, allocations_live;
static size_t allocation_sizes[4];
static void *kmalloc(size_t size, unsigned flags) {
    assert(flags == 0U && !process.linux_fds.lock.held);
    if (allocation_calls<4) allocation_sizes[allocation_calls]=size;
    if (++allocation_calls == allocation_fail) return NULL;
    void *p = malloc(size);
    if (p) ++allocations_live;
    return p;
}
static void kfree(void *p) {
    if (p) { assert(allocations_live); --allocations_live; free(p); }
}
static void spinlock_lock(spinlock_t *l) { assert(!l->held); l->held=true; ++locks; }
static void spinlock_unlock(spinlock_t *l) { assert(l->held); l->held=false; }
static bool linux_fd_valid(int64_t fd) { return fd >= 0 && fd < 256; }
static void linux_fd_entry_get(linux_fd_entry_t *entry) {
    assert(process.linux_fds.lock.held && entry->private_data->refs);
    ++entry->private_data->refs; ++pins;
}
static void linux_fd_entry_put(linux_fd_entry_t *entry) {
    assert(!process.linux_fds.lock.held);
    if (entry->kind) { assert(entry->private_data->refs); --entry->private_data->refs; ++releases; }
    *entry=(linux_fd_entry_t){0};
}
static bool linux_user_pointer(uint64_t p, size_t n) { return p && n; }
static int copy_from_user(void *out, const void *in, size_t n) {
    if(input_fail) return LINUX_EFAULT;
    memcpy(out,in,n); return K_OK;
}
static int copy_to_user(void *out, const void *in, size_t n) {
    if(output_fail) return LINUX_EFAULT;
    memcpy(out,in,n); return K_OK;
}
static uint64_t x86_read_tsc(void) { return now; }
static uint64_t x86_timeout_ns_to_tsc(uint64_t n) { return n; }
static uint64_t x86_tsc_to_ns(uint64_t n) { return n; }
static int16_t linux_poll_ready(int32_t fd, const linux_fd_entry_t *entry, int16_t events) {
    assert(!process.linux_fds.lock.held && fd >= 0 && entry->kind && entry->private_data->refs);
    return entry->private_data->ready ? events : 0;
}
static uint32_t nv_frontend_poll_state(void *p,bool consume) {
    (void)p;(void)consume;assert(!"unexpected NVIDIA entry in generic snapshot fixture");return 0;
}
static int linux_poll_capture_wait_set(const linux_pollfd_t *fds, const linux_fd_entry_t *entries,
                                        uint64_t count, linux_poll_wait_set_t *set) {
    (void)fds; (void)entries; assert(!process.linux_fds.lock.held); ++captures;
    assert(set->capacity>=count*3 && set->objects && set->owned);
    set->count=count ? 1 : 0; set->objects[0]=&process;
    return mode == 4 ? K_ENOSPC : K_OK;
}
static void linux_poll_wait_set_release(linux_poll_wait_set_t *set) {
    assert(!process.linux_fds.lock.held); ++set_releases; set->count=0;
}
static int perform_wait(void) {
    assert(!process.linux_fds.lock.held); ++waits;
    if(mode == 6) { now+=1000000; if(waits<3) return K_OK; }
    if(mode == 3) return -4;
    if(mode == 5) { now=1000000; return K_ETIMEDOUT; }
    if(mode == 2) {
        assert(objects[1].refs > 1); --objects[1].refs;
        replacement=(object_t){.refs=1, .ready=true};
        process.linux_fds.entries[1]=(linux_fd_entry_t){.private_data=&replacement, .kind=1};
    } else for(unsigned i=0;i<256;++i) objects[i].ready=true;
    return K_OK;
}
static int object_wait_on_signals(void **objects_arg, unsigned count, bool (*predicate)(void *),
                                 void *context, uint64_t timeout) {
    assert(objects_arg && count == 1 && predicate && context && timeout);
    assert(timeout<=previous_timeout); previous_timeout=timeout;
    return perform_wait();
}
static bool linux_poll_wait_ready(void *p) { return p != NULL; }
static bool linux_poll_never_ready(void *p) { return p != NULL; }
static void linux_poll_waitq_init(void) {}
static int wait_on_queue(spinlock_t *q, bool (*predicate)(void *), void *p, uint64_t timeout) {
    assert(q && predicate && !p && timeout); return perform_wait();
}
static void reset(unsigned chosen) {
    memset(&process,0,sizeof(process));
    for(unsigned i=0;i<256;++i) {
        objects[i]=(object_t){.refs=1,.ready=chosen==0};
        if(i%3) process.linux_fds.entries[i]=(linux_fd_entry_t){.private_data=&objects[i],.kind=1};
    }
    replacement=(object_t){0};
    mode=chosen; locks=pins=releases=waits=captures=set_releases=0; now=0;
    previous_timeout=UINT64_MAX;
    input_fail=output_fail=false;
    allocation_calls=allocation_fail=0;
}
static void balanced(void) {
    assert(!process.linux_fds.lock.held && pins == releases);
    for(unsigned i=0;i<256;++i) assert(objects[i].refs == (mode==2 && i==1 && waits ? 0U : 1U));
    if(mode==2 && waits) assert(replacement.refs==1);
}
'''.replace('POLL_WAIT_SET_TYPE', wait_type)
checks = r'''
int main(void) {
    for(unsigned count=0;count<=256;++count) for(unsigned pattern=0;pattern<3;++pattern) {
        reset(0); linux_pollfd_t fds[256]; unsigned expected=0;
        for(unsigned i=0;i<count;++i) {
            int fd=pattern==0 ? (int)i : pattern==1 ? 1 :
                i%4==0 ? -1 : i%4==1 ? 256 : i%4==2 ? 0 : 1;
            fds[i]=(linux_pollfd_t){.descriptor=fd,.events=1,.revents=123};
            if(fd>=0) ++expected;
        }
        assert(linux_poll(&process,(uint64_t)(uintptr_t)fds,count,0)==expected);
        assert(locks == (count ? 1U : 0U) && !waits);
        assert(allocation_calls == (count > 8U ? 2U : 0U));
        for(unsigned i=0;i<count;++i) {
            int fd=fds[i].descriptor;
            assert(fds[i].revents == (fd<0 ? 0 : fd>=256 || fd%3==0 ? LINUX_POLLNVAL : 1));
        }
        balanced();
    }
    for(unsigned chosen=1;chosen<=5;++chosen) {
        reset(chosen); linux_pollfd_t fds[3]={{1,1,0},{1,1,0},{-1,1,123}};
        int result=(int)linux_poll(&process,(uint64_t)(uintptr_t)fds,3,1);
        assert(result == (chosen<=2 ? 2 : chosen==3 ? -4 : chosen==4 ? K_ENOSPC : 0));
        assert(locks == (chosen==3 || chosen==4 ? 1U : 2U));
        assert(waits == (chosen==4 ? 0U : 1U)); balanced();
        assert(!allocation_calls && !allocations_live);
    }
    reset(5); assert(linux_poll(&process,0,0,1)==0 && !locks && waits==1); balanced();
    for(unsigned fail=0;fail<2;++fail) {
        reset(0); linux_pollfd_t fd={1,1,0}; input_fail=!fail; output_fail=fail;
        assert(linux_poll(&process,(uint64_t)(uintptr_t)&fd,1,0)==LINUX_EFAULT); balanced();
    }
    reset(0); assert(linux_poll(NULL,0,0,0)==K_EINVAL);
    assert(linux_poll(&process,0,257,0)==LINUX_EFAULT && !locks);
    assert(linux_poll_timeout_ns(-1)==UINT64_MAX);
    assert(linux_poll_timeout_ns(INT64_MAX)==UINT64_MAX);
    assert(!allocations_live);
    linux_pollfd_t allocation_fds[9];
    for (unsigned i=0; i<9; ++i) allocation_fds[i]=(linux_pollfd_t){1,1,0};
    for (unsigned fail=1; fail<=2; ++fail) {
        reset(0); allocation_calls=0; allocation_fail=fail;
        assert(linux_poll(&process,(uintptr_t)allocation_fds,9,0)==K_ENOMEM);
        assert(!allocations_live && !locks); balanced();
    }
    reset(1); allocation_fail=3;
    assert(linux_poll(&process,(uintptr_t)allocation_fds,9,1)==K_ENOMEM);
    assert(allocation_calls==3 && !allocations_live && locks==1 && !waits);
    balanced();
    for (unsigned mode_ready=0; mode_ready<2; ++mode_ready) {
        reset(mode_ready); allocation_fail=1;
        linux_pollfd_t small={1,1,0};
        assert(linux_poll(&process,(uintptr_t)&small,1,0)==(mode_ready ? 0 : 1));
        assert(!allocation_calls && !allocations_live);balanced();
    }
    reset(1);allocation_fail=1;linux_pollfd_t small={1,1,0};
    assert(linux_poll(&process,(uintptr_t)&small,1,1)==1);
    assert(!allocation_calls && !allocations_live && waits==1);balanced();
    reset(6);
    assert(linux_poll(&process,(uintptr_t)allocation_fds,9,10)==9);
    assert(waits==3 && allocation_calls==3 && !allocations_live);
    assert(allocation_sizes[2]==9U*3U*(sizeof(void *)+sizeof(uint8_t)));
    balanced();
    /* All legal blocking set sizes use exact capacity and release storage. */
    for (unsigned count=9;count<=256;++count) {
        linux_pollfd_t fds[256];
        for (unsigned i=0;i<count;++i) fds[i]=(linux_pollfd_t){1,1,0};
        reset(1);
        assert(linux_poll(&process,(uintptr_t)fds,count,1)==count);
        assert(allocation_calls==3 && !allocations_live);
        assert(allocation_sizes[2]==count*3U*(sizeof(void *)+sizeof(uint8_t)));
        balanced();
    }
    allocation_fail=0;
    puts("771 poll layouts, duplicate pins, ignored/invalid fds, ready retry, fd reuse, timeout and error cleanup passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-poll-snapshot-') as directory:
    path=Path(directory)
    def run(code):
        (path/'test.c').write_text(fixture+code+checks)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-no-pie',str(path/'test.c'),
                        '-o',str(path/'test')],check=True)
        return subprocess.run([str(path/'test')],capture_output=True,text=True)
    result=run(body)
    assert result.returncode==0,result.stdout+result.stderr
    print(result.stdout.strip())
    negative=body.replace('linux_fd_entry_get(&entries[index]);',
                          'if (false) linux_fd_entry_get(&entries[index]);')
    assert run(negative).returncode != 0, 'Missing snapshot reference accepted'
    print('Negative control rejected: copied descriptors without owned references')
    negative=body.replace('(size_t)count * 3U',
                          '(size_t)LINUX_MAX_POLL_FDS * 3U')
    assert negative!=body and run(negative).returncode!=0
    print('Negative control rejected: full-limit scratch allocation for small waits')
    actual_limit = int(re.search(r'#define OS_LINUX_FD_LIMIT (\d+)U',
        (root/'include/kernel/linux_abi.h').read_text())[1])
    fixture = re.sub(r'\b256\b', str(actual_limit), fixture)
    checks = re.sub(r'\b257\b', str(actual_limit + 1), checks)
    checks = re.sub(r'\b256\b', str(actual_limit), checks)
    result = run(body)
    assert result.returncode == 0, result.stdout + result.stderr
    print('Production descriptor capacity: full poll layouts and allocation failure cleanup passed')
