"""Production failure counters: bounded admission, exact errors and contention."""
from pathlib import Path
import resource
import subprocess
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/handlers.c').read_text()
start = source.index('static void syscall_record_resource_error(')
brace = source.index('{', start)
depth = 1
end = brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
function = source[start:end]
assert 'static atomic_ullong g_syscall_resource_errors[512][8];' in source
assert 'if (status < 0) syscall_record_resource_error(number, status);' in source
assert source.index('number = frame->rax;') < source.index(
    'int64_t status = linux_syscall_dispatch(frame);')
assert not any(word in function for word in ('printf', 'kmalloc', 'spinlock', 'x86_read_tsc'))
code = r'''
#include <assert.h>
#include <pthread.h>
#include <kernel/base.h>
static atomic_ullong g_syscall_resource_errors[512][8];
FUNCTION
static void *worker(void *unused) {
    (void)unused;
    for (unsigned i=0; i<5000; ++i)
        syscall_record_resource_error(257, K_ENOSPC);
    return 0;
}
int main(void) {
    const int errors[]={K_ENOMEM,K_EBUSY,K_EMFILE,K_ENOSPC,K_EIO,
                        K_EINVAL,K_EACCES,K_EPERM};
    for (int status=-4096; status<=4096; ++status)
        syscall_record_resource_error(0, status);
    for (unsigned c=0;c<8;++c) {
        assert(atomic_load(&g_syscall_resource_errors[0][c])==1);
        syscall_record_resource_error(511, errors[c]);
        syscall_record_resource_error(512, errors[c]);
        syscall_record_resource_error(UINT64_MAX, errors[c]);
    }
    pthread_t threads[8];
    for (unsigned i=0;i<8;++i) assert(!pthread_create(&threads[i],0,worker,0));
    for (unsigned i=0;i<8;++i) assert(!pthread_join(threads[i],0));
    for (unsigned n=1;n<512;++n)
        for (unsigned c=0;c<8;++c)
            assert(atomic_load(&g_syscall_resource_errors[n][c]) ==
                (n==511 ? 1ULL : (n==257 && c==3 ? 40000ULL : 0ULL)));
}
'''.replace('FUNCTION', function)
with tempfile.TemporaryDirectory(prefix='syscall-resource-errors-') as directory:
    path = Path(directory)
    for label, program in (
        ('actual', code),
        ('negative', code.replace(
            '1ULL,\n                              memory_order_relaxed);',
            '2ULL,\n                              memory_order_relaxed);')),
    ):
        if label == 'negative':
            assert program != code
        unit = path / (label + '.c')
        binary = path / label
        unit.write_text(program)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-pthread', '-I' + str(root / 'include'), str(unit),
                        '-o', str(binary)], check=True, timeout=30)
        result = subprocess.run([str(binary)], capture_output=True, timeout=15)
        if label == 'actual':
            assert result.returncode == 0, result.stderr.decode()
        else:
            assert result.returncode != 0 and b'Assertion' in result.stderr
print('Resource error counters: actual helper, exhaustive statuses, bounds, '
      '40000 concurrent increments, quiet path and negative control PASS')
