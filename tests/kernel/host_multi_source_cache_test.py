"""Exercise the production multi-source dependency/link rule with distinct headers."""
from pathlib import Path
import os
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'tools/host-tests.mk').read_text()
macro = 'define HOST_TEST_LINK\n' + source.split('define HOST_TEST_LINK\n', 1)[1].split('endef', 1)[0] + 'endef\n'
with tempfile.TemporaryDirectory(prefix='liteos-host-multi-cache-') as directory:
    base = Path(directory)
    (base / 'first.h').write_text('#define FIRST 1\n')
    (base / 'second.h').write_text('#define SECOND 2\n')
    (base / 'first.c').write_text('#include "first.h"\nint value(void); int main(void){return FIRST+value();}\n')
    library = '#include "second.h"\nint value(void){return SECOND;}\n'
    (base / 'second.c').write_text(library)
    (base / 'Makefile').write_text('HOSTCC=gcc\n' + macro +
        'test.exe: first.c second.c\n\t$(call HOST_TEST_LINK,-O2 -Wall -Wextra -Werror)\n'
        '-include test.exe.d\n')
    def run(success=True):
        result = subprocess.run(['make', 'test.exe'], cwd=base, capture_output=True, text=True)
        assert (result.returncode == 0) == success, result.stdout + result.stderr
    def identity():
        target = base / 'test.exe'
        return target.stat().st_ino, target.stat().st_mtime_ns, target.read_bytes()
    def status():
        return subprocess.run([str(base / 'test.exe')]).returncode
    run()
    assert status() == 3
    dependencies = (base / 'test.exe.d').read_text()
    assert 'first.h' in dependencies and 'second.h' in dependencies
    before = identity()
    run()
    assert identity() == before
    (base / 'first.h').write_text('#define FIRST 7\n')
    run()
    assert status() == 9
    (base / 'second.h').write_text('#define SECOND 11\n')
    run()
    assert status() == 18
    # Chrome 启动器测试夹具会直接包含其实现文件。其 .c 依赖必须使目标失效，但不能
    # 被重复链接。
    (base / 'included.c').write_text('#include "implementation.c"\nint main(void){return implementation();}\n')
    (base / 'implementation.c').write_text('int implementation(void){return 4;}\n')
    with (base / 'Makefile').open('a') as stream:
        stream.write('included.exe: included.c implementation.c\n'
                     '\t$(call HOST_TEST_LINK,-O2 -Wall -Wextra -Werror,$<)\n'
                     '-include included.exe.d\n')
    subprocess.run(['make', 'included.exe'], cwd=base, check=True, capture_output=True)
    assert subprocess.run([str(base / 'included.exe')]).returncode == 4
    (base / 'implementation.c').write_text('int implementation(void){return 8;}\n')
    subprocess.run(['make', 'included.exe'], cwd=base, check=True, capture_output=True)
    assert subprocess.run([str(base / 'included.exe')]).returncode == 8
    before = identity()
    receipt = (base / 'test.exe.d').read_bytes()
    (base / 'second.c').write_text('not valid C\n')
    run(success=False)
    assert identity() == before
    assert (base / 'test.exe.d').read_bytes() == receipt
    assert not list(base.glob('*.tmp*'))
    (base / 'second.c').write_text('#include "missing.h"\n')
    run(success=False)
    assert identity() == before
    assert (base / 'test.exe.d').read_bytes() == receipt
    assert not list(base.glob('*.tmp*'))
    (base / 'second.c').write_text(library)
    run()
    assert status() == 18
    before = identity()
    receipt = (base / 'test.exe.d').read_bytes()
    (base / 'second.c').write_text(library + '\n')
    wrapper = base / 'interrupted-compiler'
    wrapper.write_text('#!/bin/sh\nfor arg in "$@"; do\n'
                       'if [ "$arg" = "-MM" ]; then exec ' + shutil.which('gcc') + ' "$@"; fi\n'
                       'done\ntouch compile-running\nsleep 30\n')
    wrapper.chmod(0o755)
    process = subprocess.Popen(['make', 'test.exe', 'HOSTCC=' + str(wrapper)],
                               cwd=base, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               start_new_session=True)
    try:
        deadline = time.monotonic() + 3
        while not (base / 'compile-running').exists() and time.monotonic() < deadline:
            assert process.poll() is None
            time.sleep(.01)
        assert (base / 'compile-running').exists()
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
        interruption_output = process.communicate(timeout=5)
    assert identity() == before
    assert (base / 'test.exe.d').read_bytes() == receipt
    assert not list(base.glob('*.tmp*')), (list(base.glob('*.tmp*')), interruption_output)
print('Multi-source host cache: both headers tracked, no-op reuse, failed link preserves executable/dependencies')
