"""Production retained-chain traversal has linear I/O and no heap snapshot."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/fs/nativefs/transaction.c').read_text()
helper=source[source.index('static BOOLEAN fat_chain_tail('):source.index('static VOID fat_snapshot_restore(')]
assert 'alloc' not in helper and 'snapshot' not in helper
helper=source[source.index('static BOOLEAN fat_chain_cycle_step('):source.index('static BOOLEAN fat_snapshot_chain(')]+helper
replacement=source[source.index('BOOLEAN fat32_replace_file_entry('):source.index('void fat32_rollback_directory_extensions(')]
assert replacement.index('fat_chain_tail(filesystem, fat32_entry_cluster(target)') < replacement.index('fat_snapshot_chain(')
assert replacement.index('retired_tail == retained_tail') < replacement.index('fat_snapshot_chain(')
program=r'''
#include <assert.h>
#include <stdint.h>
typedef uint32_t UINT32;
typedef int BOOLEAN;
typedef struct {UINT32 ClusterCount;} OS_FAT32;
static UINT32 reads, fail_cluster, cycle, cycle_start, cycle_length;
static UINT32 arbitrary, links[6];
static BOOLEAN fat32_cluster_valid(const OS_FAT32 *fs,UINT32 c){return c>=2&&c<fs->ClusterCount+2;}
static BOOLEAN fat32_read_next_cluster(const OS_FAT32 *fs,UINT32 c,UINT32 *next){
    assert(fat32_cluster_valid(fs,c));++reads;
    if(c==fail_cluster)return 0;
    if(arbitrary){*next=links[c-2];return 1;}
    if(cycle_length){*next=c==cycle_start+cycle_length-1?cycle_start:c+1;return 1;}
    *next=c==fs->ClusterCount+1?(cycle?2:0):c+1;return 1;
}
''' + helper + r'''
int main(void){
    const UINT32 sizes[]={1,2,16,1000,100000};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);++i){
        OS_FAT32 fs={sizes[i]};UINT32 tail=99;
        reads=0;assert(fat_chain_tail(&fs,0,&tail)&&!tail&&!reads);
        assert(fat_chain_tail(&fs,2,&tail)&&tail==fs.ClusterCount+1&&reads==fs.ClusterCount);
        reads=0;assert(fat_chain_tail(&fs,fs.ClusterCount+1,&tail)&&tail==fs.ClusterCount+1&&reads==1);
        reads=0;assert(!fat_chain_tail(&fs,1,&tail)&&!tail&&!reads);
        assert(!fat_chain_tail(&fs,UINT32_MAX,&tail)&&!tail&&!reads);
        fail_cluster=2;assert(!fat_chain_tail(&fs,2,&tail)&&!tail&&reads==1);fail_cluster=0;
        cycle=1;reads=0;assert(!fat_chain_tail(&fs,2,&tail)&&!tail&&reads==fs.ClusterCount);cycle=0;
    }
    OS_FAT32 huge={1000000};UINT32 tail;
    for(cycle_start=2;cycle_start<40;++cycle_start) {
        for(cycle_length=1;cycle_length<40;++cycle_length) {
            reads=0;
            assert(!fat_chain_tail(&huge,2,&tail)&&!tail);
            assert(reads<=4*(cycle_start+cycle_length));
            if(cycle_start==2&&cycle_length==1)assert(reads==1);
        }
    }
    cycle_length=0;
    /* 对照独立的已访问集合基准，穷举所有六节点 FAT 图。生产实现不使用已访问集合，
     * 但从每个起点得到的结果都必须一致。 */
    OS_FAT32 small={6};arbitrary=1;
    for(UINT32 encoded=0;encoded<117649;++encoded){
        UINT32 digits=encoded;
        for(unsigned i=0;i<6;++i){UINT32 value=digits%7;digits/=7;links[i]=value?value+1:0;}
        for(UINT32 first=2;first<8;++first){
            UINT32 seen=0,current=first,expected=0;BOOLEAN valid=0;
            while(!(seen&(1U<<(current-2)))){
                seen|=1U<<(current-2);
                if(!links[current-2]){valid=1;expected=current;break;}
                current=links[current-2];
            }
            reads=0;assert(fat_chain_tail(&small,first,&tail)==valid);
            assert(tail==expected&&reads<=small.ClusterCount);
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix='fat-chain-tail-') as directory:
    p=Path(directory);(p/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('FAT chain validation: 117649 graphs against visited-set oracle, linear reads, pre-allocation guards and 1482 early short-cycle rejections passed')
