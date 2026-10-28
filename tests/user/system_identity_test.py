"""Exercise both production uname implementations and the neutral boot paths."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
libc = (ROOT / 'user/libc/posix_compat.c').read_text()
kernel = (ROOT / 'kernel/syscall/linux.c').read_text()
syscall = (ROOT / 'user/libc/syscall.c').read_text()


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


start = kernel.index('typedef struct linux_utsname {')
end = kernel.index('} linux_utsname_t;', start) + len('} linux_utsname_t;')
code = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#define __user
#define K_OK 0
#define LINUX_EFAULT (-14)
static void linux_zero(void *p, size_t n) { memset(p, 0, n); }
static void linux_copy(void *p, const void *s, size_t n) { memcpy(p, s, n); }
static int copy_to_user(void *p, const void *s, size_t n) {
    if (!p) return -1;
    memcpy(p, s, n); return K_OK;
}
''' + kernel[start:end] + '\n'
code += function(libc, 'int uname(') + '\n'
code += function(syscall, 'int gethostname(') + '\n'
code += function(kernel, 'static int64_t linux_uname(') + r'''
int main(void) {
    struct utsname native;
    linux_utsname_t linux;
    memset(&native, 0xa5, sizeof(native));
    memset(&linux, 0xa5, sizeof(linux));
    assert(uname(&native) == 0);
    assert(linux_uname((uintptr_t)&linux) == K_OK);
    assert(strcmp(native.sysname, "System") == 0);
    assert(strcmp(native.nodename, "localhost") == 0);
    assert(strcmp(native.sysname, linux.sysname) == 0);
    assert(strcmp(native.nodename, linux.nodename) == 0);
    assert(strcmp(native.machine, linux.machine) == 0);
    assert(strcmp(native.release, linux.release) == 0);
    assert(strcmp(native.version, "System Chromium") == 0);
    assert(strcmp(linux.version, "System Linux ABI") == 0);
    assert(linux.domainname[0] == 0);
    char hostname[32];
    assert(gethostname(hostname, sizeof(hostname)) == 0);
    assert(strcmp(hostname, native.nodename) == 0);
    for (size_t size = 1; size < sizeof("localhost"); ++size) {
        memset(hostname, 0xa5, sizeof(hostname));
        errno = 0;
        assert(gethostname(hostname, size) == -1 && errno == ENAMETOOLONG);
        assert(memcmp(hostname, "localhost", size) == 0);
        assert((unsigned char)hostname[size] == 0xa5);
    }
    assert(gethostname(NULL, 32) == -1 && errno == EINVAL);
    assert(gethostname(hostname, 0) == -1 && errno == EINVAL);
    errno = 0;
    assert(uname(NULL) == -1 && errno == EFAULT);
    assert(linux_uname(0) == LINUX_EFAULT);
    puts("System identity: native/Linux uname, null guards and neutral boot/font paths passed");
}
'''
loader = (ROOT / 'boot/uefi/main.c').read_text()
config = re.search(r'static CHAR16 config_path\[\] = \{([^}]+)\}', loader).group(1)
parts = re.findall(r"'((?:\\.|[^'])*)'", config)
config_path = ''.join(bytes(part, 'ascii').decode('unicode_escape') for part in parts)
assert config_path == '\\EFI\\OS\\loader.conf', config_path
assert '"\\\\EFI\\\\OS\\\\kernel.elf"' in loader
make = (ROOT / 'makefile').read_text()
assert 'KERNEL_ELF = $(BUILD)/esp/EFI/OS/kernel.elf' in make
assert "'kernel=\\EFI\\OS\\kernel.elf'" in make
assert 'assets/fonts/default.ttf' in make
assert '$(DESKTOP_ASSETS)' in make
assert 'CHROME_COMPAT_FONT_ASSET' not in make
assert 'liteos.ttf' not in make
assert (ROOT / 'assets/fonts/default.ttf').is_file()
assert (ROOT / 'assets/desktop/wallpaper.png').is_file()
for file in ('kernel/graphics/ascii_font.c', 'user/runtime/text.c',
             'user/runtime/blend2d_api_test.c', 'user/runtime/blend2d_demo.c'):
    assert any(value in (ROOT / file).read_text() for value in
               ('"/EFI/OS/fonts/default.ttf"', 'OS_FONT_DEFAULT_PATH')), file
with tempfile.TemporaryDirectory(prefix='system-identity-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
