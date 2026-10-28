"""真实块缓存的连续脏块合并、淘汰和部分写入失败恢复。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/block_cache.h>
static unsigned char media[256U*4096U], expected[sizeof(media)], data[4096];
static unsigned writes,flushes,fail_write,largest,blocks;
static BOOLEAN read_disk(VOID *ctx,UINT64 lba,UINT32 n,VOID *out) {
    OS_BLOCK_DEVICE *d=ctx;
    memcpy(out,media+lba*d->BlockSize,(size_t)n*d->BlockSize);return 1;
}
static BOOLEAN write_disk(VOID *ctx,UINT64 lba,UINT32 n,const VOID *in) {
    OS_BLOCK_DEVICE *d=ctx;
    assert(n && n*d->BlockSize<=4096 && lba+n<=d->BlockCount);
    ++writes;if(n>largest)largest=n;blocks+=n;
    if(writes==fail_write) {
        /* 设备允许先写一部分再报错，脏缓存必须仍能恢复整个请求。 */
        memcpy(media+lba*d->BlockSize,in,(size_t)n*d->BlockSize/2);return 0;
    }
    memcpy(media+lba*d->BlockSize,in,(size_t)n*d->BlockSize);return 1;
}
static BOOLEAN flush_disk(VOID *ctx) {(void)ctx;++flushes;return 1;}
static void fixture(OS_BLOCK_CACHE *cache,OS_BLOCK_DEVICE *d,unsigned bps,unsigned layout) {
    memset(cache,0,sizeof(*cache));memset(media,0x23,sizeof(media));memcpy(expected,media,sizeof(media));
    *d=(OS_BLOCK_DEVICE){.Registered=1,.BlockSize=bps,.BlockCount=256,
        .Read=read_disk,.Write=write_disk,.Flush=flush_disk};d->Context=d;
    writes=flushes=fail_write=largest=blocks=0;
    assert(block_cache_init(cache,d));
    for(unsigned i=0;i<16;++i) {
        unsigned lba=layout==0?32+i:layout==1?32+2*i:32+(i*7)%16;
        memset(data,(int)(i+1),bps);
        assert(block_cache_write(cache,lba,data));
        memcpy(expected+lba*bps,data,bps);
    }
    assert(!writes);
}
int main(void) {
    OS_BLOCK_CACHE cache;OS_BLOCK_DEVICE device;
    for(unsigned bps=512;bps<=4096;bps*=2) {
        for(unsigned layout=0;layout<3;++layout) {
            fixture(&cache,&device,bps,layout);
            assert(block_cache_writeback(&cache) && !flushes);
            assert(!memcmp(media,expected,sizeof(media)) && blocks==16);
            if(layout==0)assert(writes==16/(4096/bps));
            if(layout==1)assert(writes==16);
            for(unsigned i=0;i<16;++i)assert(!cache.Entries[i].Dirty);
            printf("%u-byte sectors, layout %u: 16 dirty blocks, %u device writes\n",bps,layout,writes);
            assert(block_cache_flush(&cache) && flushes==1);
            unsigned before=writes;assert(block_cache_flush(&cache) && writes==before && flushes==2);
        }
        unsigned requests=16/(4096/bps);
        for(unsigned failure=1;failure<=requests;++failure) {
            fixture(&cache,&device,bps,0);fail_write=failure;
            assert(!block_cache_flush(&cache) && writes==failure && !flushes);
            unsigned dirty=0;for(unsigned i=0;i<16;++i)dirty+=cache.Entries[i].Dirty!=0;
            assert(dirty==16-(failure-1)*(4096/bps));
            fail_write=0;assert(block_cache_flush(&cache) && flushes==1);
            assert(!memcmp(media,expected,sizeof(media)));
        }
        for(unsigned fail=0;fail<2;++fail) {
            fixture(&cache,&device,bps,0);fail_write=fail?1:0;
            memset(data,0xee,bps);
            BOOLEAN ok=block_cache_write(&cache,100,data);
            assert(ok==!fail && writes==1);
            if(!fail)memcpy(expected+100*bps,data,bps);
            else {
                assert(cache.Entries[0].Valid && cache.Entries[0].Dirty && cache.Entries[0].Lba==32);
                for(unsigned i=0;i<16;++i)assert(cache.Entries[i].Dirty);
            }
            fail_write=0;assert(block_cache_flush(&cache));
            assert(!memcmp(media,expected,sizeof(media)));
        }
    }
    puts("Block writeback: contiguous/scattered/shuffled slots, bounded requests, eviction and every partial-write failure/retry PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='cache-writeback-batch-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),
        '-x','c','-',str(root/'kernel/block/bio.c'),str(root/'kernel/fs/pagecache/cache.c'),
        '-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=20)
