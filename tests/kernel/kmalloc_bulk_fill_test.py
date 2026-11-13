"""Test allocator fill/check helpers against the actual non-SIMD string object."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/mm/kmalloc.c').read_text()
start = source.index('static void bytes_fill(')
end = source.index('\nstatic void cache_lock(', start)
helpers = source[start:end]
constants = '\n'.join(line for line in source.splitlines()
                      if line.startswith('#define KMEM_POISON_') or
                      line.startswith('#define KMEM_REDZONE_BYTE'))
program = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
void *liteos_memset(void *, int, size_t);
#define __builtin_memset liteos_memset
''' + constants + '\n' + helpers + r'''
#if !defined(__SANITIZE_ADDRESS__)
static double seconds(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + now.tv_nsec / 1e9;
}
#endif
__attribute__((noinline)) static void old_fill(void *p, uint8_t value, size_t size) {
    uint8_t *bytes = p;
    while (size-- != 0) *bytes++ = value;
}
int main(void) {
    /* 每个字节（包括每个字边界和尾部）在所有可能的对齐位置上，都必须能检测出空闲
     * 槽毒值已损坏。 */
    uint8_t check[2064];
    for (unsigned offset=0;offset<8;++offset) {
        for (unsigned length=0;length<=2048;++length) {
            old_fill(check, KMEM_POISON_FREE, sizeof(check));
            assert(bytes_equal(check+offset, KMEM_POISON_FREE, length));
            for (unsigned byte=0;byte<length;++byte) {
                check[offset+byte]^=1;
                assert(!bytes_equal(check+offset, KMEM_POISON_FREE, length));
                check[offset+byte]^=1;
            }
        }
    }
    /* 缓冲区末端恰好贴着保护页：任何字读取都不得越过其长度。 */
    long page=sysconf(_SC_PAGESIZE);assert(page>2048);
    uint8_t *guard=mmap(0,2*(size_t)page,PROT_READ|PROT_WRITE,
                        MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(guard!=MAP_FAILED);
    assert(!mprotect(guard+page,(size_t)page,PROT_NONE));
    old_fill(guard,KMEM_POISON_FREE,(size_t)page);
    for(unsigned length=0;length<=2048;++length) {
        assert(bytes_equal(guard+page-length,KMEM_POISON_FREE,length));
        if(length) {
            guard[page-1]^=1;
            assert(!bytes_equal(guard+page-length,KMEM_POISON_FREE,length));
            guard[page-1]^=1;
        }
    }
    assert(!munmap(guard,2*(size_t)page));
    const size_t sizes[] = {0,1,7,8,31,63,64,65,127,128,2048,4096,32768,8*1024*1024};
    const uint8_t values[] = {0,KMEM_POISON_FREE,KMEM_POISON_ALLOC,KMEM_REDZONE_BYTE};
    for (unsigned n = 0; n < sizeof(sizes)/sizeof(sizes[0]); ++n) {
        size_t size = sizes[n];
        uint8_t *memory = malloc(size + 48);
        assert(memory);
        for (unsigned offset = 0; offset < 8; ++offset) {
            size_t front = 16 + offset, back = size + 48 - front - size;
            for (unsigned v = 0; v < 4; ++v) {
                old_fill(memory, 0x69, size + 48);
                uint8_t *payload = memory + front;
                bytes_fill(payload, values[v], size);
                assert(bytes_equal(payload, values[v], size));
                assert(bytes_equal(memory, 0x69, front));
                assert(bytes_equal(payload + size, 0x69, back));
                if (size) {
                    payload[0] ^= 1;
                    assert(!bytes_equal(payload, values[v], size));
                    payload[0] ^= 1;
                    payload[size-1] ^= 1;
                    assert(!bytes_equal(payload, values[v], size));
                }
            }
        }
        free(memory);
    }
#if !defined(__SANITIZE_ADDRESS__)
    /* 仅对参考 C 循环进行比较，以排除 sanitizer 开销。 */
    size_t size = 32 * 1024 * 1024;
    uint8_t *memory = malloc(size);
    assert(memory);
    old_fill(memory, 0, size); /* Fault pages before timing either implementation. */
    double begin = seconds();
    old_fill(memory, KMEM_POISON_ALLOC, size);
    double old_seconds = seconds() - begin;
    begin = seconds();
    bytes_fill(memory, KMEM_POISON_FREE, size);
    double new_seconds = seconds() - begin;
    assert(bytes_equal(memory, KMEM_POISON_FREE, size));
    printf("allocator fill 32MiB: scalar_ms=%.3f bulk_ms=%.3f speedup=%.2f\n",
           old_seconds*1000, new_seconds*1000, old_seconds/new_seconds);
    free(memory);
#endif
    puts("Allocator fill/check: zeroing, poison, redzones, all-byte corruption, alignment and exact guard-page tails passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-kmalloc-fill-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                    '-mno-sse', '-mno-mmx', '-DOS_KERNEL_BUILD',
                    '-I'+str(root/'include'), '-Dmemset=liteos_memset',
                    '-Dmemcpy=liteos_memcpy', '-Dmemmove=liteos_memmove',
                    '-Dmemcmp=liteos_memcmp', '-c', str(root/'kernel/string.c'),
                    '-o', str(path/'string.o')], check=True)
    for extra in (['-fsanitize=address,undefined'], []):
        subprocess.run(['cc', '-D_DEFAULT_SOURCE', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                        '-fno-tree-vectorize', *extra, '-fno-pie', '-no-pie',
                        str(path/'test.c'), str(path/'string.o'),
                        '-o', str(path/'test')], check=True)
        subprocess.run([str(path/'test')], check=True)
