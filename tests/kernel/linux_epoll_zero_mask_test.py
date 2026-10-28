"""Zero-mask error delivery is distinct from a disarmed ONESHOT watch."""
from pathlib import Path
import select
import socket
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()

# Compare the zero-mask contract with the host's real Linux epoll first.
with select.epoll() as epoll:
    left, right = socket.socketpair()
    try:
        epoll.register(left.fileno(), 0)
        assert epoll.poll(0) == []
        right.close()
        assert epoll.poll(1)[0][1] & select.EPOLLHUP
        epoll.modify(left.fileno(), select.EPOLLONESHOT)
        assert epoll.poll(1)[0][1] & select.EPOLLHUP
        assert epoll.poll(0) == []
        epoll.modify(left.fileno(), 0)
        assert epoll.poll(1)[0][1] & select.EPOLLHUP
    finally:
        left.close()
        right.close()

types = source[source.index('typedef struct linux_epoll_watch {'):
               source.index('typedef struct linux_eventfd_object {')]
requests = source[source.index('static bool linux_epoll_read_requested('):
                  source.index('static bool linux_eventfd_can_write(')]
start = source.index('static bool linux_epoll_watch_ready(')
guard = source[start:source.index('    switch (watch->kind)', start)]
start = source.index('    case LINUX_FD_SOCKET:', start)
socket_case = source[start:source.index('    case LINUX_FD_UNIX_SOCKET:', start)]
start = source.index('static int64_t linux_epoll_collect(')
collect = source[start:source.index('typedef struct linux_epoll_wait_context', start)]
start = source.index('static bool linux_epoll_watch_has_wait_source(')
wait_source = source[start:source.index('static bool linux_epoll_wait_set_contains(', start)]
start = source.index('        watch->events = event.events;', source.index('static int64_t linux_epoll_ctl('))
rearm = source[start:source.index('    }\n    ++epoll->control_sequence;', start)]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdatomic.h>
#define OS_DEBUG_SERIAL 0
#define LINUX_MAX_EPOLL_WATCHES 8
#define K_EINVAL -22
#define LINUX_EPOLLIN 1U
#define LINUX_EPOLLPRI 2U
#define LINUX_EPOLLOUT 4U
#define LINUX_EPOLLERR 8U
#define LINUX_EPOLLHUP 16U
#define LINUX_EPOLLRDNORM 64U
#define LINUX_EPOLLWRNORM 256U
#define LINUX_EPOLLMSG 1024U
#define LINUX_EPOLLRDHUP 8192U
#define LINUX_EPOLLONESHOT (1U << 30)
#define SOCKET_POLL_READ 1U
#define SOCKET_POLL_WRITE 2U
#define SOCKET_POLL_ERROR 4U
#define SOCKET_POLL_HUP 8U
enum { LINUX_FD_SOCKET, LINUX_FD_NVIDIA, LINUX_FD_PIPE,
       LINUX_FD_UNIX_SOCKET, LINUX_FD_WINDOW, LINUX_FD_EVENTFD, LINUX_FD_FILE };
typedef unsigned object_header_t;
typedef unsigned spinlock_t;
typedef void file_t;
typedef struct { uint32_t state; } socket_t;
typedef struct { uint32_t events; uint64_t data; } linux_epoll_event_t;
static uint32_t socket_poll_state(socket_t *s) { return s->state; }
static void spinlock_lock(spinlock_t *l) { assert(!*l); *l=1; }
static void spinlock_unlock(spinlock_t *l) { assert(*l); *l=0; }
static uint32_t nv_frontend_poll_state(void *p, bool consume) {
    (void)p; (void)consume; assert(false); return 0;
}
''' + types + requests + guard + r'''
    switch (watch->kind) {
''' + socket_case + r'''
    default: break;
    }
    if (ready_events) *ready_events=ready;
    return ready != 0;
}
''' + collect + wait_source + r'''
static void rearm_watch(linux_epoll_watch_t *watch, linux_epoll_event_t event) {
''' + rearm + r'''
}
int main(void) {
    socket_t socket={0};
    linux_epoll_object_t epoll={.watch_limit=1};
    linux_epoll_watch_t *watch=&epoll.watches[0];
    *watch=(linux_epoll_watch_t){.used=1,.kind=LINUX_FD_SOCKET,
                               .private_data=&socket,.data=123};
    linux_epoll_event_t events[2]={{0}};
    assert(linux_epoll_watch_has_wait_source(watch));
    assert(!linux_epoll_collect(&epoll,events,2));
    socket.state=SOCKET_POLL_READ | SOCKET_POLL_WRITE;
    assert(!linux_epoll_collect(&epoll,events,2));
    for (unsigned state=SOCKET_POLL_ERROR; state<=12; state+=4) {
        socket.state=state;
        uint32_t expected=(state & SOCKET_POLL_ERROR ? LINUX_EPOLLERR : 0) |
                          (state & SOCKET_POLL_HUP ? LINUX_EPOLLHUP : 0);
        for (unsigned repeat=0; repeat<4; ++repeat) {
            assert(linux_epoll_collect(&epoll,events,2)==1);
            assert(events[0].events==expected && events[0].data==123);
        }
        rearm_watch(watch,(linux_epoll_event_t){LINUX_EPOLLONESHOT,456});
        assert(linux_epoll_collect(&epoll,events,2)==1);
        assert(events[0].events==expected && events[0].data==456);
        assert(!linux_epoll_collect(&epoll,events,2));
        assert(!linux_epoll_watch_has_wait_source(watch));
        rearm_watch(watch,(linux_epoll_event_t){0,123});
        assert(linux_epoll_watch_has_wait_source(watch));
        assert(linux_epoll_collect(&epoll,events,2)==1);
    }
    socket.state=SOCKET_POLL_READ;
    rearm_watch(watch,(linux_epoll_event_t){LINUX_EPOLLIN|LINUX_EPOLLONESHOT,789});
    assert(linux_epoll_collect(&epoll,events,2)==1 && events[0].events==LINUX_EPOLLIN);
    socket.state=SOCKET_POLL_HUP;
    assert(!linux_epoll_collect(&epoll,events,2));
    rearm_watch(watch,(linux_epoll_event_t){0,123});
    assert(linux_epoll_collect(&epoll,events,2)==1 && events[0].events==LINUX_EPOLLHUP);
    watch->used=0;
    assert(!linux_epoll_collect(&epoll,events,2));
    assert(!linux_epoll_watch_has_wait_source(watch));
    puts("EPOLL_ZERO_MASK_ONESHOT_TEST_OK");
}
'''

with tempfile.TemporaryDirectory(prefix='epoll-zero-mask-') as directory:
    fixture = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    def run(text, expected):
        fixture.write_text(text)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(fixture), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        assert (result.returncode == 0) == expected, result.stdout + result.stderr
        if expected:
            print(result.stdout.strip())
    run(code, True)
    # Both halves matter: restoring the old guard or failing to rearm must fail.
    assert '|| watch->disabled) return false;' in code
    run(code.replace('|| watch->disabled) return false;',
                     '|| watch->events == 0U) return false;', 1), False)
    run(code.replace('watch->disabled = 0U;', '/* missing rearm */'), False)
print('Linux reference, production zero-mask ERR/HUP, ONESHOT, rearm and negative controls passed')
