"""Compile actual launch policy; cover close/request/exit interleavings."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/launcher.c').read_text()

def extract(name):
    match = re.search(r'^(?:static )?(?:bool|void) ' + name + r'\(', source, re.M)
    assert match, name
    opening = source.index('{', match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

names = ['desktop_chrome_launch_claim', 'desktop_chrome_launch_publish_pid',
         'desktop_chrome_launch_release', 'desktop_chrome_request_launch',
         'desktop_chrome_window_opened_locked', 'desktop_chrome_window_closed_locked',
         'desktop_chrome_process_released_locked', 'desktop_chrome_launch_guard_self_test',
         'desktop_launch_worker_ready', 'desktop_launch_dequeue']
window = (root / 'kernel/graphics/window.c').read_text()
append = window[window.index('bool window_registry_append_locked('):
                window.index('bool window_registry_remove_locked(')]
assert 'desktop_chrome_window_opened_locked(window->owner);' in append
remove = window[window.index('void remove_window_locked('):
                window.index('kstatus_t window_server_register_manager(')]
assert remove.index('window_registry_remove_locked') < remove.index(
    'desktop_chrome_window_closed_locked(window->owner);') < remove.index('object_put(window)')
close = window[window.index('void window_server_close_process('):
               window.index('kstatus_t window_server_set_owner_address(')]
assert close.index('desktop_chrome_process_released_locked') < close.index(
    'window_unlock()') < close.index('wake_one(')
assert 'if (!desktop_chrome_request_launch())' in source

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <pthread.h>
#define DESKTOP_CHROME_LAUNCH_STARTING UINT64_MAX
#define DESKTOP_APP_CHROME 6U
#define WINDOW_DESKTOP_LAUNCH_QUEUE_CAPACITY 8U
typedef struct { uint64_t pid; } process_t;
typedef struct { process_t *owner; } window_server_window_t;
static struct {
    atomic_uint_fast64_t desktop_chrome_launch_pid;
    atomic_bool desktop_chrome_relaunch_pending;
    uint64_t desktop_chrome_closing_pid;
    atomic_uint desktop_launch_queue_count;
    uint32_t desktop_launch_queue[8], desktop_launch_queue_head;
    struct {uint32_t count;window_server_window_t *children[8];} scene;
} g_window_server;
static _Thread_local bool locked;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static void window_lock(void) { assert(!locked); assert(!pthread_mutex_lock(&mutex)); locked=true; }
static void window_unlock(void) { assert(locked); locked=false; assert(!pthread_mutex_unlock(&mutex)); }
''' + '\n'.join(extract(name) for name in names) + r'''
static atomic_uint racers_ready;
static void race_start(void) {
    atomic_fetch_add(&racers_ready,1);
    while (atomic_load(&racers_ready)!=2) {}
}
static void *race_request(void *unused) {
    (void)unused; race_start(); (void)desktop_chrome_request_launch(); return 0;
}
static void *race_exit(void *unused) {
    (void)unused; race_start();
    window_lock(); (void)desktop_chrome_process_released_locked(42); window_unlock();
    return 0;
}
int main(void) {
    process_t old={42}, next={43};
    window_server_window_t first={&old}, popup={&old};
    uint32_t app=0;
    assert(desktop_chrome_launch_guard_self_test());
    for (unsigned repeat=0; repeat<1000; ++repeat) {
        atomic_store(&g_window_server.desktop_chrome_launch_pid,0);
        atomic_store(&g_window_server.desktop_chrome_relaunch_pending,false);
        g_window_server.desktop_chrome_closing_pid=0;
        g_window_server.scene.count=0;
        assert(desktop_chrome_request_launch());
        assert(!desktop_chrome_request_launch()); /* Startup coalesces. */
        assert(!atomic_load(&g_window_server.desktop_chrome_relaunch_pending));
        assert(desktop_chrome_launch_publish_pid(
            &g_window_server.desktop_chrome_launch_pid,old.pid));
        g_window_server.scene.children[0]=&first;
        g_window_server.scene.count=1;
        window_lock(); desktop_chrome_window_opened_locked(&old); window_unlock();
        window_lock(); desktop_chrome_window_closed_locked(&old); window_unlock();
        assert(g_window_server.desktop_chrome_closing_pid==0); /* Other window remains. */
        assert(!desktop_chrome_request_launch());
        assert(!atomic_load(&g_window_server.desktop_chrome_relaunch_pending));
        g_window_server.scene.count=0;
        window_lock(); desktop_chrome_window_closed_locked(&old); window_unlock();
        assert(!desktop_chrome_request_launch());
        assert(!desktop_chrome_request_launch());
        assert(!desktop_launch_worker_ready(0)); /* No spinning while old PID lives. */
        g_window_server.desktop_launch_queue[0]=2;
        g_window_server.desktop_launch_queue_head=0;
        atomic_store(&g_window_server.desktop_launch_queue_count,1);
        assert(desktop_launch_worker_ready(0));
        assert(desktop_launch_dequeue(&app) && app==2); /* Other apps progress. */
        assert(!desktop_launch_dequeue(&app));
        /* A recreated window satisfies the request, not a future auto-restart. */
        g_window_server.scene.children[0]=&popup;
        g_window_server.scene.count=1;
        window_lock(); desktop_chrome_window_opened_locked(&old); window_unlock();
        assert(!atomic_load(&g_window_server.desktop_chrome_relaunch_pending));
        g_window_server.scene.count=0;
        window_lock(); desktop_chrome_window_closed_locked(&old); window_unlock();
        assert(!desktop_chrome_request_launch());
        window_lock(); assert(!desktop_chrome_process_released_locked(41)); window_unlock();
        assert(atomic_load(&g_window_server.desktop_chrome_relaunch_pending));
        window_lock(); assert(desktop_chrome_process_released_locked(old.pid)); window_unlock();
        assert(desktop_launch_worker_ready(0));
        assert(desktop_launch_dequeue(&app) && app==DESKTOP_APP_CHROME);
        assert(!desktop_launch_dequeue(&app));
        assert(desktop_chrome_request_launch()); /* Fallback beats dequeued request. */
        assert(!desktop_chrome_request_launch());
        assert(desktop_chrome_launch_publish_pid(
            &g_window_server.desktop_chrome_launch_pid,next.pid));
        window_lock(); desktop_chrome_window_closed_locked(&next); window_unlock();
        assert(!desktop_chrome_request_launch());
        window_lock(); assert(!desktop_chrome_process_released_locked(old.pid)); window_unlock();
        assert(atomic_load(&g_window_server.desktop_chrome_launch_pid)==next.pid);
        assert(atomic_load(&g_window_server.desktop_chrome_relaunch_pending));
        window_lock(); assert(desktop_chrome_process_released_locked(next.pid)); window_unlock();
        /* New direct request also consumes pending, even before dequeue. */
        assert(desktop_chrome_request_launch());
        assert(!desktop_launch_worker_ready(0));
        assert(!desktop_launch_dequeue(&app));
        assert(desktop_chrome_launch_release(
            &g_window_server.desktop_chrome_launch_pid,DESKTOP_CHROME_LAUNCH_STARTING));
    }
    /* Real simultaneous request/exit: either immediate claim or one pending
     * launch, never the old lost-request state (idle with no pending request). */
    for (unsigned repeat=0; repeat<1000; ++repeat) {
        pthread_t request, exit;
        atomic_store(&g_window_server.desktop_chrome_launch_pid,42);
        atomic_store(&g_window_server.desktop_chrome_relaunch_pending,false);
        g_window_server.desktop_chrome_closing_pid=42;
        atomic_store(&racers_ready,0);
        assert(!pthread_create(&request,0,race_request,0));
        assert(!pthread_create(&exit,0,race_exit,0));
        assert(!pthread_join(request,0)); assert(!pthread_join(exit,0));
        uint64_t pid=atomic_load(&g_window_server.desktop_chrome_launch_pid);
        if (pid==0) {
            assert(desktop_launch_worker_ready(0));
            assert(desktop_launch_dequeue(&app) && app==DESKTOP_APP_CHROME);
            assert(desktop_chrome_request_launch());
        } else {
            assert(pid==DESKTOP_CHROME_LAUNCH_STARTING);
            assert(!atomic_load(&g_window_server.desktop_chrome_relaunch_pending));
        }
        assert(!desktop_launch_dequeue(&app));
    }
    atomic_store(&g_window_server.desktop_chrome_launch_pid,0);
    /* Exit before the user's next click: claim immediately, no lost request. */
    assert(desktop_chrome_request_launch());
    assert(desktop_chrome_launch_publish_pid(&g_window_server.desktop_chrome_launch_pid,42));
    window_lock(); assert(!desktop_chrome_process_released_locked(42)); window_unlock();
    assert(desktop_chrome_request_launch());
    assert(!locked);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-chrome-relaunch-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Chrome relaunch: 1000 policy sequences + 1000 concurrent request/exit races, FIFO and lifetime hooks passed')
