"""验证物理页清零复用内核批量实现，覆盖页边界、保护页与非 SIMD 约束。"""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'kernel/mm/phys/internal.h').read_text()
start = header.index('static inline void page_memory_zero(')
helper = header[start:header.index('\nstatic inline uint64_t page_pool_irq_save(', start)]
program = r'''
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
void *os_test_memset(void *, int, size_t);
#define memset os_test_memset
''' + helper + r'''
__attribute__((noinline)) void zero_pages(void *p, size_t n) {
    page_memory_zero(p, n);
}
static uint64_t nanoseconds(void) {
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}
int main(int argc, char **argv) {
    (void)argv;
    size_t page = (size_t)sysconf(_SC_PAGESIZE), span = page * 256;
    uint8_t *mapping = mmap(NULL, span + page * 2, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    uint8_t *buffer = mapping + page;
    assert(!mprotect(buffer, span, PROT_READ | PROT_WRITE));
    const size_t lengths[] = {0,1,7,8,9,63,64,65,4095,4096,4097,65536};
    for (unsigned alignment = 0; alignment < 16; ++alignment) {
        for (unsigned i = 0; i < sizeof(lengths)/sizeof(lengths[0]); ++i) {
            size_t length = lengths[i];
            os_test_memset(buffer, 0xa5, span);
            zero_pages(buffer + alignment, length);
            for (size_t j = 0; j < span; ++j)
                assert(buffer[j] == (j >= alignment && j-alignment < length ? 0 : 0xa5));
            os_test_memset(buffer, 0xa5, span);
            zero_pages(buffer + span - length, length);
            for (size_t j = 0; j < span; ++j)
                assert(buffer[j] == (j >= span-length ? 0 : 0xa5));
        }
    }
    zero_pages(buffer, span);
    for (size_t i = 0; i < span; ++i) assert(!buffer[i]);
    if (argc > 1) {
        const size_t sizes[] = {4096,65536,1048576};
        for (unsigned i = 0; i < 3; ++i) {
            uint64_t start = nanoseconds();
            for (unsigned n = 0; n < 2000; ++n) zero_pages(buffer, sizes[i]);
            printf("%zu %llu ns\n", sizes[i], (unsigned long long)(nanoseconds()-start));
        }
    }
    assert(!munmap(mapping, span + page * 2));
}
'''
with tempfile.TemporaryDirectory(prefix='page-bulk-zero-') as directory:
    path = Path(directory)
    flags = ['-O2', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
             '-fno-builtin', '-mno-sse', '-mno-mmx', '-fno-tree-vectorize',
             '-DOS_KERNEL_BUILD', '-I' + str(root/'include')]
    subprocess.run(['cc', *flags, '-Dmemset=os_test_memset', '-c',
                    str(root/'kernel/string.c'), '-o', str(path/'string.o')], check=True)
    source = path/'test.c'
    source.write_text(program)
    subprocess.run(['cc', *flags, '-c', str(source), '-o', str(path/'test.o')], check=True)
    asm = subprocess.check_output(['objdump','-d', str(path/'string.o')], text=True)
    assert 'rep stos' in asm
    assert not any(register in asm for register in ('%xmm', '%ymm', '%zmm'))
    def zero_assembly(obj):
        assembly = subprocess.check_output(['objdump','-dr',str(obj)], text=True)
        body = assembly.split('<zero_pages>:',1)[1].split('\n\n',1)[0]
        return body
    assert 'os_test_memset' in zero_assembly(path/'test.o'), '清零必须复用内核实现，不能退回内联字节循环'
    subprocess.run(['cc',str(path/'test.o'),str(path/'string.o'),'-o',str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=20)
    print('物理页批量清零：零长度、16 种对齐、页尾保护、完整页块和非 SIMD 实现通过')
    # 旧实现虽然也能得到全零结果，但不得通过批量实现检查。
    old = program.replace('memset(address, 0, size);',
                          'uint8_t *p = address; while (size-- != 0) *p++ = 0;')
    assert old != program
    source.write_text(old)
    subprocess.run(['cc', *flags, '-c', str(source), '-o', str(path/'old.o')], check=True)
    assert 'os_test_memset' not in zero_assembly(path/'old.o')
    print('逐字节清零负面对照已被批量实现检查识别')
    if '--benchmark' in sys.argv:
        # 旧实现仅用于宿主对照；耗时不能替代客户机首播测量。
        subprocess.run(['cc',str(path/'old.o'),str(path/'string.o'),'-o',str(path/'old')], check=True)
        for name in ('old','test'):
            print(name,subprocess.check_output([str(path/name),'benchmark'],text=True,timeout=20))
