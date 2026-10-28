"""Actual snapshot walker bounds short cycles before volume-sized allocation."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/fs/nativefs/transaction.c').read_text()
types=source[source.index('typedef struct fat_chain_snapshot'):source.index('static VOID *fat_transaction_alloc(')]
walkers=source[source.index('static BOOLEAN fat_chain_cycle_step('):source.index('static BOOLEAN fat_chain_tail(')]
program=r'''
#include <assert.h>
#include <stdint.h>
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int BOOLEAN;
typedef struct {UINT32 ClusterCount,FatCount;} OS_FAT32;
''' + types + r'''
static UINT32 clusters[128],values[256],cycle_start,cycle_length,reads,reserves;
static BOOLEAN fat32_cluster_valid(const OS_FAT32 *fs,UINT32 c){return c>=2&&c<fs->ClusterCount+2;}
static BOOLEAN fat_snapshot_reserve(FAT_CHAIN_SNAPSHOT *s){
    ++reserves;assert(s->Count<128);s->Clusters=clusters;s->Values=values;
    s->Capacity=128;return 1;
}
static BOOLEAN fat32_read_fat_entry(const OS_FAT32 *fs,UINT32 copy,UINT32 c,UINT32 *value){
    assert(fat32_cluster_valid(fs,c)&&copy>=1&&copy<=fs->FatCount);
    ++reads;
    if(!cycle_length)*value=c==fs->ClusterCount+1?0x0FFFFFFF:c+1;
    else *value=c==cycle_start+cycle_length-1?cycle_start:c+1;
    return 1;
}
static BOOLEAN fat32_read_next_cluster(const OS_FAT32 *fs,UINT32 c,UINT32 *next){
    assert(fat32_cluster_valid(fs,c));++reads;
    if(!cycle_length){*next=c==fs->ClusterCount+1?0:c+1;return 1;}
    *next=c==cycle_start+cycle_length-1?cycle_start:c+1;return 1;
}
''' + walkers + r'''
int main(void){
    OS_FAT32 fs={1000000,2};
    for(cycle_start=2;cycle_start<40;++cycle_start)
        for(cycle_length=1;cycle_length<40;++cycle_length){
            FAT_CHAIN_SNAPSHOT s={0};reads=reserves=0;
            assert(!fat_snapshot_chain(&fs,2,&s));
            assert(s.Count==reserves&&reads==reserves*3);
            assert(reserves<=4*(cycle_start+cycle_length));
            if(cycle_start==2&&cycle_length==1)assert(reserves==1);
        }
    cycle_length=0;
    for(UINT32 length=1;length<=128;++length){
        OS_FAT32 normal={length,2};FAT_CHAIN_SNAPSHOT s={0};reads=reserves=0;
        assert(fat_snapshot_chain(&normal,2,&s)&&s.Count==length);
        assert(reserves==length&&reads==length*3);
        for(UINT32 i=0;i<length;++i)assert(s.Clusters[i]==i+2);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='fat-snapshot-cycle-') as directory:
    p=Path(directory);(p/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('FAT snapshot walker: 1482 short cycles bound reserve attempts; normal chain order and read cost unchanged passed')
