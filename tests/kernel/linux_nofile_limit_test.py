"""The actual prlimit implementation must report the actual descriptor budget."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
start = source.index('static int64_t linux_prlimit64(')
body = source[start:source.index('\n}', start) + 2]
limit = int(re.search(r'#define OS_LINUX_FD_LIMIT (\d+)U',
                     (root / 'include/kernel/linux_abi.h').read_text())[1])
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define __user
#define OS_LINUX_FD_LIMIT LIMIT_VALUE
#define K_OK 0
#define K_EINVAL -22
#define K_ENOSYS -38
#define LINUX_EFAULT -14
#define LINUX_RLIMIT_STACK 3
#define LINUX_RLIMIT_NLIMITS 16
#define LINUX_RLIM_INFINITY UINT64_MAX
typedef struct { uint64_t current, maximum; } linux_rlimit_t;
static int copy_to_user(void *to, const void *from, unsigned long size) {
    if (!to) return -1;
    memcpy(to, from, size); return 0;
}
'''.replace('LIMIT_VALUE', str(limit))
checks = r'''
int main(void) {
    linux_rlimit_t value;
    assert(linux_prlimit64(0, 7, 0, (uintptr_t)&value) == 0);
    assert(value.current == OS_LINUX_FD_LIMIT);
    assert(value.maximum == OS_LINUX_FD_LIMIT);
    assert(linux_prlimit64(0, 3, 0, (uintptr_t)&value) == 0);
    assert(value.current == 8 * 1024 * 1024);
    assert(linux_prlimit64(0, 16, 0, (uintptr_t)&value) == K_EINVAL);
    assert(linux_prlimit64(0, 7, 1, (uintptr_t)&value) == K_ENOSYS);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    for budget in (limit, 73):
        (path / 'test.c').write_text(fixture.replace(str(limit), str(budget)) + body + checks)
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)
print('Linux NOFILE budget matches descriptor capacity; alternate-budget regression passed')
