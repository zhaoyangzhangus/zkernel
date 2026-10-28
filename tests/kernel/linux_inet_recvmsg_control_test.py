"""Run production INET recvmsg with Chromium's ancillary output capacity."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()


def function(name):
    start = source.index('static int64_t ' + name + '(')
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


types = source[source.index('typedef struct linux_iovec {'):
               source.index('typedef struct linux_pollfd {')]
recv = function('linux_recvmsg').split(
    '    if (entry.kind == LINUX_FD_UNIX_SOCKET) {', 1)[0]
recv += '\n    linux_fd_entry_put(&entry); return K_ENOTSOCK;\n}\n'
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define __user
#define K_OK 0
#define K_EINVAL -22
#define K_EOVERFLOW -75
#define K_ENOTSOCK -88
#define LINUX_EFAULT -14
#define LINUX_EOPNOTSUPP -95
#define LINUX_MAX_IOV 64
#define SOCKET_MAX_PAYLOAD 4096
#define LINUX_MSG_DONTWAIT 64U
#define LINUX_MSG_CMSG_CLOEXEC 0x40000000U
#define LINUX_MSG_PEEK 2U
#define LINUX_FD_SOCKET 1
#define KOBJECT_TYPE_SOCKET 7
typedef int kstatus_t;
typedef int process_t;
typedef struct {struct {int type;} object; unsigned family;} socket_t;
typedef struct {unsigned kind;void *private_data;} linux_fd_entry_t;
static socket_t sock={{7},2};
static unsigned puts,receives;
static int receive_error;
static bool linux_user_pointer(uint64_t p,size_t n){return p>4096 && n<=UINTPTR_MAX-p;}
static int copy_from_user(void *d,const void *s,size_t n){memcpy(d,s,n);return 0;}
static int copy_to_user(void *d,const void *s,size_t n){memcpy(d,s,n);return 0;}
static int linux_fd_lookup(process_t *p,int64_t fd,linux_fd_entry_t *e){
 (void)p;assert(fd==8);e->kind=LINUX_FD_SOCKET;e->private_data=&sock;return 0;
}
static void linux_fd_entry_put(linux_fd_entry_t *e){assert(e->private_data==&sock);++puts;}
static unsigned linux_socket_address_size(unsigned family){assert(family==2);return 16;}
static int64_t linux_socket_receive_payload(socket_t *s,const linux_fd_entry_t *e,
 uint8_t *payload,size_t capacity,uint32_t flags,uint64_t name,uint64_t name_length){
 assert(s==&sock && e->private_data==s && capacity==32);
 assert(flags==0 || flags==LINUX_MSG_DONTWAIT || flags==LINUX_MSG_PEEK);
 ++receives;if(receive_error)return receive_error;
 memcpy(payload,"DNS reply",9);
 if(name){memset((void*)(uintptr_t)name,0x11,16);*(uint32_t*)(uintptr_t)name_length=16;}
 return 9;
}
''' + types + '\n' + function('linux_message_load') + '\n' + function(
    'linux_message_scatter') + '\n' + recv + r'''
int main(void){
 unsigned char output[32],control[512],name[16];
 linux_iovec_t vectors[2]={{(uintptr_t)output,4},{(uintptr_t)(output+4),28}};
 for(unsigned capacity=0;capacity<=512;capacity+=128){
  for(unsigned mode=0;mode<3;++mode){
   memset(output,0xaa,sizeof(output));memset(control,0xbb,sizeof(control));
   linux_msghdr_t m={.name=(uintptr_t)name,.name_length=16,
    .iov_pointer=(uintptr_t)vectors,.iov_length=2,.control=(uintptr_t)control,
    .control_length=capacity,.flags=0xdead};
   uint32_t flags=mode==0?0:mode==1?LINUX_MSG_DONTWAIT:LINUX_MSG_PEEK;
   assert(linux_recvmsg(0,8,(uintptr_t)&m,flags|LINUX_MSG_CMSG_CLOEXEC)==9);
   assert(!memcmp(output,"DNS reply",9)&&output[9]==0xaa);
   assert(!m.control_length&&!m.flags&&m.name_length==16);
   for(unsigned i=0;i<sizeof(control);++i)assert(control[i]==0xbb);
  }
 }
 assert(receives==15&&puts==15);
 linux_msghdr_t m={.iov_pointer=(uintptr_t)vectors,.iov_length=2,
  .control=(uintptr_t)control,.control_length=512,.flags=123};
 receive_error=-11;
 assert(linux_recvmsg(0,8,(uintptr_t)&m,0)==-11);
 assert(m.control_length==512&&m.flags==123&&receives==16&&puts==16);
 m.control=0;
 assert(linux_recvmsg(0,8,(uintptr_t)&m,0)==LINUX_EFAULT);
 assert(receives==16&&puts==16);
}
'''
with tempfile.TemporaryDirectory(prefix='inet-recvmsg-control-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
    # Restoring the old rejection must fail the same Chromium-shaped case.
    broken = program.replace(
        '        if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET) {',
        '        if (message.control_length != 0U) return LINUX_EOPNOTSUPP;\n'
        '        if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET) {', 1)
    assert broken != program
    (path / 'test.c').write_text(broken)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'old')], check=True)
    import resource
    old_limit = resource.getrlimit(resource.RLIMIT_CORE)
    try:
        resource.setrlimit(resource.RLIMIT_CORE, (0, old_limit[1]))
        assert subprocess.run([str(path / 'old')], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode != 0
    finally:
        resource.setrlimit(resource.RLIMIT_CORE, old_limit)
print('INET recvmsg: Chromium control capacity, scatter, source, peek, errors PASS')
print('Negative control rejected: old ancillary-capacity rejection')
