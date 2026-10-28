"""Production free-cluster search vs original entry reader; sector read counts."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/fs/nativefs/fat_table.c').read_text()
def function(signature):
    start=source.index(signature)
    return source[start:source.index('\n}',start)+2]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef int BOOLEAN;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
#define FAT_TABLE_TYPE_16 16U
typedef struct {
    UINT32 ClusterCount,FatCount,FatType,BytesPerSector,FatSectors;
    UINT64 FatStartLba;
    UINT32 FreeClusterSearchStart;
} OS_FAT32;
static UINT8 disk[131072];
static unsigned reads;
static UINT64 fail_lba=UINT64_MAX;
static UINT16 fat32_read_u16(const UINT8 *p) {return p[0] | ((UINT16)p[1]<<8);}
static UINT32 fat32_read_u32(const UINT8 *p) {
    return fat32_read_u16(p) | ((UINT32)fat32_read_u16(p+2)<<16);
}
static BOOLEAN fat32_read_sector(const OS_FAT32 *f,UINT64 lba,UINT8 *out) {
    ++reads;
    assert(lba>=f->FatStartLba);
    if(lba==fail_lba) return 0;
    size_t pos=(lba-f->FatStartLba)*f->BytesPerSector;
    assert(pos+f->BytesPerSector<=sizeof(disk));
    memcpy(out,disk+pos,f->BytesPerSector);return 1;
}
''' + function('BOOLEAN fat32_cluster_valid(') + '\n'
program += function('BOOLEAN fat32_read_fat_entry(') + '\n'
program += function('BOOLEAN fat32_find_free_cluster_from(') + '\n'
program += function('BOOLEAN fat32_find_free_cluster(') + r'''
static BOOLEAN oracle(OS_FAT32 *f,UINT32 *out) {
    for(UINT32 c=2;c<=f->ClusterCount+1;++c) {
        UINT32 value;
        if(!fat32_read_fat_entry(f,1,c,&value)) return 0;
        if(!value) {*out=c;return 1;}
    }
    return 0;
}
static void put(OS_FAT32 *f,UINT32 c,UINT32 value) {
    unsigned size=f->FatType==16?2:4;
    for(unsigned i=0;i<size;++i) disk[c*size+i]=(UINT8)(value>>(8*i));
}
static void check(OS_FAT32 *f) {
    /* 此测试直接重写模拟介质，显式使内存中的搜索下界失效。 */
    f->FreeClusterSearchStart=2;
    UINT32 expected=0xdeadbeef,actual=expected;
    reads=0;
    BOOLEAN ok=oracle(f,&expected);
    unsigned original_reads=reads;
    reads=0;
    assert(fat32_find_free_cluster(f,&actual)==ok && actual==expected);
    unsigned size=f->FatType==16?2:4;
    UINT32 last=ok?expected:f->ClusterCount+1;
    UINT64 end_sector=(UINT64)last*size/f->BytesPerSector;
    if(fail_lba!=UINT64_MAX && fail_lba-f->FatStartLba<end_sector)
        end_sector=fail_lba-f->FatStartLba;
    unsigned sectors=(unsigned)(end_sector-2*size/f->BytesPerSector+1);
    assert(reads==sectors);
    if(!ok && fail_lba==UINT64_MAX && f->BytesPerSector==512)
        printf("FAT%u full 8192 clusters: sector reads %u -> %u\n",
               f->FatType,original_reads,reads);
}
int main(void) {
    for(unsigned type=16;type<=32;type+=16) {
        for(unsigned bps=512;bps<=4096;bps*=2) {
            OS_FAT32 f={8192,2,type,bps,64,17,2};
            memset(disk,0xff,sizeof(disk));put(&f,8194,0);check(&f);
            unsigned per_sector=bps/(type==16?2:4);
            UINT32 positions[]={2,per_sector-1,per_sector,per_sector+1,8193};
            for(unsigned i=0;i<sizeof(positions)/sizeof(*positions);++i) {
                memset(disk,0xff,sizeof(disk));put(&f,positions[i],0);check(&f);
                put(&f,positions[i],0xff); /* allocation must be seen next call */
                put(&f,8193,0);check(&f);
            }
            memset(disk,0xff,sizeof(disk));put(&f,12,0);put(&f,7,0);check(&f);
            for(unsigned i=0;i<100;++i) {
                memset(disk,0xff,sizeof(disk));
                put(&f,2+(i*7919U)%8192,0);check(&f);
            }
            memset(disk,0xff,sizeof(disk));
            fail_lba=f.FatStartLba;check(&f);
            fail_lba=f.FatStartLba+2;check(&f);fail_lba=UINT64_MAX;
            if(type==32) {put(&f,2,0xf0000000U);check(&f);}
            memset(disk,0xff,sizeof(disk));put(&f,7,0);put(&f,1300,0);
            UINT32 found=123;reads=0;
            assert(fat32_find_free_cluster_from(&f,1023,&found) && found==1300);
            unsigned entry_size=type==16?2:4;
            assert(reads==1300*entry_size/bps-1023*entry_size/bps+1);
            put(&f,8193,0);assert(fat32_find_free_cluster_from(&f,8193,&found) && found==8193);
            found=123;reads=0;
            assert(!fat32_find_free_cluster_from(&f,8194,&found) && found==123 && !reads);
            assert(!fat32_find_free_cluster_from(&f,1,&found) && found==123 && !reads);
        }
    }
    OS_FAT32 f={100,1,32,512,8,17,2}; UINT32 out=123;
    assert(!fat32_find_free_cluster(NULL,&out) && out==123);
    assert(!fat32_find_free_cluster(&f,NULL));
    f.FatCount=0;assert(!fat32_find_free_cluster(&f,&out) && out==123);
    f.FatCount=1;f.BytesPerSector=8192;
    assert(!fat32_find_free_cluster(&f,&out) && out==123);
    f.BytesPerSector=0;assert(!fat32_find_free_cluster(&f,&out) && out==123);
    f.BytesPerSector=512;f.ClusterCount=0;reads=0;
    assert(!fat32_find_free_cluster(&f,&out) && out==123 && !reads);
    puts("FAT free scan: FAT16/32, 512..4096-byte sectors, first free, full, boundaries, failures and fresh reads passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fat-free-') as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=30)
