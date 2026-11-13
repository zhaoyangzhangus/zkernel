"""Exercise the shared front/back ownership policy, not a copied implementation."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <string.h>
#include "kernel/window_surface_exchange.h"
int main(void) {
    window_surface_exchange_t s = {0};
    uint32_t pixels[2][256] = {{0}};
    uint32_t back, held, current;
    uint64_t token, old;
    assert(window_surface_read_begin(&s, &held) && held == 0);
    assert(window_surface_write_begin(&s, &back, &token) && back == 1);
    old = token;
    for (unsigned i = 0; i < 128; ++i) pixels[back][i] = 42;
    assert(window_surface_read_begin(&s, &current) && current == held);
    for (unsigned i = 0; i < 256; ++i) assert(pixels[current][i] == 0);
    assert(!window_surface_write_begin(&s, &back, &token));
    assert(!window_surface_write_end(&s, old + 1, true));
    for (unsigned i = 128; i < 256; ++i) pixels[back][i] = 42;
    assert(window_surface_write_end(&s, old, true));
    assert(s.front == 1 && pixels[held][255] == 0);
    assert(!window_surface_write_begin(&s, &back, &token));
    assert(window_surface_read_end(&s, held));
    assert(!window_surface_write_begin(&s, &back, &token));
    assert(window_surface_read_end(&s, current));
    assert(!window_surface_read_end(&s, current));
    assert(!window_surface_read_end(&s, 2));
    for (unsigned frame = 1; frame < 10000; ++frame) {
        assert(window_surface_write_begin(&s, &back, &token));
        assert(token > old);
        assert(!window_surface_write_end(&s, old, true));
        old = token;
        memset(pixels[back], 0xff, sizeof(pixels[back]) / 2);
        assert(window_surface_read_begin(&s, &held));
        assert(held != back);
        assert(window_surface_write_end(&s, token, false));
        assert(s.front == held);  // Partial failure never publishes.
        assert(window_surface_write_begin(&s, &back, &token));
        for (unsigned i = 0; i < 256; ++i) pixels[back][i] = frame;
        assert(window_surface_write_end(&s, token, true));
        assert(window_surface_read_begin(&s, &current));
        for (unsigned i = 0; i < 256; ++i) assert(pixels[current][i] == frame);
        assert(window_surface_read_end(&s, current));
        assert(!window_surface_write_begin(&s, &back, &token));
        assert(window_surface_read_end(&s, held));
    }
    assert(window_surface_write_begin(&s, &back, &token));
    assert(window_surface_read_begin(&s, &held));
    window_surface_exchange_close(&s);
    assert(!window_surface_write_end(&s, token, true));
    assert(!window_surface_write_begin(&s, &back, &token));
    assert(!window_surface_read_begin(&s, &current));
    assert(window_surface_read_end(&s, held));
    s = (window_surface_exchange_t){ .serial = UINT64_MAX };
    assert(!window_surface_write_begin(&s, &back, &token));
    s.readers[0] = UINT32_MAX;
    assert(!window_surface_read_begin(&s, &held));
}
'''
with tempfile.TemporaryDirectory(prefix='window-surface-exchange-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(root / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=20)
    # 负对照：在写入开始时发布会复现原先半帧可见的问题，并必须违反相同的像素断言。
    header = (root / 'include/kernel/window_surface_exchange.h').read_text()
    broken = header.replace('state->writer_slot = back;',
                            'state->writer_slot = back; state->front = back;')
    assert broken != header
    (path / 'broken.c').write_text(program.replace(
        '#include "kernel/window_surface_exchange.h"', broken))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'broken.c'), '-o', str(path / 'broken')], check=True)
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([str(path / 'broken')], capture_output=True,
                            preexec_fn=no_core, timeout=20)
    assert result.returncode == -signal.SIGABRT, result.stderr.decode()
print('Surface exchange: partial writes, pinned snapshots, abort, stale tokens, '
      '10000-frame reuse, close, counter overflow and negative control passed')
