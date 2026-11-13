"""Neutral libc implementation names retain public header semantics."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
public_api = (ROOT/'user/libc/include/libc/libc.h').read_text()
assert 'liteos_syscall6' not in public_api and 'liteos_linux_syscall6' not in public_api
for name in ('__libc_native_syscall6', '__libc_linux_syscall6'):
    assert name in public_api
for file in ('complex.c', 'posix_compat.c', 'stdio.c', 'syscall.c',
             'include/endian.h', 'include/math.h', 'include/tgmath.h',
             'include/libc/libc.h', 'include/sys/select.h'):
    text = (ROOT / 'user/libc' / file).read_text()
    assert not re.search(r'^\s*#\s*(?:define|ifndef)\s+LITEOS_', text, re.M), file
    assert not re.search(r'\bg_liteos_\w+', text), file
select_header = (ROOT / 'user/libc/include/sys/select.h').read_text()
assert 'liteos_fd_' not in select_header
for operation in ('zero', 'set', 'clr', 'isset'):
    result = 'int' if operation == 'isset' else 'void'
    assert f'static inline {result} libc_fd_{operation}(' in select_header
code = r'''
#include <libc/libc.h>
#include <endian.h>
#include <tgmath.h>
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
_Static_assert(sizeof(FILE) == 80, "stdio layout unchanged");
_Static_assert(sizeof(fd_set) == 32 && FD_SETSIZE == 256, "fd set layout unchanged");
_Static_assert(LIBC_FD_WORD_BITS == 64 && LIBC_FD_WORD_COUNT == 4, "fd word dimensions");
_Static_assert(_Generic(sqrt(1.0f), float: 1, default: 0), "float generic math");
_Static_assert(_Generic(sqrt(1.0), double: 1, default: 0), "double generic math");
_Static_assert(_Generic(sqrt(1.0L), long double: 1, default: 0), "long double generic math");
int main(void) {
    fd_set set;
    FD_ZERO(&set);
    for (int i = 0; i < FD_SETSIZE; ++i) CHECK(!FD_ISSET(i, &set));
    for (int i = 0; i < FD_SETSIZE; ++i) FD_SET(i, &set);
    for (int i = 0; i < FD_SETSIZE; ++i) CHECK(FD_ISSET(i, &set));
    FD_CLR(63, &set); FD_CLR(64, &set); FD_CLR(255, &set);
    CHECK(!FD_ISSET(63, &set) && !FD_ISSET(64, &set) && !FD_ISSET(255, &set));
    CHECK(FD_ISSET(62, &set) && FD_ISSET(65, &set) && FD_ISSET(254, &set));
    FD_SET(-1, &set); FD_SET(256, &set); FD_CLR(-1, &set); FD_CLR(256, &set);
    CHECK(!FD_ISSET(-1, &set) && !FD_ISSET(256, &set));
    FD_ZERO(0); FD_SET(0, 0); FD_CLR(0, 0); CHECK(!FD_ISSET(0, 0));
    int descriptor = 0;
    fd_set *pointer = &set;
    FD_ZERO(pointer++); CHECK(pointer == &set + 1);
    pointer = &set;
    FD_SET(descriptor++, pointer++); CHECK(descriptor == 1 && pointer == &set + 1);
    pointer = &set; descriptor = 0;
    CHECK(FD_ISSET(descriptor++, pointer++));
    CHECK(descriptor == 1 && pointer == &set + 1);
    pointer = &set; descriptor = 0;
    FD_CLR(descriptor++, pointer++); CHECK(descriptor == 1 && pointer == &set + 1);
    CHECK(!FD_ISSET(0, &set));
    CHECK(htobe16(0x1234U) == 0x3412U);
    CHECK(htobe32(0x12345678U) == 0x78563412U);
    CHECK(be64toh(htobe64(UINT64_C(0x123456789abcdef0))) == UINT64_C(0x123456789abcdef0));
    CHECK(le32toh(htole32(0x12345678U)) == 0x12345678U);
    return 0;
}
'''
flags = ('READ', 'WRITE', 'APPEND', 'EOF', 'ERROR', 'OWN_BUFFER',
         'LINE_BUFFERED', 'CLOSED', 'READ_BUFFER', 'WRITE_BUFFER')
for field, offset in (('descriptor', 0), ('flags', 4), ('orientation', 8),
                      ('buffer', 16), ('buffer_size', 24), ('buffer_position', 32),
                      ('buffer_length', 40), ('ungot', 48), ('wide_ungot', 52),
                      ('wide_ungot_length', 56), ('next', 64), ('temporary_path', 72)):
    code += f'_Static_assert(offsetof(FILE, {field}) == {offset}, "FILE.{field}");\n'
for i, flag in enumerate(flags):
    code += f'_Static_assert(LIBC_FILE_{flag} == (1U << {i}), "{flag}");\n'
with tempfile.TemporaryDirectory(prefix='libc-namespace-') as directory:
    source = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    source.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(ROOT / 'user/libc/include'), '-I', str(ROOT / 'include'),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    cpp = Path(directory) / 'select.cpp'
    cpp.write_text(r'''
#include <sys/select.h>
static_assert(sizeof(fd_set) == 32 && FD_SETSIZE == 256, "fd set C++ layout");
int main() {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(255, &set);
    if (!FD_ISSET(255, &set) || FD_ISSET(0, &set)) return 1;
    FD_CLR(255, &set);
    FD_SET(-1, &set); FD_CLR(256, &set); FD_ZERO(nullptr);
    return FD_ISSET(255, &set) || FD_ISSET(-1, &set) || FD_ISSET(256, &set);
}
''')
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(ROOT / 'user/libc/include'), str(cpp),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    for header in ('libc', 'mbstate', 'utf8'):
        alias = ROOT / 'user/libc/include/liteos' / (header + '.h')
        assert alias.read_text().splitlines() == [
            '#pragma once',
            '/* Source compatibility for the external browser toolchain. */',
            f'#include <libc/{header}.h>']
        outputs = []
        for namespace in ('libc', 'liteos'):
            source.write_text(f'#include <{namespace}/{header}.h>\n')
            outputs.append(subprocess.check_output([
                'cc', '-E', '-P', '-I', str(ROOT / 'user/libc/include'),
                '-I', str(ROOT / 'include'), str(source)]))
        # GCC 每消费一个 #pragma once 都会输出一个空行。前置包含会多出这样一行，
        # 但不能改变任何声明行或词法单元行。
        lines = [tuple(line for line in output.splitlines() if line.strip())
                 for output in outputs]
        assert lines[0] == lines[1], header
    for compiler, extension, assertion in (('cc', 'c', '_Static_assert'),
                                            ('c++', 'cpp', 'static_assert')):
        mixed = Path(directory) / ('mixed.' + extension)
        mixed.write_text(''.join(f'#include <{namespace}/{header}.h>\n'
            for header in ('mbstate', 'utf8') for namespace in ('libc', 'liteos')) +
            f'{assertion}(sizeof(mbstate_t) == 12, "multibyte state ABI");\n')
        subprocess.run([compiler, '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                        '-I', str(ROOT / 'user/libc/include'),
                        '-I', str(ROOT / 'include'), str(mixed)], check=True)
print('Libc namespace: unchanged layouts, C/C++ fd bits, single evaluation, invalid/null guards and sanitized execution passed')
print('Libc headers: all three compatibility forwards preprocess identically; mixed C/C++ Unicode includes preserve the state ABI')
