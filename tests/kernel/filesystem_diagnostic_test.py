"""Exercise production FAT/cache diagnostic formatting without privileged UART I/O."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def diagnostics(path, first, last, sink):
    source = (ROOT / path).read_text()
    assert 'LITEOS' not in source, path
    source = source[source.index(first):source.index(last)]
    # 只替换硬件输出端，保留生产环境的格式化器和锁。
    start = source.index('static ', source.index('ATOMIC_FLAG_INIT'))
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    assert sink in source[start:opening]
    return source[:start] + f'static void {sink}(char c) {{ capture(c); }}\n' + source[end:]


code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/block_cache.h>
#ifdef OS_KERNEL_BUILD
#include <kernel/process.h>
#include <kernel/sched.h>
static thread_t *current;
thread_t *sched_current_thread(void) { return current; }
#endif
static char output[1024];
static size_t used;
static void capture(char c) { assert(used + 1 < sizeof(output)); output[used++] = c; }
static void reset(void) { memset(output, 0, sizeof(output)); used = 0; }
''' + diagnostics('kernel/fs/nativefs/fat_table.c',
                  'static atomic_flag', 'BOOLEAN fat32_cluster_valid', 'fat32_diag_putc')
code += diagnostics('kernel/fs/pagecache/cache.c',
                    'static atomic_flag', 'static VOID copy_bytes', 'block_cache_diag_putc')
code += r'''
int main(void) {
    const char *tags[] = {"FAT_SECTOR_RANGE_FAIL", "FAT_SECTOR_IO_FAIL", "FAT_READ_ARGS_FAIL",
        "FAT_LOCATE_FAIL", "FAT_DATA_RANGE_FAIL", "FAT_DATA_SECTOR_FAIL"};
    for (unsigned i = 0; i < sizeof(tags)/sizeof(tags[0]); ++i) {
        char expected[512]; reset();
        fat32_diag_failure(tags[i], 0, UINT64_MAX, UINT64_C(0x8000000000000000));
        snprintf(expected, sizeof(expected),
            "OS_%s a=0x0000000000000000 b=0xffffffffffffffff c=0x8000000000000000\r\n", tags[i]);
        assert(!strcmp(output, expected));
    }
    reset(); block_cache_diag(UINT32_MAX, UINT64_MAX, NULL);
    assert(!strcmp(output, "OS_BLOCK_CACHE_READ_FAIL stage=0x00000000ffffffff lba=0xffffffffffffffff pid=0x0000000000000000 tid=0x0000000000000000 owner_tid=0x0000000000000000\r\n"));
#ifdef OS_KERNEL_BUILD
    process_t process = {.pid=UINT64_MAX};
    thread_t caller = {.tid=0x123456789abcdef0, .process=&process};
    thread_t owner = {.tid=0xfedcba9876543210};
    OS_BLOCK_CACHE cache = {0}; cache.WaitLock.owner = &owner; current = &caller;
    reset(); block_cache_diag(2, 0, &cache);
    assert(!strcmp(output, "OS_BLOCK_CACHE_READ_FAIL stage=0x0000000000000002 lba=0x0000000000000000 pid=0xffffffffffffffff tid=0x123456789abcdef0 owner_tid=0xfedcba9876543210\r\n"));
    current = &owner; reset(); block_cache_diag(0, 0, NULL);
    assert(strstr(output, "pid=0x0000000000000000 tid=0xfedcba9876543210 owner_tid=0x0000000000000000\r\n"));
#endif
    puts("FILESYSTEM_DIAGNOSTIC_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='filesystem-diagnostics-') as temporary:
    fixture = Path(temporary) / 'test.c'
    binary = Path(temporary) / 'test'
    fixture.write_text(code)
    for flags in ([], ['-DOS_KERNEL_BUILD']):
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I', str(ROOT / 'include'), *flags, str(fixture), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
