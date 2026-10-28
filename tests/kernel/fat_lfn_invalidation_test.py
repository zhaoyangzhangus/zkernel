"""Actual directory lookup/enumeration: lazy invalidation versus full reset."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
directory = (root / 'kernel/fs/nativefs/directory_lifecycle.c').read_text()
start = directory.index('UINT8 fat32_short_name_checksum(')
lookup = directory[start:directory.index('BOOLEAN fat32_find_directory_entry(', start)]
source = (root / 'kernel/fs/nativefs/fat32.c').read_text()
start = source.index('static BOOLEAN fat32_enumerate_directory(')
enumerate_source = source[start:source.index('static BOOLEAN make_creation_name(', start)]
codec = (root / 'kernel/fs/nativefs/directory_codec.c').read_text()
codec = codec.replace('    for (UINT32 i = 0U; i < 260U; ++i) characters[i] = 0xFFFFU;',
                      '    ++full_resets;\n    for (UINT32 i = 0U; i < 260U; ++i) characters[i] = 0xFFFFU;')
assert '++full_resets' in codec
codec = codec.replace('    UINT32 cursor=0U, used=0U;',
                      '    ++encodings;\n    UINT32 cursor=0U, used=0U;', 1)
assert '++encodings' in codec
old_lookup = lookup.replace('fat32_lfn_invalidate(', 'fat32_lfn_reset(lfn_characters, ')
old_lookup = old_lookup[old_lookup.index('BOOLEAN fat32_find_directory_entry_ex('):]
old_lookup = old_lookup.replace('fat32_find_directory_entry_ex(', 'previous_find(', 1)
old_enumerate = enumerate_source.replace('fat32_lfn_invalidate(',
                                          'fat32_lfn_reset(lfn_characters, ')
old_enumerate = old_enumerate.replace('fat32_enumerate_path(', 'previous_enumerate(')
old_enumerate = old_enumerate.replace('fat32_enumerate_directory(', 'previous_directory(')
old_enumerate = old_enumerate.replace('fat32_copy_directory_entry', 'previous_copy_entry')
old_enumerate = old_enumerate.replace('fat32_visit_directory(', 'previous_visit_directory(')
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#define FAT32_ENTRY_SIZE 32U
#define FAT32_DIRECTORY 0x10U
#define FAT32_ATTRIBUTE_LFN 0x0FU
#define FAT32_MAX_LFN_ENTRIES 20U
static unsigned full_resets,sector_reads,directory_resolutions,encodings;
static UINT64 failed_lba=UINT64_MAX;
static UINT8 media[64*512];
static OS_FAT32 fs;
BOOLEAN fat32_cluster_valid(const OS_FAT32 *f,UINT32 c) {
    return c>=2&&c<=f->ClusterCount+1;
}
BOOLEAN fat32_root_directory_cluster(const OS_FAT32 *f,UINT32 c) {
    return f->FatType==16&&c==0;
}
UINT64 fat32_cluster_lba(const OS_FAT32 *f,UINT32 c) {(void)f;return c;}
BOOLEAN fat32_read_next_cluster(const OS_FAT32 *f,UINT32 c,UINT32 *n) {
    *n=f->FatType==32&&c<5?c+1:0;return 1;
}
BOOLEAN fat32_read_sector(const OS_FAT32 *f,UINT64 lba,UINT8 *out) {
    ++sector_reads;
    if(lba==failed_lba)return 0;
    assert(lba<64);memcpy(out,media+lba*512,f->BytesPerSector);return 1;
}
BOOLEAN fat32_resolve_directory_cluster(OS_FAT32 *f,const CHAR8 *p,UINT32 *c) {
    ++directory_resolutions;
    (void)p;*c=f->FatType==32?2:0;return 1;
}
'''+codec+lookup+enumerate_source+old_lookup+old_enumerate+r'''
static void compare_find(const char *name, unsigned length) {
    UINT8 a[32],b[32];UINT64 la=999,lb=999,lfa[20],lfb[20];
    UINT32 oa=999,ob=999,ca=999,cb=999,ofa[20],ofb[20];
    memset(a,0x75,sizeof(a));memcpy(b,a,sizeof(a));
    memset(lfa,0x75,sizeof(lfa));memcpy(lfb,lfa,sizeof(lfa));
    memset(ofa,0x75,sizeof(ofa));memcpy(ofb,ofa,sizeof(ofa));
    BOOLEAN ok=fat32_find_directory_entry_ex(&fs,0,name,length,a,&la,&oa,lfa,ofa,&ca);
    BOOLEAN old=previous_find(&fs,0,name,length,b,&lb,&ob,lfb,ofb,&cb);
    assert(ok==old&&la==lb&&oa==ob&&ca==cb);
    assert(!memcmp(a,b,sizeof(a))&&!memcmp(lfa,lfb,sizeof(lfa))&&!memcmp(ofa,ofb,sizeof(ofa)));
}
typedef struct {os_file_info_t entries[256];unsigned count,stop;} collection_t;
static BOOLEAN collect(VOID *context,const os_file_info_t *info){
    collection_t *c=context;assert(c->count<256);c->entries[c->count++]=*info;
    return c->count<c->stop;
}
static unsigned bulk_cases;
static void compare_bulk(unsigned limit,unsigned stop){
    collection_t before,after;memset(&before,0x75,sizeof(before));after=before;
    before.count=after.count=0;before.stop=after.stop=stop;
    for(unsigned i=0;i<limit;++i){
        os_file_info_t info={0};if(!previous_enumerate(&fs,"/",i,&info))break;
        if(!collect(&before,&info))break;
    }
    fat32_visit_directory(&fs,"/",limit,collect,&after);
    assert(!memcmp(&before,&after,sizeof(before)));++bulk_cases;
}
static void compare_enumerate(unsigned index) {
    os_file_info_t a,b;memset(&a,0x75,sizeof(a));memcpy(&b,&a,sizeof(a));
    BOOLEAN ok=fat32_enumerate_path(&fs,"/",index,&a);
    BOOLEAN old=previous_enumerate(&fs,"/",index,&b);
    assert(ok==old&&!memcmp(&a,&b,sizeof(a)));
}
static void short_entry(unsigned slot,unsigned number) {
    UINT8 *e=media+slot*32;char name[16];
    assert(number<10000000U);
    snprintf(name,sizeof(name),"F%07u",number);
    memset(e,0,32);memcpy(e,name,8);memcpy(e+8,"TXT",3);
}
static unsigned long_entries(unsigned slot,const char *name,unsigned length,unsigned number) {
    const unsigned offsets[]={1,3,5,7,9,14,16,18,20,22,24,28,30};
    UINT8 short_name[32]={0};char base[9];
    snprintf(base,sizeof(base),"F%07u",number);
    memcpy(short_name,base,8);memcpy(short_name+8,"TXT",3);
    UINT8 sum=fat32_short_name_checksum(short_name);
    unsigned count=(length+12)/13;
    for(unsigned ordinal=count;ordinal;--ordinal) {
        UINT8 *e=media+slot++*32;memset(e,0xff,32);
        e[0]=(UINT8)(ordinal|(ordinal==count?0x40:0));e[11]=0x0f;e[13]=sum;
        for(unsigned i=0;i<13;++i) {
            unsigned n=(ordinal-1)*13+i;
            UINT16 value=n<length?(UINT8)name[n]:n==length?0:0xffff;
            e[offsets[i]]=(UINT8)value;e[offsets[i]+1]=(UINT8)(value>>8);
        }
    }
    memcpy(media+slot++*32,short_name,32);return slot;
}
int main(void) {
    fs.FatType=16;fs.BytesPerSector=512;fs.ClusterCount=128;
    fs.RootDirectorySectors=64;fs.SectorsPerCluster=1;
    for(unsigned i=0;i<1000;++i)short_entry(i,i);
    UINT8 result[32];UINT32 count;full_resets=0;
    assert(!fat32_find_directory_entry_ex(&fs,0,"absent.txt",10,result,0,0,0,0,&count));
    assert(full_resets==0);
    assert(!previous_find(&fs,0,"absent.txt",10,result,0,0,0,0,&count));
    assert(full_resets==1001);
    os_file_info_t info;full_resets=0;
    assert(fat32_enumerate_path(&fs,"/",999,&info));
    assert(!strcmp(info.name,"F0000999.TXT")&&full_resets==0);
    assert(previous_enumerate(&fs,"/",999,&info)&&full_resets==1000);
    puts("1000 short entries: lookup/enumeration full 520-byte clears 1001/1000 -> 0");
    /* Measure the exact VFS call pattern, not an assumed disk latency:
     * each index restarts production enumeration at the first sector. */
    unsigned sizes[]={16,64,256};
    for(unsigned sample=0;sample<3;++sample){
        unsigned n=sizes[sample];memset(media,0,sizeof(media));
        for(unsigned i=0;i<n;++i)short_entry(i,i);
        sector_reads=directory_resolutions=0;
        unsigned expected_reads=0;
        for(unsigned i=0;i<n;++i){
            assert(fat32_enumerate_path(&fs,"/",i,&info));
            char expected_name[16];snprintf(expected_name,sizeof(expected_name),"F%07u.TXT",i);
            assert(!strcmp(info.name,expected_name));
            expected_reads+=i/16+1;
        }
        /* VFS stops at its 256-entry bound without requesting index 256. */
        if(n<256){assert(!fat32_enumerate_path(&fs,"/",n,&info));expected_reads+=n/16+1;}
        assert(sector_reads==expected_reads&&directory_resolutions==n+(n<256));
        printf("FAT enumeration baseline: %u entries -> %u sector-read calls, %u directory resolutions; sequential coverage is %u sectors (cache may hide device reads)\n",
               n,sector_reads,directory_resolutions,(n+15)/16+(n<256));
        collection_t batch={.stop=256};sector_reads=directory_resolutions=0;
        fat32_visit_directory(&fs,"/",256,collect,&batch);
        unsigned linear=n/16+(n<256);
        assert(batch.count==n&&sector_reads==linear&&directory_resolutions==1);
        printf("FAT one-pass: %u entries -> %u sector-read calls, one directory resolution\n",n,sector_reads);
        for(unsigned limit=0;limit<=256;limit+=16){
            compare_bulk(limit,256);compare_bulk(limit,1);compare_bulk(limit,17);
        }
        for(unsigned sector=0;sector<n/16;++sector){
            failed_lba=sector;compare_bulk(256,256);failed_lba=UINT64_MAX;
        }
    }
    unsigned cases=0;
    for(unsigned length=1;length<=255;++length)for(unsigned mode=0;mode<7;++mode) {
        memset(media,0,sizeof(media));char name[256],second[32];
        memset(name,'a',length);name[length]=0;
        /* Start across a sector boundary, with deletion/volume separators. */
        for(unsigned i=0;i<15;++i) {short_entry(i,i);media[i*32]=0xe5;}
        unsigned count_lfn=(length+12)/13;
        unsigned end=long_entries(15,name,length,123);
        if(mode==1)media[15*32+13]^=1;
        if(mode==2&&count_lfn>1)media[16*32]=0xe5;
        if(mode==3)media[(end-1)*32]=0xe5;
        if(mode==4)media[15*32]=0x55; /* invalid ordinal */
        if(mode==5)media[15*32]=0x40; /* invalid zero ordinal head */
        if(mode==6)media[(end-1)*32+11]=0x08; /* volume-label separator */
        snprintf(second,sizeof(second),"Next shorter file %u",mode);
        long_entries(end,second,(unsigned)strlen(second),124);
        compare_find(name,length);compare_find(second,(unsigned)strlen(second));
        compare_find("F0000123.TXT",12);compare_find("absent.txt",10);
        for(unsigned i=0;i<3;++i)compare_enumerate(i);
        compare_bulk(256,256);compare_bulk(1,256);compare_bulk(256,1);
        if(mode==0) {
            full_resets=0;
            assert(fat32_find_directory_entry_ex(&fs,0,name,length,result,0,0,0,0,0));
            assert(full_resets==1); /* only the actual LFN sequence head */
        }
        ++cases;
    }
    fs.FatType=32;memset(media,0,sizeof(media));
    for(unsigned i=0;i<64;++i)short_entry(32+i,i);
    for(unsigned limit=0;limit<=64;++limit)compare_bulk(limit,256);
    for(unsigned sector=2;sector<=5;++sector){failed_lba=sector;compare_bulk(256,256);}
    failed_lba=UINT64_MAX;fs.FatType=16;
    printf("FAT batch: %u full-output/count/canary comparisons, LFN/deletions/volume labels/callback stop/limits/read failures/FAT16 root/FAT32 chain passed\n",bulk_cases);
    /* Invalidation never reads or writes the character arena; a subsequent
     * head completely initializes it before publishing valid state. */
    UINT16 chars[260];memset(chars,0x75,sizeof(chars));BOOLEAN valid=1;
    UINT32 expected=7;UINT8 checksum=19;
    fat32_lfn_invalidate(&valid,&expected,&checksum);
    assert(!valid&&!expected&&!checksum);
    for(unsigned i=0;i<260;++i)assert(chars[i]==0x7575);
    memset(media,0,sizeof(media));long_entries(0,"tiny",4,125);
    fat32_lfn_store_entry(media,chars,&valid,&expected,&checksum);
    assert(valid&&!expected&&chars[0]=='t'&&chars[4]==0);
    for(unsigned i=5;i<260;++i)assert(chars[i]==0xffff);
    /* 扫过多个长名称时只转换一次；匹配首项短别名时无需转换。 */
    memset(media,0,sizeof(media));unsigned slot=0;char query[64];
    for(unsigned i=0;i<80;++i){
        snprintf(query,sizeof(query),"Long query file number %03u",i);
        slot=long_entries(slot,query,strlen(query),i);
    }
    encodings=0;
    assert(fat32_find_directory_entry_ex(&fs,0,query,strlen(query),result,0,0,0,0,&count));
    assert(encodings==1);
    encodings=0;
    assert(!fat32_find_directory_entry_ex(&fs,0,"absent long query file",22,result,0,0,0,0,&count));
    assert(encodings==1);
    encodings=0;
    assert(!fat32_find_directory_entry_ex(&fs,0,"\x80",1,result,0,0,0,0,&count));
    assert(encodings==1);
    encodings=0;
    assert(fat32_find_directory_entry_ex(&fs,0,"F0000000.TXT",12,result,0,0,0,0,&count));
    assert(encodings==0 && count==2);
    puts("LFN query: 80 candidates -> one UTF-8 conversion; invalid input cached, matching short alias needs zero conversions");
    /* 短别名命中不能把校验和不符的前导 LFN 归入该文件。 */
    memset(media,0,sizeof(media));long_entries(0,"other name",10,123);
    media[13]^=1;count=99;
    assert(fat32_find_directory_entry_ex(&fs,0,"F0000123.TXT",12,result,0,0,0,0,&count));
    assert(count==0);
    printf("LFN lookup/enumeration: %u length/sector-boundary/malformed/deletion/stale-name cases match old behavior\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fat-lfn-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root / 'include'), '-I'+str(root / 'kernel/fs/nativefs'),
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
