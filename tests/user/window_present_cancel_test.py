"""Exercise production PRESENT predicate/result on close before fence ACK."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/window.c').read_text()

def function(name):
    start = source.index('static ', source.index(name) - 40)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

predicate = function('window_present_sequence_ready')
result = function('window_present_wait_result')
remove = source[source.index('void remove_window_locked('):
                source.index('kstatus_t window_server_register_manager(')]
assert remove.index('window_registry_remove_locked') < remove.index(
    'atomic_store_explicit(&window->registry_removed, true, memory_order_release)')
assert 'atomic_init(&window->registry_removed, false)' in source
assert 'return window_present_wait_result(window, status);' in source

program = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
typedef int kstatus_t;
enum { K_OK=0, K_ECANCELED=-125, K_ETIMEDOUT=-110 };
typedef struct {
    atomic_bool registry_removed;
    atomic_uint_fast64_t compositor_presented_sequence;
} window_server_window_t;
typedef struct {
    window_server_window_t *window;
    uint64_t sequence;
} window_present_wait_context_t;
''' + predicate + '\n' + result + r'''
int main(void) {
    window_server_window_t window;
    atomic_init(&window.registry_removed, false);
    atomic_init(&window.compositor_presented_sequence, 0);
    window_present_wait_context_t context={&window, 7};
    assert(!window_present_sequence_ready(0));
    assert(!window_present_sequence_ready(&context));
    atomic_store(&window.compositor_presented_sequence, 6);
    assert(!window_present_sequence_ready(&context));
    /* ACK 前关闭必须保持为终态，包括关闭唤醒后才注册等待者的情况。
     * 不得伪造已显示序列号。 */
    atomic_store_explicit(&window.registry_removed, true, memory_order_release);
    assert(window_present_sequence_ready(&context));
    assert(window_present_sequence_ready(&context));
    assert(atomic_load(&window.compositor_presented_sequence)==6);
    assert(window_present_wait_result(&window,K_OK)==K_ECANCELED);
    assert(window_present_wait_result(&window,K_ETIMEDOUT)==K_ETIMEDOUT);
    /* 显示已完成后再关闭，也应安全地进入终态。 */
    atomic_store(&window.compositor_presented_sequence, 7);
    assert(window_present_sequence_ready(&context));
    assert(window_present_wait_result(&window,K_OK)==K_ECANCELED);
    /* 独立的活动窗口仍保留其真实的显示栅栏约定。 */
    atomic_store(&window.registry_removed, false);
    assert(window_present_sequence_ready(&context));
    assert(window_present_wait_result(&window,K_OK)==K_OK);
    atomic_store(&window.compositor_presented_sequence, 6);
    assert(!window_present_sequence_ready(&context));
}
'''
with tempfile.TemporaryDirectory(prefix='system-present-cancel-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PRESENT close-before-ACK, persistent cancellation, no forged fence and live ACK passed')
