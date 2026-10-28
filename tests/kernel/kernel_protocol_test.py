"""Freeze diagnostic protocol, serial guard and builtin font during renaming."""
from pathlib import Path
import hashlib
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
headers = list((ROOT / 'include/kernel').glob('*.h'))
headers += list((ROOT / 'include/arch').rglob('*.h'))
headers += [ROOT / name for name in ('include/boot/sha256.h',
                                    'include/console_font_a8.h',
                                    'include/usb/storage.h')]
legacy_pattern = re.compile(r'\b(?:LITE' + r'OS_|lite' + r'os_|g_lite' + r'os_)\w+')
for header in headers:
    remaining = set(legacy_pattern.findall(header.read_text()))
    assert not remaining, (header, remaining)
font = (ROOT / 'include/console_font_a8.h').read_text()
start = font.index('= {') + 2
end = font.index('};', start) + 1
assert hashlib.sha256(font[start:end].encode()).hexdigest() == \
    'a1087b4546d863e977affe5a37409d4a3c4201d14063bf2b677d6d601549eb10'
code = '''
#include <stddef.h>
#include <stdio.h>
#include <kernel/debug_stage.h>
#include <kernel/console.h>
#include <kernel/device.h>
#include <kernel/linux_abi.h>
#include <boot/sha256.h>
#define CHECK(c) _Static_assert((c), #c)
CHECK(OS_DRIVER_API_VERSION == 1);
CHECK(OS_LINUX_FD_LIMIT == 1024);
CHECK(sizeof(((linux_fd_table_t *)0)->entries) / sizeof(linux_fd_entry_t)
      == OS_LINUX_FD_LIMIT);
CHECK(OS_PROCESS_PERSONALITY_NATIVE == 0);
CHECK(OS_PROCESS_PERSONALITY_LINUX_X86_64 == 1);
CHECK(OS_SHA256_DIGEST_SIZE == 32);
CHECK(sizeof(kernel_serial_guard_t) == 16);
CHECK(offsetof(kernel_serial_guard_t, flags) == 0);
CHECK(offsetof(kernel_serial_guard_t, owned) == 8);
CHECK(offsetof(kernel_serial_guard_t, preempt_disabled) == 9);
'''
phases = ('BOOT', 'CPU', 'MEMORY', 'SCHEDULER', 'DRIVER', 'STORAGE',
          'NETWORK', 'DISPLAY', 'USER', 'USER_RUNTIME', 'DESKTOP')
for value, name in enumerate(phases, 1):
    code += f'CHECK(OS_DEBUG_PHASE_{name} == {value});\n'
for index in range(21):
    code += f'CHECK(OS_DEBUG_PHASE_SPEC_{index} == {32 + index});\n'
refactor = ('0', '1', '2', '3', '4', '5', '6', '7A', '7B', '8', '9')
for value, name in enumerate(refactor, 64):
    code += f'CHECK(OS_DEBUG_PHASE_REFACTOR_{name} == {value});\n'
steps = ('ENTER', 'LOAD', 'CREATE', 'START', 'READY', 'EXIT',
         'WAIT_ONE_ENTER', 'WAIT_ONE_BLOCK', 'WAIT_ONE_RETURN',
         'THREAD_EXIT_ENTER', 'THREAD_EXIT_PUBLISHED', 'THREAD_EXIT_SCHEDULE',
         'USER_MARK', 'STRESS', 'PENDING', 'PROGRESS', 'FAIL')
for value, name in enumerate(steps, 1):
    code += f'CHECK(OS_DEBUG_STEP_{name} == {value});\n'
code += 'int main(void) { puts("Kernel protocol: 60 diagnostic values, 1024 descriptor slots, serial guard and font bitmap verified"); }\n'
with tempfile.TemporaryDirectory(prefix='kernel-protocol-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'include'), '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
subprocess.run(['bash', 'tools/verify-roadmap-stages.sh', '--static'],
               cwd=ROOT, check=True)
