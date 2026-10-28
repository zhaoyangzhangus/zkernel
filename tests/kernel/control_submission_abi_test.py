"""执行真实系统调用，验证新结构长度、版本回写和失败时保留用户结果。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/graphics.c').read_text()
body = source[source.index('int64_t syscall_window_control_tree('):source.index('int64_t syscall_window_title(')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uapi/control_tree.h>
#define __user
typedef int kstatus_t;
typedef struct {int id;} process_t;
enum {K_OK=0,K_EINVAL=-22,K_EPERM=-1,K_EFAULT=-14};
static process_t owner;
static bool owner_available=true;
static int read_error,write_error,submit_error;
static unsigned reads,writes,submits;
static process_t *current_process(void){return owner_available?&owner:0;}
static int copy_from_user(void *out,const void *in,size_t size){
 ++reads;assert(size==48);if(read_error)return read_error;memcpy(out,in,size);return 0;
}
static int copy_to_user(void *out,const void *in,size_t size){
 ++writes;assert(size==48);if(write_error)return write_error;memcpy(out,in,size);return 0;
}
static bool versioned_header_valid(const os_versioned_header_t *h,size_t size){
 return h->size==size&&h->version==1&&!h->flags;
}
static int window_server_control_tree(process_t *process,os_window_control_tree_t *r){
 assert(process==&owner);++submits;if(submit_error)return submit_error;r->revision=123;return 0;
}
''' + body + r'''
static int invoke(os_window_control_tree_t *r){return (int)syscall_window_control_tree((uintptr_t)r,0,0,0,0,0);}
int main(void){
 os_window_control_tree_t base={.hdr={.size=48,.version=1},.identifier=7,
  .count=1,.nodes=0x1234,.revision=99},r=base;
 assert(invoke(&r)==0&&r.revision==123&&reads==1&&writes==1&&submits==1);
 for(unsigned bad=0;bad<11;++bad){
  r=base;unsigned before=submits;
  switch(bad){
   case 0:r.hdr.size=40;break;case 1:r.hdr.version=0;break;
   case 2:r.reserved=1;break;case 3:r.identifier=0;break;
   case 4:r.count=4097;break;case 5:r.text_size=65537;break;
   case 6:r.nodes=0;break;case 7:r.text_size=1;break;
   case 8:r.count=0;r.text_size=1;r.text=0x123;break;
   case 9:r.hdr.flags=1;break;case 10:r.hdr.size=56;break;
  }
  assert(invoke(&r)==K_EINVAL&&submits==before&&r.revision==99);
 }
 r=base;submit_error=-12;unsigned before=writes;
 assert(invoke(&r)==-12&&writes==before&&r.revision==99);submit_error=0;
 read_error=K_EFAULT;before=submits;assert(invoke(&r)==K_EFAULT&&submits==before);read_error=0;
 owner_available=false;before=reads;assert(invoke(&r)==K_EPERM&&reads==before);owner_available=true;
 write_error=K_EFAULT;assert(invoke(&r)==K_EFAULT&&r.revision==99);
 puts("Control submission ABI: exact 48-byte request, old-size rejection, revision copy-back and permission/copy/submission failure propagation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='control-submission-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),
                    '-x','c','-','-o',binary],input=program,text=True,check=True)
    subprocess.run([binary],check=True)
