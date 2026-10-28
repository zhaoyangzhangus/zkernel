"""Compile the complete compatibility probe and exercise its real log helpers."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'tools/chromium/linux_events_test.c'
source = SOURCE.read_text()
assert not re.search(r'LiteOS|LITEOS_|liteos-', source)
assert '#include <poll.h>' in source
assert 'struct pollfd {' not in source and 'extern int poll(' not in source
markers = sorted(set(re.findall(r'"(OS_CHROMIUM_[A-Z0-9_]+)', source)))
assert len(markers) >= 100
assert 'OS_CHROMIUM_LINUX_EVENTS_PASS' in markers
assert 'OS_CHROMIUM_SELF_EXEC_PASS' in markers
paths = re.findall(r'char path\[\] = "(/tmp/[^"\n]+)";', source)
assert len(paths) == 5 and all(path.startswith('/tmp/system-') for path in paths)
assert sum(path.endswith('XXXXXX') for path in paths) == 4
assert 'char *digits = path + sizeof(path) - 5U;' in source

helpers = source[source.index('static void emit('):
                 source.index('static int test_tcp_proxy_connect(')]
program = '''
#include <assert.h>
#include <string.h>
#include <unistd.h>
static int g_failure_code;
''' + helpers + '\nint main(void) {\n'
for marker in markers:
    program += f'emit("{marker}\\n");\n'
codes = (1, 31, 49, 68, 69, 70, 127)
for code in codes:
    program += (f'g_failure_code = {code}; '
                f'assert(fail("OS_CHROMIUM_LINUX_EVENTS_FAIL\\n") == {code});\n')
program += 'emit(""); return 0; }\n'

with tempfile.TemporaryDirectory(prefix='system-events-probe-') as directory:
    path = Path(directory)
    # Code generation, not only syntax checking, catches dormant TU errors.
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-ffreestanding', '-I' + str(ROOT / 'user/libc/include'),
                    '-I' + str(ROOT / 'include'), '-c', str(SOURCE),
                    '-o', str(path / 'probe.o')], check=True, timeout=30)
    (path / 'emit.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'emit.c'), '-o', str(path / 'emit')],
                   check=True, timeout=10)
    result = subprocess.run([str(path / 'emit')], check=True,
                            capture_output=True, text=True, timeout=10)
    expected = markers + ['OS_CHROMIUM_LINUX_EVENTS_FAIL'] * len(codes)
    assert result.stdout.splitlines() == expected, result.stdout
    (path / 'poll.c').write_text('''
#include <stddef.h>
#include <poll.h>
_Static_assert(sizeof(struct pollfd) == 8, "pollfd size");
_Static_assert(offsetof(struct pollfd, fd) == 0, "pollfd fd");
_Static_assert(offsetof(struct pollfd, events) == 4, "pollfd events");
_Static_assert(offsetof(struct pollfd, revents) == 6, "pollfd revents");
_Static_assert(sizeof(nfds_t) == 8, "poll count ABI");
''')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-ffreestanding', '-I' + str(ROOT / 'user/libc/include'),
                    '-fsyntax-only', str(path / 'poll.c')], check=True, timeout=10)

print(f'Linux events probe: full TU code generation, {len(markers)} neutral markers, '
      'real emit/failure helpers, seven failure codes, five temporary paths and poll ABI passed')
