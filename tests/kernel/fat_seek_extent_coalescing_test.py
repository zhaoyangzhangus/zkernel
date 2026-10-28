"""验证大文件随机定位能复用已验证的连续簇链，同时拒绝合并碎片和空洞。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/nativefs/file_lifecycle.c').read_text()
begin = source.index('static BOOLEAN locate_file_cluster(')
body = source[begin:source.index('static BOOLEAN count_file_clusters(', begin)]
assignment = 'file->SeekIndexLength[slot] = (UINT32)end;'
assert body.count(assignment) == 1
reference = body.replace('locate_file_cluster(', 'reference_locate_file_cluster(', 1)
reference = reference.replace(assignment, '(void)end;')

program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/fat32.h>
static unsigned probes, links, fragmented;
static BOOLEAN fat32_cluster_valid(const OS_FAT32 *fs, UINT32 cluster) {
    return cluster >= 2 && cluster < fs->ClusterCount + 2;
}
static UINT32 fat32_extent_length(const OS_FAT32 *fs, UINT32 start, UINT32 maximum) {
    ++probes;
    assert(fat32_cluster_valid(fs, start));
    if (fragmented) return 1;
    UINT32 remaining = fs->ClusterCount + 2 - start;
    return maximum < remaining ? maximum : remaining;
}
static BOOLEAN fat32_read_next_cluster(const OS_FAT32 *fs, UINT32 current, UINT32 *next) {
    ++links;
    *next = current + (fragmented ? 2 : 1);
    if (!fat32_cluster_valid(fs, *next)) *next = 0;
    return 1;
}
''' + body + reference + r'''
int main(void) {
    OS_FAT32 fs = {.ClusterCount=65536, .BytesPerSector=512, .SectorsPerCluster=8};
    OS_FAT32_FILE actual = {.FileSystem=&fs, .FirstCluster=2, .Size=65536ULL*4096};
    OS_FAT32_FILE previous = actual;
    UINT32 a=0, b=0;
    assert(locate_file_cluster(&actual, 65535, &a));
    assert(reference_locate_file_cluster(&previous, 65535, &b));
    assert(a == b && a == 65537);
    assert(actual.SeekIndexValid == UINT32_MAX);
    for (unsigned i=0; i<32; ++i) {
        assert(actual.SeekIndexLogical[i] == i*2048);
        assert(actual.SeekIndexPhysical[i] == 2+i*2048);
        assert(actual.SeekIndexLength[i] == 2048);
    }
    unsigned coalesced_probes=0, previous_probes=0;
    UINT32 random=17;
    for (unsigned i=0; i<2000; ++i) {
        random=random*1664525U+1013904223U;
        unsigned target=random%65536;
        probes=links=0;
        assert(locate_file_cluster(&actual,target,&a));
        coalesced_probes+=probes;
        assert(links==0);
        probes=0;
        assert(reference_locate_file_cluster(&previous,target,&b));
        previous_probes+=probes;
        assert(a==b && a==2+target);
    }
    assert(coalesced_probes==0 && previous_probes>1000);
    printf("256MiB contiguous file, 2000 random seeks: FAT extent probes %u -> %u; physical results identical\n",
           previous_probes,coalesced_probes);

    /* 未验证的间隔不能合并；换成不连续物理地址后也不能沿用连续推断。 */
    for (unsigned mismatch=0; mismatch<2; ++mismatch) {
        actual=(OS_FAT32_FILE){.FileSystem=&fs,.FirstCluster=2,.Size=65536ULL*4096,
            .CursorValid=1,.CursorLogicalStart=99,.CursorPhysicalStart=101,.CursorLength=1,
            .SeekIndexValid=1};
        actual.SeekIndexLogical[0]=0;
        actual.SeekIndexPhysical[0]=mismatch ? 3 : 2;
        actual.SeekIndexLength[0]=mismatch ? 100 : 16;
        assert(locate_file_cluster(&actual,100,&a) && a==102);
        assert(actual.SeekIndexLength[0]==(mismatch?100U:16U));
    }
    fragmented=1;
    actual=(OS_FAT32_FILE){.FileSystem=&fs,.FirstCluster=2,.Size=1024ULL*4096};
    assert(locate_file_cluster(&actual,1023,&a) && a==2+2*1023);
    for (unsigned i=0;i<32;++i)assert(actual.SeekIndexLength[i]==1);
    for (unsigned i=0;i<1000;++i) {
        random=random*1664525U+1013904223U;
        unsigned target=random%1024;
        assert(locate_file_cluster(&actual,target,&a) && a==2+2*target);
    }
    actual.CursorValid=0;
    assert(locate_file_cluster(&actual,0,&a) && a==2);
    assert(actual.SeekIndexValid==1 && actual.SeekIndexLength[0]==1);
    puts("fragmentation, unvalidated gaps, physical mismatch and cursor invalidation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='fat-seek-coalescing-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root/'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
