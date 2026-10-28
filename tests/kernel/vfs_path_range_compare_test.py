"""Actual VFS range predicate with actual kernel memcmp and a byte oracle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
predicate = source[source.index('static bool vfs_path_range_equals('):
                   source.index('static uint32_t vfs_free_node_slot_locked(')].replace('memcmp(', 'actual_memcmp(')
strings = (root / 'kernel/string.c').read_text()
compare = strings[strings.index('int memcmp('):].replace('int memcmp(', 'int actual_memcmp(')
compare = compare.replace('while (length >= sizeof(uint64_t)) {',
                          'while (length >= sizeof(uint64_t)) { ++words;')
compare = compare.replace('for (size_t index = 0U; index < length; ++index) {',
                          'for (size_t index = 0U; index < length; ++index) { ++bytes;')
code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static size_t words,bytes,cases,old_bytes;
''' + compare + predicate + r'''
static bool previous(const char *a,size_t n,const char *b,size_t m){
 if(!a||!b||n!=m)return false;
 for(size_t i=0;i<n;++i){++old_bytes;if(a[i]!=b[i])return false;}
 return true;
}
static void check(const char *a,size_t n,const char *b,size_t m){
 old_bytes=words=bytes=0;
 bool expected=previous(a,n,b,m);
 assert(vfs_path_range_equals(a,n,b,m)==expected);++cases;
 if(!a||!b||n!=m)assert(!words&&!bytes&&!old_bytes);
}
int main(void){
 char a[512],b[512];
 for(unsigned x=0;x<16;++x)for(unsigned y=0;y<16;++y)
 for(unsigned n=0;n<=256;++n){
  memset(a,0x81,sizeof(a));memset(b,0x81,sizeof(b));
  check(a+x,n,b+y,n);check(a+x,n,a+y,n);
  check(a+x,n,b+y,n+1);
  if(n){
   unsigned positions[]={0,n/2,n-1};
   for(unsigned i=0;i<3;++i){b[y+positions[i]]=0;check(a+x,n,b+y,n);b[y+positions[i]]=(char)0x81;}
  }
 }
 /* Embedded NULs and signed-char values remain ordinary range bytes. */
 for(unsigned n=1;n<=256;++n)for(unsigned value=0;value<256;++value){
  memset(a,0,sizeof(a));memset(b,0,sizeof(b));
  b[n/2]=(char)value;check(a,n,b,n);check(b,n,a,n);
 }
 check(0,0,0,0);check(0,0,a,0);check(a,0,0,0);
 memset(a,0x71,sizeof(a));memset(b,0x71,sizeof(b));check(a,256,b,256);
 assert(old_bytes==256&&words==32&&!bytes);
 long page=sysconf(_SC_PAGESIZE);assert(page>=256);
 char *left=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 char *right=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(left!=MAP_FAILED&&right!=MAP_FAILED);
 assert(!mprotect(left+page,(size_t)page,PROT_NONE));
 assert(!mprotect(right+page,(size_t)page,PROT_NONE));
 memset(left,0x91,(size_t)page);memset(right,0x91,(size_t)page);
 for(unsigned n=0;n<=256;++n){
  check(left+page-n,n,right+page-n,n);
  if(n){right[page-1]=0;check(left+page-n,n,right+page-n,n);right[page-1]=(char)0x91;}
 }
 /* Rejection must not touch even completely inaccessible inputs. */
 check(left+page,1,right+page,2);check(0,256,right+page,256);
 assert(!munmap(left,(size_t)page*2)&&!munmap(right,(size_t)page*2));
 printf("Path ranges: %zu exact byte-oracle alignment/overlap/NUL/value/null/length/guard cases; equal 256-byte paths use 32 word comparisons instead of 256 byte comparisons\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-path-range-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fno-builtin', '-mno-sse', '-mno-mmx',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
