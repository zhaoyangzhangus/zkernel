"""Actual IPv4/IPv6 receive functions: terminal wakeup and buffered-byte order."""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]


def function(file, signature):
    text = (root / file).read_text()
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


code = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <kernel/net_core.h>
#include "socket_model.h"
#define LINUX_ECONNRESET (-104)
static socket_t target, peer;
static socket_private_t *state_storage;
#define state (*state_storage)
static unsigned locked, waits, peer_refs=1, peer_wakes, peer_notifications;
static bool peer_available=true, detach_on_unlock;
static int arrive_error;
static bool empty_wait_timeout;
socket_private_t *socket_private(socket_t *s){return s==&target?&state:0;}
void socket_lock(spinlock_t *l){(void)l;assert(!locked);locked=1;}
void socket_unlock(spinlock_t *l){(void)l;assert(locked);locked=0;
 if(detach_on_unlock){state.peer=NULL;detach_on_unlock=false;}}
bool object_try_get(void *p){assert(p==&peer && locked);
 if(!peer_available)return false;
 ++peer_refs;return true;}
void object_put(void *p){assert(p==&peer && !locked && peer_refs==2);--peer_refs;}
uint32_t wake_all(wait_queue_t *q){assert(q==&peer.waitq && !locked && peer_refs==2);
 assert(atomic_load(&state.stream_count)<SOCKET_STREAM_BUFFER_SIZE);++peer_wakes;return 0;}
