"""真实 VFS、RTL8126 与 NVIDIA 固件加载流程的搜索、校验和所有权回归。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
io = (root/'kernel/fs/vfs/file_io.c').read_text()
load = io[io.index('kstatus_t vfs_load_file_kernel('):io.index('/* 整次写入')]
rtl_source = (root/'kernel/drivers/net/rtl8126.c').read_text()
rtl = rtl_source[rtl_source.index('static bool rtl8126_load_firmware('):rtl_source.index('static bool rtl8126_phy_wait_reset(')]
nv_source = (root/'kernel/drivers/gpu/nvidia/vendor_abi/nvabi_firmware.c').read_text()
nv = nv_source[nv_source.index('#define NVABI_FIRMWARE_MAGIC'):]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int kstatus_t;
enum {K_OK=0,K_EINVAL=-22,K_ENOMEM=-12,K_EIO=-5,K_ENOENT=-2,VFS_OPEN_READ=1};
enum {GOOD,MISSING,EMPTY,HUGE,OOM,READ_ERROR,SHORT_READ};
typedef struct {uint64_t size;} vnode_t;
typedef struct {vnode_t *vnode;} file_t;
static vnode_t node;
static file_t file={&node};
static const char *paths[4];
static unsigned plan[4],attempts,closes,reads,formats,runs,hashes,root_count;
static size_t limit;
static bool locked,bad_format,bad_run,bad_hash,handle_oom;
static void *data_memory,*handle_memory;
static unsigned family_for_hash;
static void vfs_vnode_lock(vnode_t *v){assert(v==&node&&!locked);locked=true;}
static void vfs_vnode_unlock(vnode_t *v){assert(v==&node&&locked);locked=false;}
static int vfs_open_kernel(const char *p,unsigned flags,unsigned create,file_t **out){
 assert(!locked&&!data_memory&&!handle_memory&&attempts<root_count);
 assert(!strcmp(p,paths[attempts])&&flags==VFS_OPEN_READ&&!create);
 unsigned fault=plan[attempts++];
 if(fault==MISSING)return K_ENOENT;
 node.size=fault==EMPTY?0:fault==HUGE?limit+1:64;*out=&file;return K_OK;
}
static void vfs_close(file_t *f){assert(f==&file&&!locked);++closes;}
static void *kmalloc(size_t n,unsigned flags){
 assert(!locked&&!data_memory&&!handle_memory&&n==64&&!flags);
 if(plan[attempts-1]==OOM)return NULL;
 data_memory=malloc(n);assert(data_memory);return data_memory;
}
static void *kzalloc(size_t n,unsigned flags){
 assert(data_memory&&!handle_memory&&!locked&&!flags);
 if(handle_oom)return NULL;
 handle_memory=calloc(1,n);assert(handle_memory);return handle_memory;
}
static void kfree(void *p){
 assert(p&&!locked);
 if(p==data_memory)data_memory=NULL;
 else {assert(p==handle_memory);handle_memory=NULL;}
 free(p);
}
static int vfs_read_kernel(file_t *f,void *p,size_t n,uint64_t *bytes){
 assert(f==&file&&p==data_memory&&n==64&&!locked);++reads;
 memset(p,0x5a,n);*bytes=plan[attempts-1]==SHORT_READ?n-1:n;
 return plan[attempts-1]==READ_ERROR?K_EIO:K_OK;
}
#define RTL8126_XID_2 0x649U
#define RTL8126_XID_3 0x64aU
#define RTL8126_FIRMWARE_MAX_BYTES (1024U*1024U)
typedef struct {uint32_t rtl_xid;} e1000_state_t;
static const uint32_t action=0x12345678;
static bool rtl8126_firmware_format(const uint8_t *p,size_t n,
 const uint32_t **actions,uint32_t *count){
 assert(p==data_memory&&n==64&&!locked);++formats;
 if(bad_format)return false;
 *actions=&action;*count=1;return true;
}
static bool rtl8126_firmware_run(e1000_state_t *s,const uint32_t *a,uint32_t n){
 assert(s&&a==&action&&n==1&&data_memory&&!locked);++runs;return !bad_run;
}
#define NV_API_CALL
typedef unsigned nv_firmware_type_t;
typedef unsigned nv_firmware_chip_family_t;
typedef unsigned nv_state_t;
typedef uint32_t NvU32;
typedef size_t UINTN;
#define NV_FIRMWARE_TYPE_GSP 0U
#define nvabi_memcpy memcpy
#define nvabi_memcmp memcmp
#define nvabi_strlen strlen
static const char *nv_firmware_for_chip_family(unsigned type,unsigned family){
 if(family>1)return NULL;
 if(type)return "gsp_log.bin";
 return family?"gsp_tu10x.bin":"gsp_ga10x.bin";
}
static void kernel_serial_printf_serial_only(const char *format,...){assert(format);}
static void kernel_serial_write_serial_only(const char *s){assert(s);}
static void sha256_compute(const void *p,UINTN n,uint8_t digest[32]);
'''
main = r'''
/* 单独注入 SHA 输出，只验证适配器的摘要匹配策略；不代替 SHA 算法自身测试。 */
static void sha256_compute(const void *p,UINTN n,uint8_t digest[32]){
 assert(p==data_memory&&n==64&&!locked);++hashes;
 memcpy(digest,family_for_hash?g_gsp_tu10x_sha256:g_gsp_ga10x_sha256,32);
 if(bad_hash)digest[0]^=1;
}
static void reset(void){
 assert(!data_memory&&!handle_memory&&!locked);
 attempts=closes=reads=formats=runs=hashes=0;
 bad_format=bad_run=bad_hash=handle_oom=false;
 for(unsigned i=0;i<4;++i)plan[i]=MISSING;
}
static void rtl_paths(unsigned family){
 static const char *const names[2][4]={
  {"/rtl_nic/rtl8126a-2.fw","/lib/firmware/rtl_nic/rtl8126a-2.fw",
   "/usr/lib/firmware/rtl_nic/rtl8126a-2.fw","/boot/firmware/rtl_nic/rtl8126a-2.fw"},
  {"/rtl_nic/rtl8126a-3.fw","/lib/firmware/rtl_nic/rtl8126a-3.fw",
   "/usr/lib/firmware/rtl_nic/rtl8126a-3.fw","/boot/firmware/rtl_nic/rtl8126a-3.fw"}};
 for(unsigned i=0;i<4;++i)paths[i]=names[family][i];
 root_count=4;limit=RTL8126_FIRMWARE_MAX_BYTES;
}
static void nv_paths(unsigned family){
 paths[0]=family?"/EFI/OS/firmware/gsp_tu10x.bin":"/EFI/OS/firmware/gsp_ga10x.bin";
 paths[1]=family?"/lib/firmware/gsp_tu10x.bin":"/lib/firmware/gsp_ga10x.bin";
 family_for_hash=family;root_count=2;limit=NVABI_FIRMWARE_MAX_BYTES;
}
int main(void){
 unsigned rtl_cases=0,nv_cases=0;
 for(unsigned family=0;family<2;++family)for(unsigned fault=MISSING;fault<=SHORT_READ;++fault)
  for(unsigned location=0;location<=4;++location){
   reset();rtl_paths(family);e1000_state_t state={family?RTL8126_XID_3:RTL8126_XID_2};
   for(unsigned i=0;i<location;++i)plan[i]=fault;
   if(location<4)plan[location]=GOOD;
   assert(rtl8126_load_firmware(&state)==(location<4));
   assert(attempts==(location<4?location+1:4)&&formats==(location<4)&&runs==formats);
   assert(!data_memory&&!handle_memory&&!locked);++rtl_cases;
  }
 for(unsigned failure=0;failure<2;++failure){
  reset();rtl_paths(0);plan[0]=GOOD;bad_format=!failure;bad_run=!!failure;
  e1000_state_t state={RTL8126_XID_2};assert(!rtl8126_load_firmware(&state));
  assert(attempts==1&&formats==1&&runs==failure&&!data_memory);
 }
 reset();assert(!rtl8126_load_firmware(NULL)&&!attempts);
 for(unsigned family=0;family<2;++family)for(unsigned fault=MISSING;fault<=SHORT_READ;++fault)
  for(unsigned location=0;location<=2;++location){
   reset();nv_paths(family);
   for(unsigned i=0;i<location;++i)plan[i]=fault;
   if(location<2)plan[location]=GOOD;
   const void *buffer=(void*)1;NvU32 size=99;
   const void *h=nv_get_firmware(NULL,NV_FIRMWARE_TYPE_GSP,family,&buffer,&size);
   assert(!!h==(location<2)&&attempts==(location<2?location+1:2));
   if(h){
    const nvabi_firmware_handle_t *handle=h;
    assert(buffer==data_memory&&size==64&&hashes==1&&handle->magic==NVABI_FIRMWARE_MAGIC);
    assert(!strcmp(handle->path,paths[location])&&handle->family==family&&handle->type==NV_FIRMWARE_TYPE_GSP);
    nv_put_firmware(h);
   } else assert(!buffer&&!size&&!hashes);
   assert(!data_memory&&!handle_memory&&!locked);++nv_cases;
  }
 for(unsigned failure=0;failure<2;++failure){
  reset();nv_paths(0);plan[0]=plan[1]=GOOD;bad_hash=!failure;handle_oom=!!failure;
  const void *buffer=(void*)1;NvU32 size=99;
  assert(!nv_get_firmware(NULL,0,0,&buffer,&size));
  assert(attempts==1&&hashes==1&&!buffer&&!size&&!data_memory&&!handle_memory);
 }
 for(unsigned invalid=0;invalid<4;++invalid){
  reset();nv_paths(0);const void *buffer=(void*)1;NvU32 size=99;
  assert(!nv_get_firmware(NULL,invalid==0?1:0,invalid==1?2:0,
          invalid==2?NULL:&buffer,invalid==3?NULL:&size));
  assert(!attempts);
  if(invalid!=2)assert(!buffer);
  if(invalid!=3)assert(!size);
 }
 nv_put_firmware(NULL);
 printf("Firmware loading: RTL=%u NVIDIA=%u search/failure cases; format/run/hash rejection and handle lifetime PASS\n",rtl_cases,nv_cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-firmware-') as directory:
    binary = Path(directory)/'test'
    code = prefix+load+rtl+nv+main
    def run(source, negative=False):
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-x','c','-','-o',str(binary)],input=source,text=True,check=True)
        result = subprocess.run([str(binary)],capture_output=True,text=True,timeout=30,
                                preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        if negative:
            assert result.returncode and 'Assertion' in result.stderr, result.stderr
            print('Negative control rejected: bypassing the NVIDIA firmware hash check')
        else:
            assert result.returncode == 0,result.stderr
            print(result.stdout.strip())
    run(code)
    guard = 'if (!nvabi_firmware_hash_matches(digest, expected_hash))'
    assert code.count(guard)==1
    run(code.replace(guard,'if (!nvabi_firmware_hash_matches(digest, expected_hash) && false)'),True)
