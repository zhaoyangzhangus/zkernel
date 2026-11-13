"""Actual memcmp: unsigned first-difference semantics and exact read boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/string.c').read_text()
helper = source[source.index('int memcmp('):].replace('int memcmp(', 'int actual_memcmp(')
assert helper.count('while (length >= sizeof(uint64_t))') == 1
helper = helper.replace('while (length >= sizeof(uint64_t)) {',
                        'while (length >= sizeof(uint64_t)) { ++words;')
code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static size_t words,cases;
''' + helper + r'''
static void check(const unsigned char *a,const unsigned char *b,size_t n) {
    int expected=0;size_t first=n;
    for(size_t i=0;i<n;++i)if(a[i]!=b[i]) {expected=a[i]<b[i]?-1:1;first=i;break;}
    words=0;assert(actual_memcmp(a,b,n)==expected);
    size_t expected_words=n/8;
    if(first<n/8*8)expected_words=first/8+1;
    assert(words==expected_words);++cases;
}
int main(void) {
    check(0,0,0);
    unsigned char a[256],b[256];
    for(unsigned x=0;x<16;++x)for(unsigned y=0;y<16;++y) {
        memset(a,0x81,sizeof(a));memset(b,0x81,sizeof(b));
        for(unsigned n=0;n<=128;++n) {
            check(a+x,b+y,n);check(a+x,a+y,n);
            for(unsigned i=0;i<n;++i) {
                b[y+i]=255;check(a+x,b+y,n);check(b+y,a+x,n);
                /* 后面相反方向的差异不能改变结果。 */
                if(i+1<n) {b[y+i+1]=0;check(a+x,b+y,n);b[y+i+1]=0x81;}
                b[y+i]=0x81;
            }
        }
    }
    const size_t big=4*1024*1024;
    unsigned char *large=malloc(big),*copy=malloc(big);assert(large && copy);
    uint32_t seed=7;
    for(size_t i=0;i<big;++i) {seed=seed*1664525U+1013904223U;large[i]=(unsigned char)(seed>>24);}
    memcpy(copy,large,big);check(large,copy,big);assert(words==big/8);
    for(unsigned step=0;step<10000;++step) {
        seed=seed*1664525U+1013904223U;size_t n=seed%65536;
        seed=seed*1664525U+1013904223U;size_t mismatch=n?seed%n:0;
        if(n)copy[mismatch]^=128;
        check(large,copy,n);if(n)copy[mismatch]^=128;
    }
    free(copy);free(large);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    unsigned char *guard=mmap(0,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(guard!=MAP_FAILED && !mprotect(guard+page,page,PROT_READ|PROT_WRITE));
    unsigned char *other=malloc(page);assert(other);memset(other,0x81,page);memset(guard+page,0x81,page);
    for(size_t n=0;n<=page;++n) {
        check(guard+2*page-n,other,n);check(guard+page,other,n);
        if(n) {guard[2*page-1]=0;check(guard+2*page-n,other,n);guard[2*page-1]=0x81;}
    }
    free(other);assert(!munmap(guard,page*3));
    printf("memcmp: %zu unsigned first-difference, all alignment/overlap, equal-word skip, random and guard-page cases passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-memcmp-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fno-builtin','-mno-sse','-mno-mmx',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
