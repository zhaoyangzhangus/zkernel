"""Actual FAT read/write functions, modeled media and independent byte oracle."""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/nativefs/file_lifecycle.c').read_text()
functions = source[source.index('static UINT64 fat32_contiguous_extent_bytes('):
                   source.index('static BOOLEAN fat32_write(')]
assert functions.count('__builtin_memcpy(') == 2
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int BOOLEAN;
typedef void VOID;
#define OS_BLOCK_CACHE_ENTRY_COUNT 16
#define OS_BLOCK_CACHE_MAX_READ_BYTES (1024U*1024U)
#define OS_BLOCK_CACHE_MAX_WRITE_BYTES (1024U*1024U)
typedef struct { int Initialized; } CACHE;
typedef struct { UINT32 BytesPerSector, SectorsPerCluster; CACHE Cache; void *Device; BOOLEAN Fat3Available; } OS_FAT32;
typedef struct {
    OS_FAT32 *FileSystem;
    UINT64 Size;
    UINT32 FirstCluster, CursorLogicalStart, CursorLength, CursorPhysicalStart;
    int Used, CursorValid;
} OS_FAT32_FILE;
static unsigned char media[131072], expected[131072], output[131136], input[131136];
static OS_FAT32 fs;
static int fail_read, fail_write, reads, writes, ranges, copies;
void *liteos_memcpy(void *,const void *,size_t);
static void *counted_copy(void *d,const void *s,size_t n) {
    ++copies; return liteos_memcpy(d,s,n);
}
static void fat32_diag_failure(const char *s,UINT64 a,UINT64 b,UINT64 c) {
    (void)s;(void)a;(void)b;(void)c;
}
static BOOLEAN locate_file_cluster(OS_FAT32_FILE *f,UINT32 n,UINT32 *p) {
    (void)f;*p=n;return 1;
}
static UINT32 fat32_extent_length(const OS_FAT32 *f,UINT32 c,UINT32 maximum) {
    (void)f;(void)c;(void)maximum;return 1U;
}
static UINT64 fat32_cluster_lba(OS_FAT32 *f,UINT32 p) {
    return (UINT64)p*f->SectorsPerCluster;
}
static BOOLEAN fat32_read_sector(OS_FAT32 *f,UINT64 lba,void *out) {
    if(reads++==fail_read)return 0;
    assert((lba+1)*f->BytesPerSector<=sizeof(media));
    memcpy(out,media+lba*f->BytesPerSector,f->BytesPerSector);return 1;
}
static BOOLEAN block_read(void *d,UINT64 lba,UINT32 n,void *out) {
    (void)d;++ranges;
    assert((lba+n)*fs.BytesPerSector<=sizeof(media));
    memcpy(out,media+lba*fs.BytesPerSector,n*fs.BytesPerSector);return 1;
}
static BOOLEAN block_cache_read_range(CACHE *c,UINT64 lba,UINT32 n,void *out) {
    (void)c;return block_read(0,lba,n,out);
}
static BOOLEAN block_cache_write(CACHE *c,UINT64 lba,const void *in) {
    (void)c;if(writes++==fail_write)return 0;
    memcpy(media+lba*fs.BytesPerSector,in,fs.BytesPerSector);return 1;
}
static BOOLEAN block_cache_write_range(CACHE *c,UINT64 lba,UINT32 n,const void *in) {
    (void)c;(void)lba;(void)n;(void)in;assert(0);return 0;
}
static BOOLEAN extend_file_to(OS_FAT32_FILE *f,UINT64 n,UINT64 zero_until) {
    (void)zero_until;
    if(n>sizeof(media))return 0;
    if(n>f->Size)f->Size=n;
    return 1;
}
'''+functions.replace('__builtin_memcpy(', 'counted_copy(')+r'''
int main(void) {
    unsigned cases=0;
    for(unsigned i=0;i<sizeof(input);++i)input[i]=(unsigned char)(i*29+7);
    for(unsigned bps=512;bps<=4096;bps*=2)
    for(unsigned spc=1;spc<=8;spc*=2)
    for(unsigned cached=0;cached<2;++cached)
    for(unsigned align=0;align<16;++align)
    for(unsigned edge=0;edge<5;++edge)
    for(unsigned length=0;length<6;++length) {
        unsigned offsets[]={0,1,bps-1,bps,bps*spc-1};
        unsigned sizes[]={1,7,bps-1,bps,bps+1,bps*16+7};
        unsigned off=offsets[edge],n=sizes[length];
        unsigned sectors=(off%bps+n+bps-1)/bps;
        unsigned partial=(off%bps!=0)+( (off+n)%bps!=0);
        if(sectors==1&&partial==2)partial=1;
        fs=(OS_FAT32){.BytesPerSector=bps,.SectorsPerCluster=spc,.Cache={cached}};
        OS_FAT32_FILE file={.FileSystem=&fs,.Size=sizeof(media),.FirstCluster=1,.Used=1};
        for(unsigned i=0;i<sizeof(media);++i)media[i]=(unsigned char)(i*13+5);
        memset(output,0x75,sizeof(output));
        fail_read=fail_write=-1;reads=writes=ranges=copies=0;
        UINT32 done=0;
        assert(fat32_read(&file,off,output+align,n,&done)&&done==n);
        for(unsigned i=0;i<sizeof(output);++i)
            assert(output[i]==(i>=align&&i<align+n?media[off+i-align]:0x75));
        assert((unsigned)copies==partial);++cases;
        memcpy(expected,media,sizeof(media));
        for(unsigned i=0;i<n;++i)expected[off+i]=input[align+i];
        reads=writes=ranges=copies=0;
        assert(fat32_write_locked(&file,off,input+align,n,&done)&&done==n);
        assert(!memcmp(media,expected,sizeof(media))&&(unsigned)copies==partial);
        assert((unsigned)writes==sectors);
        assert((unsigned)reads==partial);++cases;
    }
    /* Errors after a completed sector must preserve exact prior progress. */
    fs=(OS_FAT32){.BytesPerSector=512,.SectorsPerCluster=8,.Cache={1}};
    OS_FAT32_FILE file={.FileSystem=&fs,.Size=sizeof(media),.FirstCluster=1,.Used=1};
    for(unsigned fail=0;fail<3;++fail) {
        UINT32 done=999;reads=writes=copies=0;fail_read=fail;fail_write=-1;
        memset(output,0x75,sizeof(output));
        assert(!fat32_read(&file,0,output,1536,&done)&&done==fail*512);
        for(unsigned i=done;i<sizeof(output);++i)assert(output[i]==0x75);
        ++cases;
        fail_read=-1;fail_write=fail;reads=writes=copies=0;
        memcpy(expected,media,sizeof(media));
        for(unsigned i=0;i<fail*512;++i)expected[i]=input[i];
        assert(!fat32_write_locked(&file,0,input,1536,&done)&&done==fail*512);
        assert(!memcmp(media,expected,sizeof(media)));++cases;
    }
    printf("FAT sector copy: %u geometry/alignment/boundary/cache/failure cases; full sectors have no staging copies PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fat-copy-') as directory:
    path = Path(directory)
    obj, binary = str(path / 'string.o'), str(path / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-fno-builtin', '-mno-sse', '-mno-mmx',
                    '-I'+str(root / 'include'), '-fsanitize=address,undefined', '-fno-pie',
                    '-Dmemcpy=liteos_memcpy', '-Dmemmove=liteos_memmove',
                    '-Dmemset=liteos_memset', '-Dmemcmp=liteos_memcmp',
                    '-c', str(root / 'kernel/string.c'), '-o', obj], check=True)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-x', 'none', obj, '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
    # 独立拒绝重新引入完整扇区的读或写中转，不能只验证最终字节正确。
    variants = {
        'read': code.replace('BOOLEAN complete_sector = sector_offset == 0U &&\n                                  copy_size == filesystem->BytesPerSector;',
                             'BOOLEAN complete_sector = 0;'),
        'write': code.replace('const UINT8 *data = source + *written_size;',
                              'const UINT8 *data = source + *written_size;\n'
                              'if (sector_offset == 0U && copy_size == filesystem->BytesPerSector) {\n'
                              ' counted_copy(sector,data,copy_size);data=sector; }'),
    }
    for name, negative in variants.items():
        assert negative != code
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-x', 'c', '-', '-x', 'none', obj, '-o', binary],
                       input=negative, text=True, check=True)
        failed = subprocess.run([binary], capture_output=True, text=True, timeout=60,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert failed.returncode != 0 and 'copies==partial' in failed.stderr, failed.stderr
        print('Negative control rejected: full-sector '+name+' staging')
