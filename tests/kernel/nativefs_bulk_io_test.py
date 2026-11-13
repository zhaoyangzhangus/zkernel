"""Whole production journal/LiteFS with actual kernel bulk string operations."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/litefs.h>
enum {BLOCKS=64, CAPACITY=LITEFS_FILE_EXTENT_BLOCKS*JOURNAL_BLOCK_SIZE};
static unsigned char disk[BLOCKS][JOURNAL_BLOCK_SIZE],durable[BLOCKS][JOURNAL_BLOCK_SIZE];
static unsigned reads,writes,flushes;
static kstatus_t read_block(void *ctx,uint64_t block,void *out,size_t length) {
 (void)ctx;assert(block<BLOCKS && length==JOURNAL_BLOCK_SIZE);++reads;
 memcpy(out,disk[block],length);return K_OK;
}
static kstatus_t write_block(void *ctx,uint64_t block,const void *in,size_t length) {
 (void)ctx;assert(block<BLOCKS && length==JOURNAL_BLOCK_SIZE);++writes;
 memcpy(disk[block],in,length);return K_OK;
}
static kstatus_t flush(void *ctx) {
 (void)ctx;++flushes;memcpy(durable,disk,sizeof(disk));return K_OK;
}
int main(void) {
 assert(journal_self_test());
 journal_storage_t storage={.read=read_block,.write=write_block,.flush=flush};
 litefs_t fs,recovered;
 unsigned char input[CAPACITY],expected[CAPACITY],out[CAPACITY];
 unsigned rng=3;for(unsigned i=0;i<CAPACITY;++i){rng=rng*1664525U+1013904223U;input[i]=rng>>24;}
 unsigned cases=0;
 for(unsigned offset=0;offset<JOURNAL_BLOCK_SIZE;offset+=7) {
  const unsigned lengths[]={1,17,63,64,65,JOURNAL_BLOCK_SIZE-1,JOURNAL_BLOCK_SIZE,JOURNAL_BLOCK_SIZE+1};
  for(unsigned l=0;l<sizeof(lengths)/sizeof(lengths[0]);++l) {
   unsigned n=lengths[l];uint32_t inode;uint64_t bytes;
   memset(disk,0x69,sizeof(disk));memset(durable,0x69,sizeof(durable));
   assert(litefs_format(&storage,BLOCKS)==K_OK);
   assert(litefs_mount(&fs,&storage,BLOCKS)==K_OK);
   assert(litefs_create_file(&fs,"bulk",0100666,&inode)==K_OK);
   memset(expected,0,sizeof(expected));memcpy(expected+offset,input,n);
   /* LiteFS 有意拒绝稀疏写入。通过其常规 API 创建零填充前缀，不假定它支持扩展空洞。 */
   if(offset) {
    assert(litefs_write_file(&fs,inode,offset,input,n,&bytes)==K_EINVAL);
    assert(litefs_write_file(&fs,inode,0,expected,offset,&bytes)==K_OK && bytes==offset);
   }
   assert(litefs_write_file(&fs,inode,offset,input,n,&bytes)==K_OK && bytes==n);
   assert(litefs_fsync(&fs)==K_OK);
   /* 丢弃易失状态，并重新挂载实际元数据/日志。 */
   memcpy(disk,durable,sizeof(disk));
   assert(litefs_mount(&recovered,&storage,BLOCKS)==K_OK);
   memset(out,0x75,sizeof(out));
   assert(litefs_read_file(&recovered,inode,0,out,sizeof(out),&bytes)==K_OK && bytes==offset+n);
   assert(!memcmp(out,expected,bytes));
   for(unsigned j=(unsigned)bytes;j<sizeof(out);++j)assert(out[j]==0x75);
   ++cases;
  }
 }
 /* 覆盖整个范围，包括最后一个允许访问的字节。 */
 uint32_t inode=0;uint64_t bytes;
 assert(litefs_write_file(&recovered,inode,0,input,sizeof(input),&bytes)==K_OK && bytes==sizeof(input));
 assert(litefs_fsync(&recovered)==K_OK);memcpy(disk,durable,sizeof(disk));
 assert(litefs_mount(&fs,&storage,BLOCKS)==K_OK);
 assert(litefs_read_file(&fs,inode,0,out,sizeof(out),&bytes)==K_OK && bytes==sizeof(out));
 assert(!memcmp(out,input,sizeof(out)));
 /* 持久化超级块仍受校验和保护。 */
 disk[0][7]^=1;assert(litefs_mount(&recovered,&storage,BLOCKS)!=K_OK);
 printf("Production journal self-test and LiteFS: %u unaligned writes, zero prefixes/sparse rejection, fsync/remount, full extent and checksum corruption passed (%u reads, %u writes, %u flushes)\n",cases,reads,writes,flushes);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-nativefs-bulk-') as directory:
    path = Path(directory)
    objects = []
    rename = ['-Dmemcpy=liteos_memcpy','-Dmemmove=liteos_memmove',
              '-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp']
    for name, source in [('journal','kernel/fs/nativefs/journal.c'),
                         ('litefs','kernel/fs/nativefs/litefs.c'),
                         ('string','kernel/string.c')]:
        obj = str(path/(name+'.o'));objects.append(obj)
        subprocess.run(['cc','-std=c11','-O2','-ffunction-sections','-fdata-sections',
                        '-fno-builtin','-mno-sse','-mno-mmx','-fsanitize=address,undefined',
                        '-fno-pie','-I'+str(root/'include'),
                        *rename,'-c',str(root/source),'-o',obj],check=True)
    binary = str(path/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-Wl,--gc-sections',
                    '-x','c','-','-x','none',*objects,'-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
