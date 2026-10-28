"""执行真实用户读取：动态批次、内存不足回退和错误进度必须同时正确。"""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/file_io.c').read_text()
body = source[source.index('static kstatus_t vfs_read_backend_chunk('):
              source.index('kstatus_t vfs_read_kernel(')]
program = r'''
#include <assert.h>
/* 非缓存测试只桩化读取合并；映射一致性由单独的真实代码回归测试覆盖。 */
#define vfs_page_cache_present(vnode) ((void)(vnode), false)
#define K_ENOMEM (-12)
#define vfs_page_cache_buffer_alias(vnode,buffer,length) \
    ((void)(vnode),(void)(buffer),(void)(length),false)
#define vfs_backend_consistent_read_lock(backend) ((void)(backend), K_OK)
#define vfs_backend_consistent_read_unlock(backend) ((void)(backend))
#define vfs_page_cache_overlay_read(vnode,offset,buffer,length) \
    ((void)(vnode),(void)(offset),(void)(buffer),(void)(length),K_OK)
/* 单句柄测试只桩化偏移锁；共享句柄并发由专门的回归测试覆盖。 */
#define vfs_file_position_init(file) ((void)(file))
#define vfs_file_position_lock(file) ((void)(file), K_OK)
#define vfs_file_position_unlock(file) ((void)(file))
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __user
#define PAGE_SIZE 4096U
enum {K_OK=0,K_EINVAL=-22,K_EIO=-5,K_EACCES=-13,KOBJECT_TYPE_FILE=1,VFS_OPEN_READ=1};
typedef int kstatus_t;
typedef struct {void *context;uint64_t size;
 int (*read)(void *,uint64_t,void *,size_t,uint64_t *);} vfs_backend_node_t;
typedef struct {void *fs_private;} vnode_t;
typedef struct {struct {unsigned type;} object;vnode_t *vnode;unsigned rights;uint64_t position;} file_t;
static unsigned held,calls,copies,allocations,live,yields,stop,mode;
static bool oom,shrink;
static uint64_t initial;
static size_t capacity;
static unsigned char disk[3*65536+200],output[sizeof(disk)+2];
static void *kmalloc(size_t n,unsigned flags){
 (void)flags;assert(!held&&n>=16384&&n<=65536);++allocations;
 if(oom)return NULL;
 void *p=malloc(n);assert(p);++live;return p;
}
static void kfree(void *p){assert(p&&live&&!held);--live;free(p);}
static bool x86_user_range_valid(const void *p,size_t n){return p||!n;}
static void vfs_backend_lock(vfs_backend_node_t *b){
 assert(!held);held=1;if(shrink&&calls)b->size=initial+capacity;
}
static void vfs_backend_unlock(vfs_backend_node_t *b){(void)b;assert(held);held=0;}
static void vfs_read_kernel_yield(void){assert(!held);++yields;}
static int backend_read(void *c,uint64_t pos,void *buf,size_t n,uint64_t *got){
 (void)c;assert(held&&n<=capacity&&pos+n<=sizeof(disk));unsigned call=calls++;
 *got=n;
 if(call==stop){
  if(mode==1)return K_EIO;
  if(mode==2){*got=n+1;return K_OK;}
  if(mode==3)*got=n/2;
  if(mode==4)*got=0;
 }
 memcpy(buf,disk+pos,(size_t)*got);return K_OK;
}
static int copy_to_user(void *d,const void *s,size_t n){
 assert(!held);unsigned call=copies++;if(call==stop&&mode==5)return K_EACCES;
 memcpy(d,s,n);return K_OK;
}
''' + body + r'''
int main(void){
 for(unsigned i=0;i<sizeof(disk);++i)disk[i]=(unsigned char)(i*17+5);
 const size_t sizes[]={0,1,4096,9000,16383,16384,65535,65536,65537,2*65536+71};
 unsigned cases=0;
 for(unsigned o=0;o<2;++o)for(unsigned n=0;n<sizeof(sizes)/sizeof(*sizes);++n)
 for(unsigned s=0;s<4;++s)for(unsigned m=0;m<6;++m){
  assert(!live);oom=o;stop=s;mode=m;initial=13;shrink=false;
  calls=copies=allocations=yields=held=0;memset(output,0xcc,sizeof(output));
  size_t length=sizes[n];capacity=length>=16384&&!oom?(length<65536?length:65536):4096;
  vfs_backend_node_t b={NULL,sizeof(disk),backend_read};vnode_t v={&b};
  file_t f={{KOBJECT_TYPE_FILE},&v,VFS_OPEN_READ,initial};
  size_t expected=0;unsigned count=0;int status=K_OK;
  while(expected<length){
   size_t chunk=length-expected;if(chunk>capacity)chunk=capacity;
   if(count++==stop){
    if(mode==1||mode==2){status=K_EIO;break;}
    if(mode==5){status=K_EACCES;break;}
    if(mode==4)break;
    if(mode==3){expected+=chunk/2;break;}
   }
   expected+=chunk;
  }
  uint64_t bytes=123;assert(vfs_read(&f,output+1,length,&bytes)==status);
  assert(bytes==expected&&f.position==initial+expected&&!live&&!held&&calls==count);
  assert(allocations==(length>=16384));
  assert(!memcmp(output+1,disk+initial,expected));
  for(size_t i=expected+1;i<sizeof(output);++i)assert(output[i]==0xcc);
  assert(output[0]==0xcc);++cases;
 }
 /* 连续读 128 KiB 从 32 次后端调用降为 2 次；并发缩短 EOF 不再读取旧范围。 */
 oom=false;mode=0;stop=99;initial=0;capacity=65536;shrink=true;
 calls=copies=allocations=yields=0;
 vfs_backend_node_t b={NULL,sizeof(disk),backend_read};vnode_t v={&b};
 file_t f={{KOBJECT_TYPE_FILE},&v,VFS_OPEN_READ,0};uint64_t bytes;
 assert(vfs_read(&f,output,2*65536,&bytes)==K_OK&&bytes==65536&&calls==1&&!live);
 shrink=false;b.size=sizeof(disk);f.position=0;calls=0;
 assert(vfs_read(&f,output,2*65536,&bytes)==K_OK&&bytes==2*65536&&calls==2&&!live);
 printf("User read batches: %u cases; bounded allocation, OOM fallback, exact bytes, errors, EOF shrink, 128KiB in 2 backend calls PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-user-read-batch-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    old = 'if (chunk > capacity) chunk = capacity;'
    assert old in program
    subprocess.run(command, input=program.replace(old,
        '(void)capacity; if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;'), text=True, check=True)
    result = subprocess.run([binary], capture_output=True, text=True, timeout=30,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
    print('Negative control rejected: page-at-a-time user read')
