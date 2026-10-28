"""Real allocation/destructor: poisoned buffers, control state and OOM cleanup.

Close and registry ownership are mocked; the production destructor must call
them before freeing the combined private/bitmap state and public object.
"""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/net/socket.c').read_text()
allocation = source[source.index('kstatus_t socket_allocate('):
                    source.index('kstatus_t socket_shutdown(')]
ring = source[source.index('uint32_t socket_stream_write_locked('):
              source.index('static uint32_t socket_active_ipv4_address(')]
destroy = source[source.index('static void socket_destroy('):
                 source.index('static void socket_handle_close(')]
program = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <kernel/wait.h>
#include "socket_model.h"
static void socket_destroy(void *object);
static const object_ops_t g_socket_object_ops={.destroy=socket_destroy};
static unsigned calls,live,fail_at,poison;
static unsigned reservations,closes;
static size_t cleared,private_bytes;
static bool reserve_fail,close_fails;
static kstatus_t socket_registry_reserve(void){if(reserve_fail)return K_ENOMEM;++reservations;return K_OK;}
static void socket_registry_release(void){assert(reservations);--reservations;}
socket_private_t *socket_private(socket_t *s){return s?s->protocol_state:NULL;}
kstatus_t socket_close(socket_t *s){
 assert(s && s->protocol_state && live>=2 && reservations);
 socket_private_t *p=s->protocol_state;assert(!atomic_load(&p->closed));
 atomic_store(&p->closed,true);++closes;return close_fails?K_EIO:K_OK;
}
static void *allocate(size_t n,bool zero){
 ++calls;if(calls==fail_at)return NULL;
 void *p=malloc(n);assert(p);memset(p,zero?0:(int)poison,n);++live;return p;
}
void *kzalloc(size_t n,uint32_t flags){assert(!flags);return allocate(n,true);}
void *kmalloc(size_t n,uint32_t flags){assert(!flags);private_bytes=n;return allocate(n,false);}
void kfree(void *p){if(p){assert(live);--live;free(p);}}
void vfree(void *p){assert(!p);}
void socket_zero(void *p,size_t n){
 assert(n==offsetof(socket_private_t,stream_buffer));cleared+=n;memset(p,0,n);
}
void socket_copy(void *d,const void *s,size_t n){if(n)memmove(d,s,n);}
void object_header_init(object_header_t *h,object_type_id_t t,const object_ops_t *ops){
 memset(h,0,sizeof(*h));h->type=t;h->ops=ops;
}
void wait_queue_init(wait_queue_t *q){memset(q,0,sizeof(*q));}
''' + ring + allocation + destroy + r'''
static void release(socket_t *s){
 unsigned old_closes=closes;
 assert(s->object.ops->destroy==socket_destroy);
 s->object.ops->destroy(s);
 assert(!live && !reservations && closes==old_closes+1);
}
int main(void){
 const unsigned poisons[]={0,0x5a,0xa5,0xff};
 for(unsigned pattern=0;pattern<4;++pattern)
 for(unsigned family=0;family<2;++family)for(unsigned stream=0;stream<2;++stream){
  poison=poisons[pattern];calls=0;fail_at=0;cleared=0;close_fails=(pattern&1)!=0;
  socket_t *s=NULL;unsigned f=family?OS_AF_INET6:OS_AF_INET4;
  unsigned t=stream?OS_SOCK_STREAM:OS_SOCK_DGRAM;
  assert(socket_allocate(f,t,stream?SOCKET_PROTOCOL_TCP:SOCKET_PROTOCOL_UDP,&s)==K_OK);
  assert(calls==2U && reservations==1);
  assert(s && s->family==f && s->type==t && s->protocol_state==s->rx_queue && !s->tx_queue);
  assert(s->object.type==KOBJECT_TYPE_SOCKET && s->object.ops==&g_socket_object_ops);
  socket_private_t *p=s->protocol_state,*expected=calloc(1,sizeof(*expected));assert(expected);
  expected->tcp_rx_present=p->tcp_rx_present;
  expected->tcp_keepidle=7200;expected->tcp_keepintvl=75;expected->tcp_keepcnt=9;
  size_t control=offsetof(socket_private_t,stream_buffer);
  size_t allocation=sizeof(*p)+(stream?(SOCKET_STREAM_BUFFER_SIZE+7U)/8U:sizeof(socket_datagram_t)*SOCKET_RX_QUEUE_DEPTH);
  assert(private_bytes==allocation);
  assert(sizeof(*p)<65536 && allocation<131072);
  assert(cleared==control && control<512);
  assert(!memcmp(p,expected,control));free(expected);
  size_t poisoned=stream?sizeof(*p):allocation;
  for(size_t i=control;i<poisoned;++i)assert(((unsigned char *)p)[i]==poison);
  if(stream){
   assert(p->tcp_rx_present==(uint8_t *)(p+1));
   for(unsigned i=0;i<(SOCKET_STREAM_BUFFER_SIZE+7)/8;++i)assert(!p->tcp_rx_present[i]);
  }else assert(!p->tcp_rx_present);
  unsigned char input[65],output[65];for(unsigned i=0;i<65;++i)input[i]=(unsigned char)(i+1);
  p->stream_read=p->stream_write=SOCKET_STREAM_BUFFER_SIZE-17;
  unsigned next=socket_stream_write_locked(p,input,sizeof(input));
  assert(next==48 && socket_stream_read_locked(p,output,sizeof(output))==48);
  assert(!memcmp(input,output,sizeof(input)));
  release(s);
 }
 for(unsigned fault=1;fault<=3;++fault){
  calls=0;fail_at=fault;socket_t *s=NULL;
  kstatus_t rc=socket_allocate(OS_AF_INET4,OS_SOCK_STREAM,SOCKET_PROTOCOL_TCP,&s);
  if(fault<3)assert(rc==K_ENOMEM && !s && !live && !reservations && calls==fault);
  else {assert(rc==K_OK && s && calls==2 && ((socket_private_t *)s->protocol_state)->tcp_rx_present);release(s);}
 }
 calls=0;fail_at=0;reserve_fail=true;socket_t *s=NULL;
 assert(socket_allocate(OS_AF_INET6,OS_SOCK_DGRAM,SOCKET_PROTOCOL_UDP,&s)==K_ENOMEM && !s && !live);
 assert(!reservations && calls==2);
 unsigned old_closes=closes;socket_destroy(NULL);assert(closes==old_closes && !live && !reservations);
 printf("Socket allocation: %zu control bytes vs %zu base; 16 poisoned family/type layouts, ring wrap, combined bitmap, object/private OOM and registry failure PASS\n",
        offsetof(socket_private_t,stream_buffer),sizeof(socket_private_t));
 puts("Production destructor: close/registry release, combined private/bitmap ownership, close failures and null destruction PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='socket-allocation-') as directory:
    binary = directory+'/test'
    command = ['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
               '-I'+str(root/'include'),'-I'+str(root/'kernel/net'),
               '-fsanitize=address,undefined','-fno-pie','-no-pie',
               '-x','c','-','-o',binary]
    subprocess.run(command,input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    for name, old, new in (
        ('full private clearing', 'socket_zero(private, offsetof(socket_private_t, stream_buffer));',
         'socket_zero(private, sizeof(*private));'),
        ('uninitialized metadata', 'socket_zero(private, offsetof(socket_private_t, stream_buffer));',
         'socket_zero(private, 0);'),
        ('implicit full-buffer initialization',
         'kmalloc(private_size, 0)', 'kzalloc(private_size, 0)'),
        ('corrupted initialized metadata',
         'socket_zero(private, offsetof(socket_private_t, stream_buffer));',
         'socket_zero(private, offsetof(socket_private_t, stream_buffer)); private->listening=true;'),
        ('UDP allocation retained on TCP', 'if (type == OS_SOCK_DGRAM)', 'if (true)'),
        ('UDP tail omitted', 'if (type == OS_SOCK_DGRAM)', 'if (false)'),
        ('allocation continues after object OOM',
         'if (socket == 0) return K_ENOMEM;',
         'if (socket == 0) { void *waste=kmalloc(sizeof(socket_private_t),0); kfree(waste); return K_ENOMEM; }'),
        ('bitmap initialization omitted',
         'memset(private->tcp_rx_present, 0,', 'memset(private->tcp_rx_present, 0xa5,'),
        ('destructor omits registry release',
         '    socket_registry_release();', '    if (false) socket_registry_release();'),
        ('destructor omits close',
         '    (void)socket_close(socket);', '    if (false) (void)socket_close(socket);'),
    ):
        broken=program.replace(old,new)
        assert broken!=program,name
        subprocess.run(command,input=broken,text=True,check=True)
        result=subprocess.run([binary],text=True,capture_output=True,timeout=30,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert result.returncode==-signal.SIGABRT and 'Assertion' in result.stderr,(name,result)
        print('Negative control rejected:',name)
