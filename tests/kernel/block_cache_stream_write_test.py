"""真实块缓存连续写入：覆盖脏/干净缓存及设备部分失败后的读取和重试。"""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
program=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/block_cache.h>
static unsigned char disk[256U*4096U],data[OS_BLOCK_CACHE_MAX_WRITE_BYTES],out[OS_BLOCK_CACHE_MAX_WRITE_BYTES],one[4096];
static unsigned writes,fail_call;
static BOOLEAN read_disk(VOID *ctx,UINT64 lba,UINT32 count,VOID *buffer){
 OS_BLOCK_DEVICE *d=ctx;assert(lba+count<=d->BlockCount);
 memcpy(buffer,disk+lba*d->BlockSize,(size_t)count*d->BlockSize);return 1;
}
static BOOLEAN write_disk(VOID *ctx,UINT64 lba,UINT32 count,const VOID *buffer){
 OS_BLOCK_DEVICE *d=ctx;assert(lba+count<=d->BlockCount);++writes;
 size_t n=(size_t)count*d->BlockSize;
 if(writes==fail_call){memcpy(disk+lba*d->BlockSize,buffer,n/2);return 0;}
 memcpy(disk+lba*d->BlockSize,buffer,n);return 1;
}
static void setup(OS_BLOCK_CACHE *cache,OS_BLOCK_DEVICE *d,unsigned bps){
 memset(cache,0,sizeof(*cache));memset(disk,0x11,sizeof(disk));
 *d=(OS_BLOCK_DEVICE){.BlockSize=bps,.BlockCount=256,.Registered=1,.Read=read_disk,.Write=write_disk};
 d->Context=d;writes=fail_call=0;assert(block_cache_init(cache,d));
 memset(one,0x22,bps);assert(block_cache_write(cache,25,one));
 memset(one,0x33,bps);assert(block_cache_write(cache,26,one));
 memset(one,0x44,bps);assert(block_cache_write(cache,90,one));
 assert(block_cache_writeback(cache));
 memset(one,0x55,bps);assert(block_cache_write(cache,25,one));
 memset(one,0x66,bps);assert(block_cache_write(cache,90,one));
 writes=0;
}
static void check_valid(OS_BLOCK_CACHE *cache,UINT64 lba,bool valid,bool dirty){
 bool found=false;for(unsigned i=0;i<OS_BLOCK_CACHE_ENTRY_COUNT;i++){
  OS_BLOCK_CACHE_ENTRY *entry=&cache->Entries[i];
  if(entry->Valid&&entry->Lba==lba){assert(!found);found=true;assert(!!entry->Dirty==dirty);}
 }assert(found==valid);
}
int main(void){
 OS_BLOCK_CACHE cache;OS_BLOCK_DEVICE device;
 for(unsigned bps=512;bps<=4096;bps*=2){
  unsigned count=64;size_t n=(size_t)bps*count;
  for(size_t i=0;i<n;i++)data[i]=(unsigned char)(i*31U+(i>>9));
  setup(&cache,&device,bps);
  assert(block_cache_write_range(&cache,20,count,data));
  assert(writes==2&&!memcmp(disk+20*bps,data,n));
  check_valid(&cache,25,false,false);check_valid(&cache,26,false,false);
  check_valid(&cache,90,true,true);
  assert(block_cache_read_range(&cache,20,count,out)&&!memcmp(out,data,n));
  assert(block_cache_flush(&cache)&&disk[90*bps]==0x66);
  assert(!block_cache_write_range(&cache,20,0,data));
  assert(!block_cache_write_range(&cache,250,7,data));
  assert(!block_cache_write_range(&cache,UINT64_MAX,1,data));
  assert(!block_cache_write_range(&cache,20,OS_BLOCK_CACHE_MAX_WRITE_BYTES/bps+1,data));
  assert(!block_cache_write_range(&cache,20,1,0));
  /* 第一处失败发生在旧脏扇区回写：该脏数据仍在缓存，随后可重试。 */
  setup(&cache,&device,bps);fail_call=1;
  assert(!block_cache_write_range(&cache,20,count,data));
  check_valid(&cache,25,true,true);check_valid(&cache,26,true,false);check_valid(&cache,90,true,true);
  fail_call=0;assert(block_cache_write_range(&cache,20,count,data));
  assert(!memcmp(disk+20*bps,data,n));
  /* 第二处失败发生在直写且设备已写半数：范围缓存必须失效，读取与设备一致。 */
  setup(&cache,&device,bps);fail_call=2;
  assert(!block_cache_write_range(&cache,20,count,data));
  check_valid(&cache,25,false,false);check_valid(&cache,26,false,false);check_valid(&cache,90,true,true);
  assert(!memcmp(disk+20*bps,data,n/2));
  assert(block_cache_read_range(&cache,20,count,out));
  assert(!memcmp(out,disk+20*bps,n));
  fail_call=0;assert(block_cache_write_range(&cache,20,count,data));
  assert(block_cache_read_range(&cache,20,count,out)&&!memcmp(out,data,n));
  assert(block_cache_flush(&cache)&&disk[90*bps]==0x66);
  /* 无覆盖缓存时，64 个连续扇区只发一次设备写入。 */
  setup(&cache,&device,bps);writes=0;
  assert(block_cache_write_range(&cache,100,count,data)&&writes==1);
 }
 puts("Block streaming write: 512-4096 byte sectors, dirty/clean overlap, unrelated dirty data, partial device failures, retry and bounds PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='cache-stream-write-') as directory:
 binary=str(Path(directory)/'test')
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
  '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),
  '-x','c','-',str(root/'kernel/block/bio.c'),str(root/'kernel/fs/pagecache/cache.c'),
  '-o',binary],input=program,text=True,check=True)
 subprocess.run([binary],check=True)
