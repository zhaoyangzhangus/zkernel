"""真实 FAT 簇数统计：连续段快照复用及独立图遍历对照。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
table = (root / 'kernel/fs/nativefs/fat_table.c').read_text()
files = (root / 'kernel/fs/nativefs/file_lifecycle.c').read_text()

def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2] + '\n'

count = function(files, 'static BOOLEAN count_file_clusters(')
reference = count.replace('count_file_clusters(', 'short_count_file_clusters(', 1)
reference = reference.replace('cluster, remaining)',
                              'cluster, remaining < 16U ? remaining : 16U)')
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/fat32.h>
#define FAT_TABLE_TYPE_16 16U
#define FAT_TABLE16_EOC_MIN 0xfff8U
#define FAT_TABLE16_BAD_CLUSTER 0xfff7U
#define FAT_TABLE_EOC_MIN 0x0ffffff8U
#define FAT_TABLE_BAD_CLUSTER 0x0ffffff7U
static UINT8 media[524288];
static unsigned reads;
static UINT64 fail_lba=UINT64_MAX;
static UINT16 fat32_read_u16(const UINT8 *p) {return (UINT16)p[0]|(UINT16)p[1]<<8;}
static UINT32 fat32_read_u32(const UINT8 *p) {return (UINT32)fat32_read_u16(p)|(UINT32)fat32_read_u16(p+2)<<16;}
static BOOLEAN fat32_read_sector(const OS_FAT32 *fs,UINT64 lba,UINT8 *out) {
    ++reads;if(lba==fail_lba)return 0;
    UINT64 offset=(lba-fs->FatStartLba)*fs->BytesPerSector;
    assert(lba>=fs->FatStartLba && offset+fs->BytesPerSector<=sizeof(media));
    memcpy(out,media+offset,fs->BytesPerSector);return 1;
}
static void store(const OS_FAT32 *fs,UINT32 cluster,UINT32 value) {
    unsigned width=fs->FatType/8;
    for(unsigned i=0;i<width;++i)media[(size_t)cluster*width+i]=(UINT8)(value>>(8*i));
}
'''
for signature in ('BOOLEAN fat32_cluster_valid(', 'BOOLEAN fat32_read_fat_entry(',
                  'BOOLEAN fat32_read_next_cluster(', 'UINT32 fat32_extent_length('):
    program += function(table, signature)
program += count + reference + r'''
int main(void) {
    for(unsigned bits=16;bits<=32;bits+=16) {
        for(unsigned bps=512;bps<=4096;bps*=2) {
            OS_FAT32 fs={.ClusterCount=4096,.FatType=bits,.FatCount=2,
                         .BytesPerSector=bps,.FatStartLba=32};
            for(UINT32 c=2;c<=4097;++c)store(&fs,c,c==4097?(bits==16?0xfff8U:0x0ffffff8U):c+1);
            UINT64 total=0,old_total=0;UINT32 tail=0,old_tail=0;
            reads=0;assert(short_count_file_clusters(&fs,2,&old_total,&old_tail));unsigned old=reads;
            reads=0;assert(count_file_clusters(&fs,2,&total,&tail));unsigned current=reads;
            assert(total==4096 && total==old_total && tail==4097 && tail==old_tail);
            assert(current<=((4098U*(bits/8)+bps-1)/bps)+1 && old>current*8);
            printf("FAT%u/%u contiguous chain: metadata snapshots %u -> %u\n",bits,bps,old,current);
            /* 扩展 FAT 的长度提示仍然受卷大小和剩余统计预算约束。 */
            fs.Fat3Available=1;fs.FatCount=3;
            fs.FatSectors=(4098U*(bits/8)+bps-1)/bps;
            unsigned hint=2*fs.FatSectors*bps+2*(bits/8);
            for(unsigned i=0;i<bits/8;++i)media[hint+i]=(UINT8)(4096U>>(8*i));
            reads=0;assert(count_file_clusters(&fs,2,&total,&tail) && total==4096 && tail==4097 && reads==2);
            store(&fs,4097,2);reads=0;
            assert(!count_file_clusters(&fs,2,&total,&tail) && reads==2);
            fs.Fat3Available=0;fs.FatCount=2;
            /* 连续整卷回环也只允许有界扫描，不能每跳一次重新扫描整卷。 */
            fs.ClusterCount=256;
            for(UINT32 c=2;c<=257;++c)store(&fs,c,c==257?2:c+1);
            reads=0;total=77;assert(!count_file_clusters(&fs,2,&total,&tail) && total==77);
            assert(reads<=((258U*(bits/8)+bps-1)/bps)+1);
            store(&fs,257,bits==16?0xfff8U:0x0ffffff8U);
            const UINT32 invalid[]={0,1,258,bits==16?0xfff7U:0x0ffffff7U};
            for(unsigned i=0;i<4;++i) {
                store(&fs,4,invalid[i]);reads=0;total=77;
                assert(!count_file_clusters(&fs,2,&total,&tail) && total==77);
            }
            store(&fs,4,5);fail_lba=fs.FatStartLba;reads=0;
            assert(!count_file_clusters(&fs,2,&total,&tail));fail_lba=UINT64_MAX;
        }
        OS_FAT32 small={.ClusterCount=5,.FatType=bits,.FatCount=2,
                        .BytesPerSector=512,.FatStartLba=32};
        /* 穷举五节点图；参考实现独立维护访问位图，不使用生产 extent 或统计函数。 */
        for(unsigned encoded=0;encoded<7776;++encoded) {
            unsigned value=encoded;UINT32 links[5];
            for(unsigned i=0;i<5;++i) {
                unsigned digit=value%6;value/=6;links[i]=digit?digit+1:0;
                store(&small,i+2,links[i]?links[i]:(bits==16?0xfff8U:0x0ffffff8U));
            }
            for(UINT32 first=2;first<7;++first) {
                unsigned seen=0,expected=0;UINT32 cursor=first,tail=0;bool valid=false;
                while(!(seen&(1U<<(cursor-2)))) {
                    seen|=1U<<(cursor-2);++expected;
                    if(!links[cursor-2]) {valid=true;break;}
                    cursor=links[cursor-2];
                }
                UINT64 total=99;reads=0;
                assert(!!count_file_clusters(&small,first,&total,&tail)==valid);
                if(valid)assert(total==expected && tail==cursor);
                else assert(total==99);
                assert(reads<=small.ClusterCount*2);
            }
        }
    }
    puts("FAT count: 77760 independent graph/start comparisons; contiguous, fragmented, cyclic, invalid-link and I/O failure cases passed");
}
'''
with tempfile.TemporaryDirectory(prefix='fat-cluster-count-') as directory:
    path = Path(directory)
    variants={'current':count,
              'short_probe':count.replace('cluster, remaining)',
                                         'cluster, remaining < 16U ? remaining : 16U)'),
              'repeated_volume_scan':count.replace('while (total < filesystem->ClusterCount)',
                    'for (UINT32 hops=0; hops<filesystem->ClusterCount; ++hops)')
                    .replace('filesystem->ClusterCount - total', 'filesystem->ClusterCount')}
    for name, implementation in variants.items():
        (path / 'test.c').write_text(program.replace(count,implementation,1))
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
        result=subprocess.run([str(path/'test')],capture_output=True,text=True,timeout=20)
        if name=='current':
            assert result.returncode==0,result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode!=0,name
            print('负对照拒绝：'+name)
