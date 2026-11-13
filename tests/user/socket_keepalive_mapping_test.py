"""Actual libc set/get and Linux option translation for keepalive namespaces."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
def function(file,signature):
    source=(root/file).read_text();start=source.index(signature)
    end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
socket_header=(root/'user/libc/include/sys/socket.h').read_text()
defines='\n'.join(line for line in socket_header.splitlines()
                  if line.startswith('#define SO_') or line.startswith('#define SOL_SOCKET '))
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include "uapi/socket.h"
#include "uapi/linux_abi.h"
#include "uapi/syscall.h"
#define IPPROTO_TCP 6
typedef uint32_t socklen_t;
''' + (root/'user/libc/include/netinet/tcp.h').read_text().replace('#pragma once','') + defines + r'''
static bool linux_mode,linux_fd;
static unsigned native_calls,linux_calls,last_option,last_level,last_name;
static int last_value,reply_value=17;
static bool libc_linux_personality(void){return linux_mode;}
static bool __libc_descriptor_is_linux_fd(int fd){assert(fd==7);return linux_fd;}
static int __libc_linux_fd_for_descriptor(int fd){assert(fd==7);return 42;}
static int socket_handle(int fd,os_handle_t *out,void *unused){assert(fd==7 && !unused);*out=99;return 0;}
static int socket_error(int value){errno=value;return -1;}
static int __libc_status_result(int64_t status){if(status<0){errno=(int)-status;return -1;}return (int)status;}
static int64_t __libc_native_syscall6(uint64_t nr,uint64_t arg,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f){
 assert(!b && !c && !d && !e && !f);++native_calls;
 os_socket_option_value_t *value=(void*)(uintptr_t)arg;
 assert(value->socket==99 && value->hdr.size==sizeof(*value));last_option=value->option;
 if(nr==OS_SYS_SOCKET_SET_OPTION)last_value=value->value;
 else{assert(nr==OS_SYS_SOCKET_GET_OPTION);value->value=reply_value;}return 0;
}
static int64_t __libc_linux_syscall6(uint64_t nr,uint64_t fd,uint64_t level,uint64_t name,uint64_t value,uint64_t length,uint64_t unused){
 assert(fd==42 && !unused);++linux_calls;last_level=(unsigned)level;last_name=(unsigned)name;
 int *address=(void*)(uintptr_t)value;
 if(nr==LINUX_SYS_SETSOCKOPT){assert(length==sizeof(int));last_value=*address;}
 else{assert(nr==LINUX_SYS_GETSOCKOPT);*address=reply_value;*(socklen_t*)(uintptr_t)length=sizeof(int);}return 0;
}
''' + '\n'.join(function('user/libc/socket.c',s) for s in (
    'static uint32_t socket_option_number(', 'int setsockopt(', 'int getsockopt(')) + '\n' + function('kernel/syscall/linux.c','static bool linux_socket_option(') + r'''
int main(void){
 struct {int level,name;uint32_t native;} cases[]={
  {SOL_SOCKET,SO_KEEPALIVE,OS_SOCKET_OPTION_KEEPALIVE},
  {IPPROTO_TCP,TCP_KEEPIDLE,OS_SOCKET_OPTION_KEEPIDLE},
  {IPPROTO_TCP,TCP_KEEPINTVL,OS_SOCKET_OPTION_KEEPINTVL},
  {IPPROTO_TCP,TCP_KEEPCNT,OS_SOCKET_OPTION_KEEPCNT}};
 for(int mode=0;mode<3;++mode){
  linux_mode=mode!=0;linux_fd=mode==2;
  for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
   int value=3,out=0; socklen_t length=sizeof(out);uint32_t translated=0;
   assert(linux_socket_option(cases[i].level,cases[i].name,&translated));
   assert(translated==cases[i].native);
   assert(setsockopt(7,cases[i].level,cases[i].name,&value,sizeof(value))==0);
   assert(last_value==3);
   assert(getsockopt(7,cases[i].level,cases[i].name,&out,&length)==0);
   assert(out==17 && length==sizeof(out));
   if(mode==2)assert(last_level==(unsigned)cases[i].level && last_name==(unsigned)cases[i].name);
   else assert(last_option==cases[i].native);
  }
 }
 assert(native_calls==16 && linux_calls==8);
 int value=1; socklen_t length=sizeof(value);uint32_t out=0;
 assert(setsockopt(7,99,SO_KEEPALIVE,&value,sizeof(value))==-1 && errno==ENOPROTOOPT);
 assert(setsockopt(7,IPPROTO_TCP,TCP_NODELAY,&value,sizeof(value))==-1 && errno==ENOPROTOOPT);
 assert(setsockopt(7,SOL_SOCKET,SO_KEEPALIVE,0,sizeof(value))==-1 && errno==EINVAL);
 assert(setsockopt(7,SOL_SOCKET,SO_KEEPALIVE,&value,1)==-1 && errno==EINVAL);
 assert(getsockopt(7,SOL_SOCKET,SO_KEEPALIVE,0,&length)==-1 && errno==EINVAL);
 assert(getsockopt(7,SOL_SOCKET,SO_KEEPALIVE,&value,0)==-1 && errno==EINVAL);
 assert(!linux_socket_option(99,SO_KEEPALIVE,&out));
 assert(!linux_socket_option(IPPROTO_TCP,TCP_NODELAY,&out));
 assert(!linux_socket_option(IPPROTO_TCP,TCP_KEEPIDLE,0));
 // 选项编号 4 在 SOL_SOCKET 和 SOL_TCP 下含义不同。
 assert(socket_option_number(SOL_SOCKET,SO_ERROR)==OS_SOCKET_OPTION_ERROR);
 assert(socket_option_number(IPPROTO_TCP,TCP_KEEPIDLE)==OS_SOCKET_OPTION_KEEPIDLE);
 assert(native_calls==16 && linux_calls==8);
}
'''
with tempfile.TemporaryDirectory(prefix='system-keepalive-mapping-') as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
                    '-I',str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('keepalive mapping: actual libc native/Linux-fd set/get, option namespaces and validation passed (ASan/UBSan)')
