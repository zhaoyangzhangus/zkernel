"""真实只读 GPU 查询：混合设备枚举、字段校验和用户复制失败。"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/syscall/graphics.c').read_text();a=s.index('int64_t syscall_gpu_get_info(');b=s.index('\n}\n',a)+3
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <uapi/gpu.h>
#include <uapi/syscall.h>
#define __user
#define PCI_MAX_DEVICES 8U
typedef int kstatus_t;
enum {K_OK=0,K_EINVAL=-22,K_ENOENT=-2,K_EAGAIN=-11,K_EFAULT=-14};
enum {DEVICE_DISCOVERED,DEVICE_ENUMERATED,DEVICE_BOUND,DEVICE_ACTIVE,DEVICE_SUSPENDED,DEVICE_RECOVERING,DEVICE_REMOVING,DEVICE_REMOVED,DEVICE_FAILED};
typedef struct {struct {atomic_uint state;} device;uint32_t class_code,vendor_id,device_id,segment,bus,slot,function;} pci_device_t;
typedef struct {bool initialized;uint32_t device_count;pci_device_t devices[8];} pci_host_t;
static pci_host_t host;static unsigned copy_fail;
static uint64_t nv_rm_driver_memory_bytes(const pci_device_t *device){return device==&host.devices[3]?12ULL*1024*1024*1024:0;}
static const pci_host_t *pci_current_host(void){return &host;}
static bool versioned_header_valid(const os_versioned_header_t *h,unsigned size){return h->size==size&&h->version==OS_SYSCALL_ABI_VERSION;}
static int copy_from_user(void *d,const void *s,size_t n){if(copy_fail==1)return K_EFAULT;memcpy(d,s,n);return K_OK;}
static int copy_to_user(void *d,const void *s,size_t n){if(copy_fail==2)return K_EFAULT;memcpy(d,s,n);return K_OK;}
'''+s[a:b]+r'''
int main(void){
 host.initialized=true;host.device_count=4;
 host.devices[0]=(pci_device_t){.class_code=2,.vendor_id=0x8086};
 host.devices[1]=(pci_device_t){.class_code=3,.vendor_id=0x1234,.device_id=0x1111};
 host.devices[2]=(pci_device_t){.class_code=4,.vendor_id=0x10de};
 host.devices[3]=(pci_device_t){.class_code=3,.vendor_id=0x10de,.device_id=0x2f04,.bus=1,.slot=2,.function=3};
 atomic_store(&host.devices[3].device.state,DEVICE_ACTIVE);
 for(unsigned index=0;index<2;++index){
  os_gpu_info_t info={.hdr={sizeof(info),OS_SYSCALL_ABI_VERSION},.index=index};
  assert(syscall_gpu_get_info((uintptr_t)&info,0,0,0,0,0)==K_OK);
  assert(info.index==index&&info.count==2&&!info.reserved);
  assert(info.vram_bytes==(index?12ULL*1024*1024*1024:0));
  if(index)assert(info.vendor_id==0x10de&&info.device_id==0x2f04&&info.bus==1&&info.slot==2&&info.function==3&&info.state==OS_GPU_DEVICE_ACTIVE);
  else assert(info.vendor_id==0x1234&&info.device_id==0x1111&&info.state==OS_GPU_DEVICE_DISCOVERED);
 }
 os_gpu_info_t info={.hdr={sizeof(info),OS_SYSCALL_ABI_VERSION},.index=2},before=info;
 assert(syscall_gpu_get_info((uintptr_t)&info,0,0,0,0,0)==K_ENOENT&&!memcmp(&info,&before,sizeof(info)));
 info.index=0;info.reserved=1;assert(syscall_gpu_get_info((uintptr_t)&info,0,0,0,0,0)==K_EINVAL);
 info.reserved=0;--info.hdr.size;assert(syscall_gpu_get_info((uintptr_t)&info,0,0,0,0,0)==K_EINVAL);++info.hdr.size;
 host.initialized=false;assert(syscall_gpu_get_info((uintptr_t)&info,0,0,0,0,0)==K_EAGAIN);host.initialized=true;
 for(copy_fail=1;copy_fail<=2;++copy_fail)assert(syscall_gpu_get_info((uintptr_t)&info,0,0,0,0,0)==K_EFAULT);
 puts("GPU info: mixed PCI classes, multiple cards, cached state, exact scalar output and failure rollback PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-info-') as directory:
 p=Path(directory);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',
  '-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
