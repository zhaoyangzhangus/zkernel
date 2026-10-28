"""验证外观接口边界和多网卡枚举，执行真实系统调用函数。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
network = (root / 'kernel/syscall/network.c').read_text().split('int64_t syscall_net_enum_adapter(', 1)[1]
theme = (root / 'kernel/syscall/graphics.c').read_text().split('int64_t syscall_theme_settings(', 1)[1]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "uapi/network.h"
#include "uapi/window_theme.h"
#define __user
#define K_OK 0
#define K_EINVAL -22
#define K_EBUSY -16
#define PCI_MAX_DEVICES 512U
typedef int kstatus_t;
typedef struct pci_device { uint16_t segment,vendor_id,device_id;
 uint8_t bus,slot,function,subclass,class_code; } pci_device_t;
typedef struct pci_host {bool initialized;uint32_t device_count;pci_device_t devices[PCI_MAX_DEVICES];} pci_host_t;
static pci_host_t host;
static const pci_device_t *active;
static int busy;
static uint32_t current_theme;
static const pci_host_t *pci_current_host(void) {return &host;}
static kstatus_t e1000_active_adapter(const pci_device_t **p) {*p=active;return busy?K_EBUSY:K_OK;}
static kstatus_t copy_from_user(void *out,const void *in,size_t n) {if(!in)return -14;memcpy(out,in,n);return 0;}
static kstatus_t copy_to_user(void *out,const void *in,size_t n) {if(!out)return -14;memcpy(out,in,n);return 0;}
static bool versioned_header_valid(const os_versioned_header_t *h,size_t n) {return h->size==n&&h->version==1&&!h->flags;}
static uint32_t window_server_theme(void) {return current_theme;}
static int window_server_set_theme(uint32_t value,uint32_t bg) {if(value>=OS_THEME_COUNT||bg>=OS_THEME_BACKGROUND_COUNT)return K_EINVAL;current_theme=value|(bg<<8U);return 0;}
''' + 'int64_t syscall_net_enum_adapter(' + network + '\nint64_t syscall_theme_settings(' + theme + r'''
#define CALL(fn,p) fn((uintptr_t)(p),0,0,0,0,0)
int main(void) {
 os_net_adapter_t a={.hdr={sizeof(a),1},.index=0};
 assert(!CALL(syscall_net_enum_adapter,&a)&&a.count==0&&!a.flags);
 host.initialized=true;host.device_count=4;
 host.devices[0]=(pci_device_t){.class_code=3};
 host.devices[1]=(pci_device_t){.class_code=2,.vendor_id=0x10ec,.device_id=0x8126,.bus=7};
 host.devices[2]=(pci_device_t){.class_code=2,.vendor_id=0x8086,.device_id=0x100e,.bus=8};
 host.devices[3]=(pci_device_t){.class_code=1};active=&host.devices[2];
 assert(!CALL(syscall_net_enum_adapter,&a)&&a.count==2&&a.vendor==0x10ec&&!a.flags&&a.bus==7);
 a.index=1;assert(!CALL(syscall_net_enum_adapter,&a)&&a.flags==OS_NET_ADAPTER_ACTIVE&&a.bus==8);
 a.index=UINT32_MAX;assert(!CALL(syscall_net_enum_adapter,&a)&&a.count==2&&!a.vendor&&!a.flags&&!a.bus);
 busy=1;assert(CALL(syscall_net_enum_adapter,&a)==K_EBUSY);busy=0;
 a.hdr.flags=1;assert(CALL(syscall_net_enum_adapter,&a)==K_EINVAL);
 assert(CALL(syscall_net_enum_adapter,NULL)==-14);
 os_theme_settings_t t={.hdr={sizeof(t),1}};
 assert(!CALL(syscall_theme_settings,&t)&&t.theme==OS_THEME_BLUE);
 t.operation=OS_THEME_WRITE;t.theme=OS_THEME_GREEN;assert(!CALL(syscall_theme_settings,&t));
 t.theme=OS_THEME_COUNT;assert(CALL(syscall_theme_settings,&t)==K_EINVAL&&current_theme==OS_THEME_GREEN);
 t.operation=2;assert(CALL(syscall_theme_settings,&t)==K_EINVAL);
 t.operation=0;t.theme=100;assert(!CALL(syscall_theme_settings,&t)&&t.theme==OS_THEME_GREEN);
 t.hdr.version=0;assert(CALL(syscall_theme_settings,&t)==K_EINVAL);
 t.hdr.version=1;t.operation=OS_THEME_WRITE;t.background=OS_THEME_BACKGROUND_COUNT;
 assert(CALL(syscall_theme_settings,&t)==K_EINVAL);
 t.background=3;t.theme=OS_THEME_PURPLE;assert(!CALL(syscall_theme_settings,&t)&&t.background==3);
 assert(os_theme_resolve(OS_THEME_SURFACE,3U<<8)!=OS_THEME_SURFACE);
 assert(os_theme_resolve(OS_THEME_TEXT,3U<<8)!=OS_THEME_TEXT);
 for(uint32_t id=0;id<OS_THEME_COUNT;++id) {
  uint32_t value=os_theme_resolve(0x801a73e8U,id);
  assert((value>>24)==0x80);
  assert(os_theme_resolve(value,id)==value);
  assert(os_theme_resolve(0x12345678U,id)==0x12345678U);
  assert(os_theme_resolve(OS_THEME_SURFACE,id)==OS_THEME_SURFACE);
  if(id) assert(os_theme_resolve(OS_THEME_HEADER,id)!=OS_THEME_HEADER);
 }
 puts("PASS: theme API validation, palette alpha, multiple adapters, identity isolation, busy and bounds");
}
'''
with tempfile.TemporaryDirectory() as d:
    src=Path(d)/'test.c';src.write_text(program)
    out=Path(d)/'test'
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-I'+str(root/'include'),str(src),'-o',str(out)],check=True)
    subprocess.run([str(out)],check=True)