void object_notify_changed(void *p){assert(p==&peer && !locked && peer_refs==2);++peer_notifications;}
void socket_copy(void *out,const void *in,size_t n){memcpy(out,in,n);}
void socket_tcp_output_get(socket_tcp_ipv4_output_fn *fn,void **ctx){*fn=0;*ctx=0;}
void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *fn,void **ctx){*fn=0;*ctx=0;}
kstatus_t wait_on_queue(wait_queue_t *q,bool (*ready)(void *),void *ctx,uint64_t ns){
 assert(q==&target.waitq && !locked && ns==123);++waits;
 if(empty_wait_timeout && !ready(ctx))return K_ETIMEDOUT;
 if(!ready(ctx)){
  assert(arrive_error);socket_lock(&target.lock);
  socket_tcp_fail_locked(&state,arrive_error);socket_unlock(&target.lock);
 }
 assert(ready(ctx));return K_OK;
}
''' + function('kernel/net/socket.c', 'uint32_t socket_stream_read_locked(')
code += '\n'.join(function('kernel/net/socket_io.c', name) for name in (
    'static void socket_receive_notify_peer(',
    'static size_t socket_receive_stream_locked(',
    'static bool socket_receive_ready(',
    'static kstatus_t socket_recv_ipv4_impl(',
    'static kstatus_t socket_recv_ipv6_impl('))
code += function('kernel/net/socket.c', 'kstatus_t socket_get_option(')
code += function('kernel/syscall/linux.c', 'static int64_t linux_socket_status(')
code += function('kernel/syscall/linux.c', 'static int64_t linux_socket_receive_status(')
code += r'''
static int receive(unsigned family,char *buffer,size_t n,uint64_t *bytes,bool peek){
 socket_ipv4_endpoint_t ep4={0};socket_ipv6_endpoint_t ep6={0};
 return family?socket_recv_ipv6_impl(&target,buffer,n,&ep6,123,bytes,peek):
  socket_recv_ipv4_impl(&target,buffer,n,&ep4,123,bytes,peek);
}
static void reset(unsigned family){
 memset(&state,0,sizeof(state));memset(&target,0,sizeof(target));
 target.object.type=KOBJECT_TYPE_SOCKET;target.type=OS_SOCK_STREAM;
 target.family=family?OS_AF_INET6:OS_AF_INET4;
 state.tcp_state=SOCKET_TCP_ESTABLISHED;state.tcp_send_pending=true;
 state.tcp_send_deadline_tsc=99;state.tcp_local_retry_tsc=88;
 state.tcp_keep_deadline_tsc=77;arrive_error=0;waits=0;empty_wait_timeout=false;
}
int main(void){
 state_storage=calloc(1,sizeof(socket_private_t)+sizeof(socket_datagram_t)*SOCKET_RX_QUEUE_DEPTH);
 assert(state_storage);
 const int errors[]={K_ETIMEDOUT,K_ECONNRESET,K_ECONNREFUSED,K_ENETUNREACH,K_EIO};
 char out[8];uint64_t bytes;
 for(unsigned family=0;family<2;++family){
  reset(family);empty_wait_timeout=true;
  assert(linux_socket_receive_status(&target,
   receive(family,out,sizeof(out),&bytes,false))==K_EAGAIN);
  assert(!locked && waits==1 && !atomic_load(&state.tcp_error));
  for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);++i){
   for(unsigned before_wait=0;before_wait<2;++before_wait){
    reset(family);arrive_error=errors[i];
    if(before_wait){socket_lock(&target.lock);socket_tcp_fail_locked(&state,errors[i]);socket_unlock(&target.lock);}
    assert(receive(family,out,sizeof(out),&bytes,false)==errors[i]);
    assert(!locked && waits==1 && !state.tcp_send_pending);
    assert(linux_socket_receive_status(&target,
     receive(family,out,sizeof(out),&bytes,false))==errors[i]);
    assert(!state.tcp_send_deadline_tsc && !state.tcp_local_retry_tsc && !state.tcp_keep_deadline_tsc);
   }
   reset(family);state.stream_read=SOCKET_STREAM_BUFFER_SIZE-1;
   state.stream_buffer[state.stream_read]='a';state.stream_buffer[0]='b';
   atomic_store(&state.stream_count,2);
   socket_lock(&target.lock);socket_tcp_fail_locked(&state,errors[i]);socket_unlock(&target.lock);
   assert(receive(family,out,sizeof(out),&bytes,true)==K_OK && bytes==2 && !memcmp(out,"ab",2));
   assert(atomic_load(&state.stream_count)==2);
   assert(receive(family,out,sizeof(out),&bytes,false)==K_OK && bytes==2 && !memcmp(out,"ab",2));
   assert(!atomic_load(&state.stream_count));
   assert(receive(family,out,sizeof(out),&bytes,false)==errors[i]);
  }
  reset(family);atomic_store(&state.peer_closed,true);
  assert(receive(family,out,sizeof(out),&bytes,false)==K_OK && bytes==0);
  reset(family);atomic_store(&state.receive_shutdown,true);
  assert(receive(family,out,sizeof(out),&bytes,false)==K_OK && bytes==0 && !waits);
  reset(family);state.peer=&peer;
  memset(state.stream_buffer,'x',sizeof(state.stream_buffer));
  atomic_store(&state.stream_count,SOCKET_STREAM_BUFFER_SIZE);
  peer_wakes=peer_notifications=0;
  assert(receive(family,out,1,&bytes,true)==K_OK && bytes==1);
  assert(!peer_wakes && !peer_notifications && peer_refs==1);
  assert(receive(family,NULL,0,&bytes,false)==K_OK && !bytes);
  assert(!peer_wakes && !peer_notifications && peer_refs==1);
  assert(receive(family,out,1,&bytes,false)==K_OK && bytes==1);
  assert(peer_wakes==1 && peer_notifications==1 && peer_refs==1);
  // Notify on partial drains too: a blocked atomic write may need more room.
  assert(receive(family,out,2,&bytes,false)==K_OK && bytes==2);
  assert(peer_wakes==2 && peer_notifications==2 && peer_refs==1);
  // A reference, not the mutable link, owns notification after unlock.
  detach_on_unlock=true;
  assert(receive(family,out,1,&bytes,false)==K_OK && bytes==1);
  assert(!state.peer && peer_wakes==3 && peer_notifications==3 && peer_refs==1);
  state.peer=&peer;peer_available=false;
  assert(receive(family,out,1,&bytes,false)==K_OK && bytes==1);
  assert(peer_wakes==3 && peer_notifications==3 && peer_refs==1);
  peer_available=true;
 }
 free(state_storage);
 puts("Terminal receive: both families, 5 errors, buffered ordering, EOF/shutdown; loopback full/partial drain wake, peek/zero-read silence and peer lifetime PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='socket-terminal-receive-') as directory:
    path = Path(directory)
    for negative in (None, 'terminal_error', 'peer_notification', 'peek_consumes'):
        candidate = code
        if negative == 'terminal_error':
            candidate = candidate.replace(
                'atomic_load_explicit(&wait->private->tcp_error, memory_order_acquire) != 0',
                'false')
        elif negative == 'peer_notification':
            candidate = candidate.replace('object_notify_changed(peer);', '(void)peer;')
        elif negative == 'peek_consumes':
            candidate = candidate.replace('if (!peek) {', 'if (true) {')
        if negative:
            assert candidate != code
        (path / 'test.c').write_text(candidate)
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I', str(root/'include'), '-I', str(root/'kernel/net'),
                        str(path/'test.c'), '-o', str(path/'test')], check=True)
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        result = subprocess.run([str(path/'test')], capture_output=True, text=True,
                                preexec_fn=no_core)
        if negative:
            assert result.returncode != 0, 'Missing receive wake must fail'
            print('Negative control rejected:', negative)
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout, end='')
