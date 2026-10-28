"""Production extension: monotonic transaction-local search and rollback."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/nativefs/file_lifecycle.c').read_text()
start=source.index('static BOOLEAN extend_file_to(')
code=source[start:source.index('static BOOLEAN truncate_file_to(',start)]
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int BOOLEAN;
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
#define FAT32_EOC_MIN 0x0ffffff8U
typedef struct {UINT32 BytesPerSector,SectorsPerCluster,ClusterCount;} OS_FAT32;
typedef struct {OS_FAT32 *FileSystem;UINT64 Size;UINT32 FirstCluster;BOOLEAN CursorValid;} OS_FAT32_FILE;
static UINT32 table[8194],baseline[8194];
static unsigned visits,writes,fail_write;
static BOOLEAN fail_directory,fail_zero;
static BOOLEAN count_file_clusters(const OS_FAT32 *f,UINT32 c,UINT64 *n,UINT32 *last) {
    *n=0;*last=0;
    while(c) {
        assert(c>=2 && c<=f->ClusterCount+1 && ++*n<=f->ClusterCount);
        *last=c;c=table[c]>=FAT32_EOC_MIN?0:table[c];
    }
    return 1;
}
static BOOLEAN fat32_find_free_cluster_from(OS_FAT32 *f,UINT32 first,UINT32 *out) {
    for(UINT32 c=first;c<=f->ClusterCount+1;++c) {
        ++visits;if(!table[c]) {*out=c;return 1;}
    }
    return 0;
}
static BOOLEAN fat32_find_free_cluster(OS_FAT32 *f,UINT32 *out) {
    return fat32_find_free_cluster_from(f,2,out);
}
static BOOLEAN fat32_write_file_fat_value(OS_FAT32 *f,UINT32 c,UINT32 value,UINT32 extent) {
    (void)f;(void)extent;
    if(++writes==fail_write) return 0;
    table[c]=value;return 1;
}
static BOOLEAN fat32_free_cluster_chain(OS_FAT32 *f,UINT32 c) {
    unsigned hops=0;
    while(c) {
        assert(c>=2 && c<=f->ClusterCount+1 && ++hops<=f->ClusterCount);
        UINT32 next=table[c]>=FAT32_EOC_MIN?0:table[c];table[c]=0;c=next;
    }
    return 1;
}
static BOOLEAN zero_file_range(OS_FAT32_FILE *file, UINT32 first, UINT64 begin, UINT64 end) {
    (void)file;(void)first;(void)begin;(void)end;return !fail_zero;
}
static BOOLEAN update_file_directory_entry(OS_FAT32_FILE *f,UINT32 first,UINT64 size) {
    (void)f;(void)first;(void)size;return !fail_directory;
}
''' + code + r'''
static void reset(OS_FAT32_FILE *file,BOOLEAN existing) {
    memset(table,0,sizeof(table));
    for(unsigned c=2;c<2002;++c) table[c]=FAT32_EOC_MIN;
    if(existing) table[2]=3;
    file->FirstCluster=existing?2:0;file->Size=existing?1024:0;file->CursorValid=1;
    memcpy(baseline,table,sizeof(table));visits=writes=fail_write=0;fail_directory=fail_zero=0;
}
int main(void) {
    OS_FAT32 fs={512,1,8192};OS_FAT32_FILE file={.FileSystem=&fs};
    /* Keep the legacy helper exercised even after production adopts 'from'. */
    reset(&file,0);UINT32 first;assert(fat32_find_free_cluster(&fs,&first) && first==2002);
    for(unsigned existing=0;existing<=1;++existing) {
        reset(&file,existing);
        UINT64 target=file.Size+1024*512ULL;
        assert(extend_file_to(&file,target,target) && file.Size==target && !file.CursorValid);
        UINT64 count;UINT32 last;assert(count_file_clusters(&fs,file.FirstCluster,&count,&last));
        assert(count==1024+existing*2 && last==3025);
        printf("1024-cluster extension: scanned %u entries\n",visits);
        assert(visits==3024);
        for(unsigned c=4;c<2002;++c) assert(table[c]==baseline[c]);
        /* New allocation, new-chain link, old-tail attach and directory failures. */
        unsigned failures[]={1,2,3,2047,2048};
        for(unsigned i=0;i<sizeof(failures)/sizeof(*failures);++i) {
            if(!existing && failures[i]==2048) continue;
            reset(&file,existing);fail_write=failures[i];
            assert(!extend_file_to(&file,file.Size+1024*512ULL,file.Size+1024*512ULL));
            assert(!memcmp(table,baseline,sizeof(table)));
            assert(file.FirstCluster==(existing?2U:0U) && file.Size==(existing?1024U:0U));
            assert(file.CursorValid);
        }
        /* 清空新区域失败时，刚分配和连接的簇也必须回滚。 */
        reset(&file,existing);fail_zero=1;
        assert(!extend_file_to(&file,file.Size+1024*512ULL,file.Size+1024*512ULL));
        assert(!memcmp(table,baseline,sizeof(table)) && file.CursorValid);
        assert(file.FirstCluster==(existing?2U:0U) && file.Size==(existing?1024U:0U));
        reset(&file,existing);fail_directory=1;
        assert(!extend_file_to(&file,file.Size+1024*512ULL,file.Size+1024*512ULL));
        assert(!memcmp(table,baseline,sizeof(table)) && file.CursorValid);
        fail_directory=0;visits=0;
        assert(extend_file_to(&file,file.Size+1024*512ULL,file.Size+1024*512ULL) && visits==3024);
        visits=0;assert(extend_file_to(&file,file.Size,file.Size) && !visits);
        /* Exhaustion after a partial chain must roll back and retry from 2. */
        reset(&file,existing);
        for(unsigned c=2002;c<8194;++c) table[c]=FAT32_EOC_MIN;
        table[8190]=table[8192]=0;memcpy(baseline,table,sizeof(table));
        assert(!extend_file_to(&file,file.Size+3*512ULL,file.Size+3*512ULL));
        assert(!memcmp(table,baseline,sizeof(table)));
        assert(extend_file_to(&file,file.Size+2*512ULL,file.Size+2*512ULL));
        assert(table[8190]==8192 && table[8192]==FAT32_EOC_MIN);
        reset(&file,existing);
        for(unsigned c=2002;c<8194;++c) table[c]=FAT32_EOC_MIN;
        memcpy(baseline,table,sizeof(table));
        assert(!extend_file_to(&file,file.Size+512,file.Size+512));
        assert(!memcmp(table,baseline,sizeof(table)));
    }
    puts("FAT extension: linear search, empty/existing chains, table/link/directory rollback and full volume passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fat-extend-') as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)
