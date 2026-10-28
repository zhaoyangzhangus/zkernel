"""Actual UDP injection/receive: poisoned slots, lengths, peek and FIFO reuse."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def function(file, signature):
    source = (root/file).read_text()
    start = source.index(signature)
    end = source.index('{', start)+1
    depth = 1
    while depth:
        depth += (source[end] == '{')-(source[end] == '}')
        end += 1
    return source[start:end]


code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/net_core.h>
#include "socket_model.h"
socket_t target;
static socket_private_t *state_storage;
#define state (*state_storage)
static socket_binding_t binding;
static spinlock_t binding_lock;
unsigned locked;
static unsigned refs=1,wakes,notifications;
static bool found=true;
static const uint8_t src6[16]={0x20,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
static const uint8_t dst6[16]={0x20,1,0,0,0,0,0,0,0,0,0,0,0,0,0,2};
void socket_initialize_globals(void){}
spinlock_t *socket_binding_lock(void){return &binding_lock;}
int32_t socket_find_binding_locked(uint32_t a,uint16_t p,uint16_t t,bool l){
 assert(locked && a==2 && p==4321 && t==OS_SOCK_DGRAM && !l);return found?0:-1;
}
int32_t socket_find_binding6_locked(const uint8_t a[16],uint16_t p,uint16_t t,bool l){
 assert(locked && !memcmp(a,dst6,16) && p==4321 && t==OS_SOCK_DGRAM && !l);return found?0:-1;
}
const socket_binding_t *socket_binding_at(uint32_t n){assert(locked && !n);return &binding;}
socket_private_t *socket_private(socket_t *s){return s==&target?&state:NULL;}
void socket_lock(spinlock_t *l){(void)l;assert(!locked);locked=1;}
void socket_unlock(spinlock_t *l){(void)l;assert(locked);locked=0;}
bool object_try_get(void *p){assert(p==&target && locked);++refs;return true;}
void object_put(void *p){assert(p==&target && !locked && refs==2);--refs;}
void object_notify_signaled(void *p){assert(p==&target && !locked);++notifications;}
void object_notify_changed(void *p){(void)p;assert(0);}
uint32_t wake_all(wait_queue_t *q){assert(q==&target.waitq && !locked);++wakes;return 0;}
void socket_copy(void *d,const void *s,size_t n){if(n){assert(d && s);memmove(d,s,n);}}
void socket_tcp_output_get(socket_tcp_ipv4_output_fn *f,void **c){*f=NULL;*c=NULL;}
void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *f,void **c){*f=NULL;*c=NULL;}
'''
code += function('kernel/net/socket.c', 'uint32_t socket_stream_read_locked(')
code += '\n'.join(function('kernel/net/socket_protocol.c', name) for name in (
    'kstatus_t socket_inject_udp(', 'kstatus_t socket_inject_udp_ipv6('))
code += '\n'.join(function('kernel/net/socket_io.c', name) for name in (
    'static void socket_receive_notify_peer(',
    'static size_t socket_receive_stream_locked(',
    'static bool socket_receive_ready(', 'static kstatus_t socket_recv_ipv4_impl(',
    'static kstatus_t socket_recv_ipv6_impl('))
code += r'''
static kstatus_t inject(unsigned family,const void *data,size_t n){
 return family?socket_inject_udp_ipv6(src6,1234,dst6,4321,data,n):
               socket_inject_udp(1,1234,2,4321,data,n);
}
static kstatus_t receive(unsigned family,void *out,size_t cap,uint64_t *bytes,bool peek){
 socket_ipv4_endpoint_t e4;socket_ipv6_endpoint_t e6;
 memset(&e4,0xa5,sizeof(e4));memset(&e6,0xa5,sizeof(e6));
 kstatus_t rc=family?socket_recv_ipv6_impl(&target,out,cap,&e6,0,bytes,peek):
                    socket_recv_ipv4_impl(&target,out,cap,&e4,0,bytes,peek);
 if(rc==K_OK || rc==K_EAGAIN){
  if(family)assert(!memcmp(e6.address,src6,16) && e6.port==1234 && !e6.reserved);
  else assert(e4.address==1 && e4.port==1234 && !e4.reserved);
 }
 assert(!locked && refs==1);return rc;
}
static unsigned char input[SOCKET_MAX_PAYLOAD],output[SOCKET_MAX_PAYLOAD+32];
int main(void){
 const size_t state_size=sizeof(socket_private_t)+sizeof(socket_datagram_t)*SOCKET_RX_QUEUE_DEPTH;
 state_storage=malloc(state_size);assert(state_storage);
 binding.socket=&target;target.object.type=KOBJECT_TYPE_SOCKET;target.type=OS_SOCK_DGRAM;
 unsigned cases=0;
 for(unsigned family=0;family<2;++family)for(unsigned pattern=1;pattern<=3;++pattern){
  memset(&state,(int)(pattern*61),state_size);
  memset(&state,0,offsetof(socket_private_t,stream_buffer));
  target.family=family?OS_AF_INET6:OS_AF_INET4;
  for(unsigned n=0;n<=SOCKET_MAX_PAYLOAD;++n)for(unsigned mode=0;mode<3;++mode){
   for(unsigned i=0;i<n;++i)input[i]=(unsigned char)(i*17+n);
   assert(inject(family,n?input:NULL,n)==K_OK);
   unsigned cap=mode==0?n:mode==1?n/2:0;
   for(unsigned peek=1;peek<=2;++peek){
    memset(output,0x59,sizeof(output));uint64_t bytes=UINT64_MAX;
    assert(receive(family,cap?output+7:NULL,cap,&bytes,peek==1)==(cap<n?K_EAGAIN:K_OK));
    assert(bytes==cap && !memcmp(output+7,input,cap));
    for(unsigned i=0;i<7;++i)assert(output[i]==0x59);
    for(unsigned i=7+cap;i<sizeof(output);++i)assert(output[i]==0x59);
    assert(atomic_load(&state.datagram_count)==(peek==1?1U:0U));
   }
   uint64_t bytes=UINT64_MAX;
   assert(receive(family,output,sizeof(output),&bytes,false)==K_ETIMEDOUT && bytes==UINT64_MAX);
   ++cases;
  }
  unsigned before=wakes;
  for(unsigned i=0;i<SOCKET_RX_QUEUE_DEPTH;++i){input[0]=(uint8_t)i;assert(inject(family,input,1)==K_OK);}
  assert(inject(family,input,1)==K_EAGAIN && wakes==before+SOCKET_RX_QUEUE_DEPTH);
  for(unsigned i=0;i<SOCKET_RX_QUEUE_DEPTH;++i){
   uint64_t bytes;assert(receive(family,output,1,&bytes,false)==K_OK && bytes==1 && output[0]==i);
  }
  before=wakes;
  atomic_store(&state.receive_shutdown,true);assert(inject(family,input,1)==K_OK);
  atomic_store(&state.receive_shutdown,false);
  atomic_store(&state.closed,true);assert(inject(family,input,1)==K_OK);
  atomic_store(&state.closed,false);found=false;
  assert(inject(family,input,1)==K_ENOENT);found=true;
  assert(inject(family,input,SOCKET_MAX_PAYLOAD+1)==K_EINVAL);
  assert(inject(family,NULL,1)==K_EINVAL);
  assert(!atomic_load(&state.datagram_count) && wakes==before && !locked && refs==1);
 }
 assert(wakes==notifications);
 free(state_storage);
 printf("UDP receive: %u poisoned IPv4/IPv6 length/peek/truncation cases; output guards, empty queue, FIFO wrap/full, shutdown and balanced refs PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='socket-datagram-') as directory:
    binary = directory+'/test'
    flags = ['cc','-std=c11','-O2','-g','-Wall','-Wextra','-Werror',
             '-I'+str(root/'include'),'-I'+str(root/'kernel/net'),
             '-fsanitize=address,undefined','-fno-pie']
    # Keep the scheduler boundary out of the socket translation unit, as in
    # the kernel; whole-fixture IPA otherwise folds this controlled wait stub.
    wait_mock = r'''
#include <assert.h>
#include "socket_model.h"
extern socket_t target;
extern unsigned locked;
kstatus_t wait_on_queue(wait_queue_t *q,bool (*ready)(void *),void *ctx,uint64_t ns){
 assert(q==&target.waitq && !locked && !ns);return ready(ctx)?K_OK:K_ETIMEDOUT;
}
'''
    wait_object=directory+'/wait.o'
    subprocess.run([*flags,'-x','c','-','-c','-o',wait_object],
                   input=wait_mock,text=True,check=True)
    command = [*flags,'-no-pie','-x','c','-','-x','none',wait_object,'-o',binary]
    subprocess.run(command,input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    for name, old, new in (
        ('payload overread', 'length : datagram_length;', 'length : (size_t)datagram_length+1;'),
        ('peek consumes packet', 'if (!peek) {\n                private->datagram_read',
         'if (true) {\n                private->datagram_read'),
    ):
        broken=code.replace(old,new)
        assert broken!=code,name
        subprocess.run(command,input=broken,text=True,check=True)
        result=subprocess.run([binary],text=True,capture_output=True,timeout=30,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert result.returncode==-signal.SIGABRT and 'Assertion' in result.stderr,(name,result)
        print('Negative control rejected:',name)
