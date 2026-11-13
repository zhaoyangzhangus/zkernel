"""Actual IEEE helper and filesystem/PNG wrappers against bitwise oracle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
def function(path, start, end):
    source = (root/path).read_text()
    return source[source.index(start):source.index(end)]
wrappers = function('kernel/fs/nativefs/journal.c',
    'static uint32_t journal_crc(', 'static bool sequence_newer(')
wrappers += function('kernel/fs/nativefs/litefs.c',
    'static uint32_t litefs_crc(', 'static void litefs_lock(')
wrappers += function('kernel/graphics/png_chunks.c',
    'static uint32_t png_crc32(', 'static bool png_signature_valid(')
code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <kernel/crc32.h>
#include <kernel/journal.h>
typedef struct __attribute__((packed)) {
 uint32_t magic;uint64_t sequence;uint32_t kind;
 uint64_t target;uint32_t length;uint32_t checksum;
} journal_disk_header_t;
static uint32_t oracle(uint32_t crc,const uint8_t *data,size_t length) {
 for(size_t i=0;i<length;++i) {
  crc^=data[i];
  for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xEDB88320U:0);
 }
 return crc;
}
static uint32_t masked_oracle(const uint8_t *bytes,size_t n,size_t off) {
 uint32_t crc=UINT32_MAX;
 for(size_t i=0;i<n;++i) {
  uint8_t byte=i>=off && i-off<4?0:bytes[i];crc=oracle(crc,&byte,1);
 }
 return ~crc;
}
''' + wrappers + r'''
static volatile uint32_t sink;
static double seconds(void) {
 struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));
 return t.tv_sec+t.tv_nsec*1e-9;
}
int main(void) {
 assert(~crc32_ieee_update(UINT32_MAX,"123456789",9)==0xCBF43926U);
 assert(png_crc32((const uint8_t*)"IEND",0,0)==0xAE426082U);
 assert(crc32_ieee_zeroed(0,0,0)==0 && crc32_ieee_update(42,0,0)==42);
 uint8_t bytes[8192+16+4];unsigned rng=17;
 for(unsigned i=0;i<sizeof(bytes);++i) {rng=rng*1664525U+1013904223U;bytes[i]=rng>>24;}
 /* 覆盖所有长度和未对齐输入，并使用彼此独立的初始 CRC 值。 */
 for(size_t n=0;n<=8192;++n) {
  unsigned alignment=n&15;uint8_t *p=bytes+alignment;
  uint32_t initial=(uint32_t)n*12345U;
  uint32_t expected=oracle(initial,p,n);
  assert(crc32_ieee_update(initial,p,n)==expected);
  size_t split=n/3;
  assert(crc32_ieee_update(crc32_ieee_update(initial,p,split),p+split,n-split)==expected);
  size_t offsets[]={0,1,28,n/2,n?n-1:0,n,n+1,SIZE_MAX};
  for(unsigned j=0;j<sizeof(offsets)/sizeof(offsets[0]);++j)
   assert(litefs_crc(p,n,offsets[j])==masked_oracle(p,n,offsets[j]));
  assert(png_crc32(p,p+4,n)==~oracle(oracle(UINT32_MAX,p,4),p+4,n));
 }
 assert(journal_crc(bytes)==masked_oracle(bytes,JOURNAL_BLOCK_SIZE,28));
 uint32_t original=journal_crc(bytes);
 for(unsigned i=0;i<JOURNAL_BLOCK_SIZE;++i) {
  bytes[i]^=1;
  assert(journal_crc(bytes)==masked_oracle(bytes,JOURNAL_BLOCK_SIZE,28));
  assert((journal_crc(bytes)==original)==(i>=28 && i<32));bytes[i]^=1;
 }
 /* 精确长度保护页：不得越界读取到相邻的未映射页面。 */
 size_t page=(size_t)sysconf(_SC_PAGESIZE);
 uint8_t *guard=mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(guard!=MAP_FAILED && !mprotect(guard+page,page,PROT_NONE));
 memset(guard,0x69,page);
 for(size_t n=0;n<=page;++n) {
  uint8_t *p=guard+page-n;
  assert(crc32_ieee_update(UINT32_MAX,p,n)==oracle(UINT32_MAX,p,n));
  assert(litefs_crc(p,n,n/2)==masked_oracle(p,n,n/2));
 }
 assert(!munmap(guard,page*2));
 puts("IEEE CRC32: known vectors, 8193 lengths, alignment/splits, zeroed checksum fields, journal corruption, guard-page bounds passed");
#ifndef SANITIZED
 size_t length=256*1024;uint8_t *large=malloc(length);assert(large);memset(large,0x75,length);
 double start=seconds();for(unsigned i=0;i<32;++i)sink=oracle(UINT32_MAX,large,length);
 double old=seconds()-start;
 start=seconds();for(unsigned i=0;i<32;++i)sink=crc32_ieee_update(UINT32_MAX,large,length);
 double now=seconds()-start;
 printf("Host-only CRC microbenchmark: bitwise %.4fs, nibble %.4fs, %.2fx; not application speedup\n",old,now,old/now);
 free(large);
#else
 (void)sink;(void)seconds;
#endif
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-crc32-') as directory:
    for sanitized in (True, False):
        binary = str(Path(directory)/('checked' if sanitized else 'bench'))
        options = ['-fsanitize=address,undefined','-DSANITIZED'] if sanitized else []
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-I'+str(root/'include'),'-fno-pie','-no-pie',*options,
                        '-x','c','-','-o',binary],input=code,text=True,check=True)
        subprocess.run([binary],check=True,timeout=60)
