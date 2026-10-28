#!/usr/bin/env python3
"""Compare production prefetch policies on interleaved metadata/data reads."""
import pathlib
import subprocess
import tempfile

root=pathlib.Path(__file__).resolve().parents[2]
code=r'''
#include <stdio.h>
#include <string.h>
#include <kernel/block_cache.h>
static unsigned reads;
static BOOLEAN read_disk(VOID *ctx,UINT64 lba,UINT32 n,VOID *out) {
 (void)ctx; ++reads;
 for (UINT32 i=0;i<n;++i) memset((UINT8*)out+512*i,(int)((lba+i)&255),512);
 return 1;
}
static BOOLEAN write_disk(VOID *ctx,UINT64 lba,UINT32 n,const VOID *in) {
 (void)ctx;(void)lba;(void)n;(void)in;return 1;
}
int main(int argc, char **argv) {
 (void)argv;
 OS_BLOCK_DEVICE d={0}; OS_BLOCK_CACHE cache={0}; UINT8 b[512];
 d.BlockSize=512;d.BlockCount=2048;d.Read=read_disk;d.Write=write_disk;d.Registered=1;
 if(!block_cache_init(&cache,&d))return 1;
 if(argc>2) {
  for(unsigned i=0;i<32;++i) {
   for(unsigned m=0;m<3;++m)
    if(!block_cache_read(&cache,m*16,b)||b[0]!=m*16)return 5;
   UINT64 lba=128+16*i;
   if(!block_cache_read(&cache,lba,b)||b[0]!=(lba&255))return 6;
  }
 } else if(argc>1) {
  for(unsigned i=0;i<480;++i)
   if(!block_cache_read(&cache,i,b)||b[0]!=(i&255))return 4;
 } else for(unsigned i=0;i<32;++i) {
  if(!block_cache_read(&cache,0,b)||b[0]!=0)return 2;
  UINT64 lba=32+16*i;
  if(!block_cache_read(&cache,lba,b)||b[0]!=(lba&255))return 3;
 }
 printf("%u\n",reads);
 return !block_cache_destroy(&cache);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-cache-perf-') as directory:
    results=[]
    sequential=[]
    metadata=[]
    for old in (True,False):
        binary=str(pathlib.Path(directory)/('old' if old else 'new'))
        source = root/'kernel/fs/pagecache/cache.c'
        if old:
            baseline = source.read_text()
            start = baseline.index('        UINT32 hot_count = 0U;')
            end = baseline.index('        while (reserved_count < prefetch_limit', start)
            baseline = baseline[:start] + baseline[end:]
            baseline = baseline.replace('entry->LastUse = 0U;',
                                        'entry->LastUse = cache->Clock;')
            source = pathlib.Path(directory)/'baseline.c'
            source.write_text(baseline)
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',
                        '-I'+str(root/'include'),
                        *(['-DOS_BLOCK_CACHE_READ_AHEAD_BLOCKS=16U'] if old else []),
                        '-x','c','-',str(root/'kernel/block/bio.c'),
                        str(source),'-o',binary],
                       input=code,text=True,check=True)
        results.append(int(subprocess.check_output([binary],text=True)))
        sequential.append(int(subprocess.check_output([binary,'sequential'],text=True)))
        metadata.append(int(subprocess.check_output([binary,'metadata','three'],text=True)))
    assert results[1] == 33, results
    assert results[0] == 64, results
    assert sequential == [30,32], sequential
    assert metadata[0] == 128, metadata
    print(f'block-cache: identical reads verified; device reads {results[0]} -> {results[1]}')
    print(f'sequential: device reads {sequential[0]} -> {sequential[1]} (480 sectors)')
    print(f'three hot metadata sectors: device reads {metadata[0]} -> {metadata[1]}')
    assert metadata[1] == 35, metadata
