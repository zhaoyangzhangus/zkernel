"""实际扩展/截断分支：目录项失败不得提前发布长度，同长度截断不产生 I/O。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/nativefs/file_lifecycle.c').read_text()
start = source.index('static BOOLEAN extend_file_to(')
body = source[start:source.index('static UINT64 fat32_contiguous_extent_bytes(', start)]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/fat32.h>
#define FAT32_EOC_MIN 0x0FFFFFF8U
static unsigned counts,commits,locates,links;
static bool fail_commit;
static uint64_t disk_size;
static BOOLEAN count_file_clusters(const OS_FAT32 *fs,UINT32 first,UINT64 *count,UINT32 *last) {
    assert(fs && first==2);++counts;*count=2;*last=3;return 1;
}
static BOOLEAN zero_file_range(OS_FAT32_FILE *file, UINT32 first, UINT64 begin, UINT64 end) {
    (void)file;(void)first;(void)begin;(void)end;return 1;
}
static BOOLEAN update_file_directory_entry(OS_FAT32_FILE *file,UINT32 first,UINT64 size) {
    assert(file && first==2);++commits;if(fail_commit)return 0;disk_size=size;return 1;
}
static BOOLEAN commit_truncate_size(OS_FAT32_FILE *file,UINT64 size) {
    return update_file_directory_entry(file,file->FirstCluster,size);
}
static BOOLEAN fat32_find_free_cluster_from(OS_FAT32 *fs,UINT32 first,UINT32 *out) {
    (void)fs;(void)first;(void)out;assert(false);return 0;
}
static BOOLEAN fat32_write_file_fat_value(OS_FAT32 *fs,UINT32 cluster,UINT32 value,UINT32 extent) {
    (void)fs;(void)cluster;(void)value;(void)extent;assert(false);return 0;
}
static BOOLEAN fat32_free_cluster_chain(OS_FAT32 *fs,UINT32 cluster) {
    (void)fs;(void)cluster;assert(false);return 0;
}
static BOOLEAN locate_file_cluster(OS_FAT32_FILE *file,UINT32 logical,UINT32 *physical) {
    assert(file && logical==1);++locates;file->CursorLogicalStart=17;*physical=3;return 1;
}
static BOOLEAN fat32_read_next_cluster(const OS_FAT32 *fs,UINT32 cluster,UINT32 *next) {
    assert(fs && cluster==3);++links;*next=0;return 1;
}
static BOOLEAN fat32_truncate_clusters(OS_FAT32_FILE *file,UINT32 cluster,UINT64 size) {
    (void)file;(void)cluster;(void)size;assert(false);return 0;
}
''' + body + r'''
int main(void) {
    OS_FAT32 fs={.ClusterCount=128};
    OS_FAT32_FILE invalid={0};
    assert(!extend_file_to(0,0,0) && !truncate_file_to(0,0));
    assert(!extend_file_to(&invalid,0,0) && !truncate_file_to(&invalid,0));
    assert(!counts && !commits);
    for(unsigned sector=512;sector<=4096;sector*=2) {
        for(unsigned cluster=1;cluster<=8;cluster*=2) {
            fs.BytesPerSector=sector;fs.SectorsPerCluster=cluster;
            uint64_t bytes=(uint64_t)sector*cluster;
            for(unsigned shrink=0;shrink<2;++shrink) {
                OS_FAT32_FILE file;memset(&file,0xa5,sizeof(file));
                file.FileSystem=&fs;file.FirstCluster=2;file.Size=bytes+100;
                file.CursorValid=1;file.Used=1;
                OS_FAT32_FILE before=file;
                uint64_t target=shrink?bytes+50:bytes+150;
                counts=commits=locates=links=0;disk_size=file.Size;fail_commit=true;
                BOOLEAN ok=shrink?truncate_file_to(&file,target):extend_file_to(&file,target,target);
                assert(!ok && counts==!shrink && commits==1);
                assert(locates==shrink && links==shrink);
                assert(!memcmp(&file,&before,sizeof(file)) && disk_size==before.Size);
                fail_commit=false;
                ok=shrink?truncate_file_to(&file,target):extend_file_to(&file,target,target);
                assert(ok && counts==2U*!shrink && commits==2 && file.Size==target && disk_size==target);
                assert(locates==2U*shrink && links==2U*shrink);
                assert(!file.CursorValid && file.FirstCluster==2);
                /* 相同长度请求保留整个游标及索引状态，并且不查询簇链或目录项。 */
                file.CursorValid=1;before=file;counts=commits=0;
                for(unsigned repeat=0;repeat<1000;++repeat)assert(truncate_file_to(&file,target));
                assert(!counts && !commits && !memcmp(&file,&before,sizeof(file)));
                assert(!truncate_file_to(&file,(uint64_t)UINT32_MAX+1));
                assert(!counts && !commits && !memcmp(&file,&before,sizeof(file)));
            }
        }
    }
    puts("FAT resize: 32 growth/shrink geometry cases preserve the full handle on failed commit and retry; shrinking performs no full-chain count; 32000 same-size truncates perform no metadata work");
}
'''
with tempfile.TemporaryDirectory(prefix='fat-resize-commit-') as directory:
    path = Path(directory)
    variants = {'current':program}
    for name, first in (('early_growth','old_first'),('early_shrink','file->FirstCluster')):
        commit = (f'if (!update_file_directory_entry(file, {first}, required_size)) return 0;'
                  if name == 'early_growth' else
                  'if (!commit_truncate_size(file, required_size)) return 0;')
        variants[name] = program.replace(commit,
            'file->Size = required_size; file->CursorValid = 0; ' + commit, 1)
    variants['same_size_scan'] = program.replace('if (required_size == file->Size) return 1;', '')
    for name, code in variants.items():
        (path / 'test.c').write_text(code)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
        result = subprocess.run([str(path/'test')],capture_output=True,text=True)
        if name == 'current':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0, name
            print('负对照拒绝：' + name)
