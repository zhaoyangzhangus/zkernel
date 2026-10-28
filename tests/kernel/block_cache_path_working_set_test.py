"""实际块缓存：多级目录与流式数据交错时的字节一致性和 I/O 次数。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/pagecache/cache.c').read_text()
policy = ('cache->Clock - candidate->LastUse <\n'
          '                    OS_BLOCK_CACHE_ENTRY_COUNT * 5U / 8U')
assert policy in source
program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/block_cache.h>
static unsigned reads;
static unsigned char value(UINT64 lba, unsigned byte) {
    return (unsigned char)(lba * 31 + byte * 17 + (lba >> 8));
}
static BOOLEAN read_disk(VOID *ctx, UINT64 lba, UINT32 count, VOID *out) {
    (void)ctx; ++reads;
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = 0; j < 512; ++j)
            ((UINT8 *)out)[i * 512 + j] = value(lba + i, j);
    return 1;
}
static BOOLEAN write_disk(VOID *ctx, UINT64 lba, UINT32 count, const VOID *in) {
    (void)ctx; (void)lba; (void)count; (void)in;
    assert(0); return 0;
}
static void read_checked(OS_BLOCK_CACHE *cache, UINT64 lba) {
    UINT8 out[544]; memset(out, 0xa5, sizeof(out));
    assert(block_cache_read(cache, lba, out + 16));
    for (unsigned i = 0; i < 512; ++i) assert(out[16 + i] == value(lba, i));
    for (unsigned i = 0; i < 16; ++i)
        assert(out[i] == 0xa5 && out[528 + i] == 0xa5);
}
int main(int argc, char **argv) {
    assert(argc == 2); bool scan = !strcmp(argv[1], "scan");
    unsigned depth = scan ? 6U : (unsigned)atoi(argv[1]);
    OS_BLOCK_DEVICE disk = {.Registered=1, .BlockSize=512, .BlockCount=16384,
                            .Read=read_disk, .Write=write_disk};
    OS_BLOCK_CACHE cache = {0}; assert(block_cache_init(&cache, &disk));
    for (unsigned round = 0; round < 128; ++round) {
        for (unsigned level = 0; level < depth; ++level)
            read_checked(&cache, level * 64);
        if (scan) {
            for (unsigned sector = 0; sector < 16; ++sector) {
                if (sector == 8) read_checked(&cache, 1024);
                read_checked(&cache, sector < 8 ? 4096 + sector : 8192 + sector - 8);
            }
        } else read_checked(&cache, 4096 + round * 32);
    }
    assert(block_cache_destroy(&cache)); printf("%u\n", reads);
}
'''
with tempfile.TemporaryDirectory(prefix='block-cache-path-') as directory:
    directory = Path(directory)
    counts = {}
    scans = {}
    for variant in ('previous', 'current', 'scan_previous', 'half_cache'):
        cache = directory / (variant + '.c')
        candidate = source
        if variant in ('previous', 'scan_previous', 'half_cache'):
            window = '4U' if variant == 'previous' else 'OS_BLOCK_CACHE_ENTRY_COUNT / 2U'
            old_policy = (f'cache->Clock - candidate->LastUse < {window} &&\n'
                          f'                !(candidate->Lba < lba && '
                          f'lba - candidate->Lba <= {window})')
            candidate = candidate.replace(policy, old_policy)
        if variant == 'scan_previous':
            begin = candidate.index('    /* 连续消费预读扇区')
            end = candidate.index('    *result = entry;', begin)
            candidate = (candidate[:begin] +
                         '    entry->LastUse = ++cache->Clock;\n' + candidate[end:])
            candidate = candidate.replace('                reserved_mask |= 1U << i;\n', '')
        cache.write_text(candidate)
        binary = directory / variant
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I' + str(root / 'include'), '-x', 'c', '-',
                        str(root / 'kernel/block/bio.c'), str(cache), '-o', str(binary)],
                       input=program, text=True, check=True)
        if variant in ('previous', 'current'):
            counts[variant] = [int(subprocess.check_output([str(binary), str(depth)],
                                 text=True, timeout=10)) for depth in range(1, 13)]
        scans[variant] = int(subprocess.check_output([str(binary), 'scan'],text=True,timeout=10))
    # 可驻留的目录工作集只需各读一次，之后每轮只读取新的流式数据。
    for depth in range(1, 8):
        assert counts['current'][depth - 1] == 128 + depth, counts
        assert counts['current'][depth - 1] <= counts['previous'][depth - 1], counts
    assert counts['previous'][5] > counts['current'][5] * 5, counts
    print('多级目录与流式数据交错：12 种深度字节及边界检查通过；六层目录读取次数',
          counts['previous'][5], '->', counts['current'][5])
    assert scans['current'] < scans['scan_previous'] / 2, scans
    assert scans['current'] < scans['half_cache'], scans
    print('六层父目录与分散的双簇目录扫描：', scans)
