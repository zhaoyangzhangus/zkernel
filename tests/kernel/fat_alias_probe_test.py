"""执行真实别名分配及创建逻辑，覆盖单遍扫描、冲突边界及读盘失败。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
codec = (root / 'kernel/fs/nativefs/directory_codec.c').read_text()
codec = codec.replace('    UINT32 cursor=0U, used=0U;',
                      '    ++encodings;\n    UINT32 cursor=0U, used=0U;', 1)
assert '++encodings' in codec
source = (root / 'kernel/fs/nativefs/directory_lifecycle.c').read_text()
probe = source[source.index('static void fat32_mark_alias('):source.index('BOOLEAN fat32_find_directory_entry_ex(')]
source = (root / 'kernel/fs/nativefs/fat32.c').read_text()
creation = source[source.index('static BOOLEAN make_creation_name('):source.index('static BOOLEAN fat32_create_path_locked(')]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#define FAT32_ENTRY_SIZE 32U
#define FAT32_ATTRIBUTE_LFN 0x0FU
#define FAT32_MAX_LFN_ENTRIES 20U
static UINT8 media[128][512];
static UINT32 links[128], reads, fail_lba, fail_link;
static OS_FAT32 fs;
static unsigned encodings;
BOOLEAN fat32_cluster_valid(const OS_FAT32 *f, UINT32 c) {
    return c >= 2 && c < f->ClusterCount + 2;
}
BOOLEAN fat32_root_directory_cluster(const OS_FAT32 *f, UINT32 c) {
    return f->FatType == 16 && c == 0;
}
UINT64 fat32_cluster_lba(const OS_FAT32 *f, UINT32 c) {(void)f;return c;}
BOOLEAN fat32_read_sector(const OS_FAT32 *f, UINT64 lba, UINT8 *out) {
    (void)f;++reads;assert(lba < 128);
    if (lba == fail_lba) return 0;
    memcpy(out,media[lba],512);return 1;
}
BOOLEAN fat32_read_next_cluster(const OS_FAT32 *f, UINT32 c, UINT32 *next) {
    (void)f;assert(c < 128);if(c == fail_link)return 0;
    *next=links[c];return 1;
}
''' + codec + probe + creation + r'''
static void reset(unsigned type) {
    memset(&fs,0,sizeof(fs));memset(media,0,sizeof(media));memset(links,0,sizeof(links));
    fs.FatType=type;fs.BytesPerSector=512;fs.ClusterCount=126;
    fs.SectorsPerCluster=1;fs.RootDirectoryLba=6;fs.RootDirectorySectors=2;
    fail_lba=fail_link=999;reads=0;
}
static void full(unsigned lba) {
    for(unsigned i=0;i<16;++i){memset(media[lba]+i*32,'Q',11);media[lba][i*32+11]=0x20;}
}
static void entry(unsigned lba,unsigned slot,const UINT8 name[11]) {
    memcpy(media[lba]+slot*32,name,11);media[lba][slot*32+11]=0x20;
}
static void check(unsigned parent,const char *leaf,BOOLEAN success,unsigned suffix) {
    UINT8 out[11],expected[11];memset(out,0x75,sizeof(out));
    assert(fat32_allocate_long_name_alias(&fs,parent,leaf,strlen(leaf),out)==success);
    if(success){
        assert(fat32_make_long_name_alias(leaf,strlen(leaf),suffix,expected));
        assert(!memcmp(out,expected,11));
    }else for(unsigned i=0;i<11;++i)assert(out[i]==0x75);
}
int main(void) {
    UINT8 name[11],second[11];fat32_creation_name_t prepared;
    const char *leaf="下载.bin";UINT32 length=strlen(leaf);
    assert(fat32_make_long_name_alias(leaf,length,1,name));
    assert(fat32_make_long_name_alias(leaf,length,2,second));
    for(unsigned type=16;type<=32;type+=16){
        unsigned parent=type==16?0:2, first=type==16?6:2, last=type==16?7:3;
        reset(type);check(parent,leaf,1,1);assert(reads==1);
        entry(first,0,name);check(parent,leaf,1,2);
        media[first][0]=0xE5;check(parent,leaf,1,1);
        entry(first,0,name);media[first][11]=0x0F;check(parent,leaf,1,1);
        reset(type);full(first);links[first]=last;entry(last,0,name);
        check(parent,leaf,1,2);
        fail_lba=last;check(parent,leaf,0,0);
        assert(!make_creation_name(&fs,parent,leaf,length,&prepared));
        reset(type);full(first);links[first]=last;full(last);check(parent,leaf,1,1);
        reset(type);entry(first,0,name);
        assert(make_creation_name(&fs,parent,leaf,length,&prepared));
        assert(prepared.lfn_count==1 && !memcmp(prepared.short_name,second,11));
    }
    reset(32);full(2);fail_link=2;check(2,leaf,0,0);
    fail_link=999;links[2]=2;check(2,leaf,0,0);
    links[2]=1;check(2,leaf,0,0);
    fs.BytesPerSector=511;check(2,leaf,0,0);
    fs.BytesPerSector=8192;check(2,leaf,0,0);
    fs.BytesPerSector=512;fs.SectorsPerCluster=0;check(2,leaf,0,0);
    assert(!fat32_allocate_long_name_alias(NULL,2,leaf,length,prepared.short_name));
    assert(!fat32_allocate_long_name_alias(&fs,2,leaf,length,NULL));
    reset(32);
    for(unsigned i=0;i<80;++i){
        fat32_make_long_name_alias(leaf,length,i+1,name);entry(2+i/16,i%16,name);
    }
    for(unsigned i=2;i<7;++i)links[i]=i+1;
    check(2,leaf,1,81);assert(reads==6);
    printf("80 alias collisions: 246 -> %u sector-read calls\n",reads);
    /* 覆盖后缀位数变化、前缀自带波浪号、纯中文及别名空间耗尽。 */
    const char *names[]={"下载.bin","abcdefghijkl.txt","a~b~cdefghi.txt",".中文","中文.图片"};
    for(unsigned n=0;n<sizeof(names)/sizeof(names[0]);++n){
        const char *text=names[n];reset(32);
        for(unsigned suffix=1;suffix<1000;++suffix){
            assert(fat32_make_long_name_alias(text,strlen(text),suffix,name));
            entry(2+(suffix-1)/16,(suffix-1)%16,name);
        }
        for(unsigned i=2;i<64;++i)links[i]=i+1;
        check(2,text,0,0);assert(reads==63);
        for(unsigned hole=1;hole<1000;hole+=17){
            unsigned slot=hole-1;UINT8 *e=media[2+slot/16]+slot%16*32;
            UINT8 saved=e[0];e[0]=0xE5;reads=0;
            check(2,text,1,hole);assert(reads==63);e[0]=saved;
        }
    }
    /* 数字相同但不同扩展名、前缀或带前导零的别名不能占用候选。 */
    reset(32);entry(2,0,(const UINT8 *)"_~01    BIN");
    entry(2,1,(const UINT8 *)"X~1     BIN");entry(2,2,(const UINT8 *)"_~1     TXT");
    check(2,leaf,1,1);
    /* 最长文件名的 20 个目录项只复用一次编码；短名称无需进入转换路径。 */
    reset(32);char maximum[256];memset(maximum,'x',255);maximum[255]=0;
    encodings=0;assert(make_creation_name(&fs,2,maximum,255,&prepared));
    assert(encodings==1 && prepared.units==255 && prepared.lfn_count==20);
    UINT8 slot[32];
    for(unsigned ordinal=prepared.lfn_count;ordinal;--ordinal)
        fat32_make_lfn_entry(prepared.characters,prepared.units,ordinal,prepared.checksum,slot);
    assert(encodings==1);
    encodings=0;assert(make_creation_name(&fs,2,"a.txt",5,&prepared));
    assert(encodings==0 && prepared.units==0 && prepared.lfn_count==0);
    puts("FAT alias allocation: one-pass scans, 999 suffixes, 295 holes, FAT16/FAT32, I/O failures and invalid/cyclic chains passed");
}

'''
with tempfile.TemporaryDirectory(prefix='fat-alias-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root/'include'), '-I'+str(root/'kernel/fs/nativefs'),
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
