"""Execute production snapshot slot pinning and retirement under its lock contract."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/window.c').read_text()
def function(name):
    start = source.index('void ' + name + '(')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
program = r'''
#include <assert.h>
#include "kernel/window_surface_exchange.h"
typedef struct {
    bool surface_exchange_enabled;
    uint64_t surface_span;
    uint32_t surface_front_width,surface_front_height;
    window_surface_exchange_t surface_exchange;
    unsigned surface_waitq;
} window_server_window_t;
typedef struct {
    window_server_window_t *reference;
    uint64_t surface_offset,buffer_size;
    uint32_t surface_slot;
    uint32_t surface_width,surface_height;
    bool surface_lease,resize_pending;
} compositor_window_view_t;
static bool locked;
static unsigned wakes;
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static int wake_all(unsigned *queue){assert(!locked&&queue);++wakes;return 0;}
''' + function('window_surface_snapshot_capture_locked') + function('window_surface_snapshot_release') + r'''
int main(void) {
    window_server_window_t w={.surface_span=4096};
    compositor_window_view_t a={.reference=&w,.buffer_size=4096};
    window_lock();window_surface_snapshot_capture_locked(&a);window_unlock();
    assert(!a.surface_lease && a.surface_offset==0 && a.buffer_size==4096);
    window_surface_snapshot_release(&a);assert(!wakes);
    w.surface_exchange_enabled=true;
    w.surface_front_width=16;w.surface_front_height=12;
    window_lock();window_surface_snapshot_capture_locked(&a);window_unlock();
    assert(a.surface_lease && w.surface_exchange.readers[0]==1);
    assert(a.surface_width==16&&a.surface_height==12);
    uint32_t slot;uint64_t token;
    window_lock();
    assert(window_surface_write_begin(&w.surface_exchange,&slot,&token));
    assert(slot==1);
    assert(window_surface_write_end(&w.surface_exchange,token,true));
    w.surface_front_width=20;w.surface_front_height=15;
    window_unlock();
    compositor_window_view_t b={.reference=&w,.buffer_size=8192};
    window_lock();window_surface_snapshot_capture_locked(&b);window_unlock();
    assert(b.surface_lease && b.surface_offset==4096 && b.buffer_size==4096);
    assert(b.surface_width==20&&b.surface_height==15&&a.surface_width==16&&a.surface_height==12);
    assert(a.surface_offset==0);  // Old snapshot still reads the old frame.
    assert(!window_surface_write_begin(&w.surface_exchange,&slot,&token));
    window_surface_snapshot_release(&a);assert(wakes==1);
    window_surface_snapshot_release(&a);assert(wakes==1);
    assert(window_surface_write_begin(&w.surface_exchange,&slot,&token));
    assert(slot==0);
    assert(window_surface_write_end(&w.surface_exchange,token,false));
    compositor_window_view_t c={.reference=&w};
    window_lock();window_surface_snapshot_capture_locked(&c);window_unlock();
    window_surface_snapshot_release(&b);assert(wakes==1);
    window_surface_exchange_close(&w.surface_exchange);
    window_surface_snapshot_release(&c);assert(wakes==1);
    assert(!w.surface_exchange.readers[0]&&!w.surface_exchange.readers[1]);
    compositor_window_view_t closed={.reference=&w};
    window_lock();window_surface_snapshot_capture_locked(&closed);window_unlock();
    assert(!closed.surface_lease && closed.resize_pending);
}
'''
with tempfile.TemporaryDirectory(prefix='window-surface-snapshot-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(root / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=20)
print('Snapshot slots: immutable offset, old-front reuse blocked, shared readers, '
      'single unlocked wake, idempotent retire, abort, close and legacy mode passed')
