"""Execute actual FIONREAD and pipe-byte snapshot with pin/copy fault checks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
linux = (root / 'kernel/syscall/linux.c').read_text()
pipe = (root / 'kernel/ipc/pipe.c').read_text()
start = linux.index('static int64_t linux_fionread(')
helper = linux[start:linux.index('\n}', start) + 2]
snapshot = pipe[pipe.index('kstatus_t pipe_pending_bytes('):pipe.index('kstatus_t pipe_stat(')]
fixture = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#define __user
#define K_OK 0
#define K_EBADF -9
#define LINUX_EFAULT -14
#define LINUX_ENOTTY -25
#define LINUX_FD_PIPE 7
#define LINUX_FD_UNIX_SOCKET 10
typedef int kstatus_t;
typedef struct { bool held; uint64_t count; } pipe_state_t;
typedef struct { bool valid; pipe_state_t *state; unsigned refs; } pipe_endpoint_t;
typedef struct { unsigned refs; } linux_unix_channel_t;
typedef struct { pipe_endpoint_t *endpoint; linux_unix_channel_t *channel; } linux_unix_socket_t;
typedef struct { unsigned kind; void *private_data; } linux_fd_entry_t;
static bool pointer_valid=true, copy_valid=true;
static bool pipe_is_endpoint(const pipe_endpoint_t *p) {return p&&p->valid;}
static void pipe_lock(pipe_state_t *s) {assert(!s->held);s->held=true;}
static void pipe_unlock(pipe_state_t *s) {assert(s->held);s->held=false;}
static void linux_unix_socket_pin_io(linux_unix_socket_t *s,bool write,pipe_endpoint_t **p,linux_unix_channel_t **c) {
 assert(!write);*p=s->endpoint;*c=*p?s->channel:0;
 if(*p)++(*p)->refs;if(*c)++(*c)->refs;
}
static void linux_unix_socket_release_endpoint(pipe_endpoint_t **p) {if(*p){assert((*p)->refs>1);--(*p)->refs;*p=0;}}
static void linux_unix_socket_release_channel(linux_unix_channel_t **p) {if(*p){assert((*p)->refs>1);--(*p)->refs;*p=0;}}
static bool linux_user_pointer(uint64_t p,unsigned long n) {assert(n==4);return p&&pointer_valid;}
static int copy_to_user(void *p,const void *v,unsigned long n) {if(!copy_valid)return -1;memcpy(p,v,n);return 0;}
'''
checks = r'''
int main(void) {
 pipe_state_t state={0};pipe_endpoint_t p={true,&state,1};linux_unix_channel_t c={1};
 linux_unix_socket_t socket={&p,&c};int32_t output=-1;
 linux_fd_entry_t entry={LINUX_FD_PIPE,&p};
 for(unsigned kind=0;kind<2;++kind) {
  entry.kind=kind?LINUX_FD_UNIX_SOCKET:LINUX_FD_PIPE;entry.private_data=kind?(void*)&socket:(void*)&p;
  for(unsigned n=0;n<1000;++n){state.count=n;assert(!linux_fionread(&entry,(uintptr_t)&output)&&output==(int32_t)n&&!state.held&&p.refs==1&&c.refs==1);}
  state.count=UINT64_MAX;assert(!linux_fionread(&entry,(uintptr_t)&output)&&output==INT32_MAX);
  pointer_valid=false;assert(linux_fionread(&entry,(uintptr_t)&output)==LINUX_EFAULT);pointer_valid=true;
  copy_valid=false;assert(linux_fionread(&entry,(uintptr_t)&output)==LINUX_EFAULT);copy_valid=true;
  assert(p.refs==1&&c.refs==1&&!state.held);
 }
 socket.endpoint=0;assert(!linux_fionread(&entry,(uintptr_t)&output)&&output==0);
 entry.kind=0;assert(linux_fionread(&entry,(uintptr_t)&output)==LINUX_ENOTTY);
 uint64_t bytes;assert(pipe_pending_bytes(0,&bytes)==K_EBADF);assert(pipe_pending_bytes(&p,0)==K_EBADF);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture + snapshot + helper + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                    '-fsanitize=address,undefined', '-no-pie', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Actual FIONREAD: locked byte snapshot, pipe/Unix socket, shutdown EOF, int saturation, pin release and copy faults passed')
