"""Exercise production pre-connect ring ownership, including allocation races.

This is not a window-scaling/throughput test: connection integration is separate.
"""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/net/socket.c').read_text()
reserve = source[source.index('static kstatus_t socket_stream_resize_status('):
                 source.index('kstatus_t socket_allocate(')]
ring = source[source.index('uint32_t socket_stream_write_locked('):
              source.index('static uint32_t socket_active_ipv4_address(')]
destroy = source[source.index('static void socket_destroy('):
                 source.index('static void socket_handle_close(')]
program = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <kernel/net_core.h>
#include "socket_model.h"
#include "tcp_reassembly.h"
static unsigned locked, allocations, releases, live, race;
static bool fail;
static socket_t *subject;
static void *owned[8];
socket_private_t *socket_private(socket_t *s){return s?s->protocol_state:NULL;}
void socket_lock(spinlock_t *l){assert(l && !locked);locked=1;}
void socket_unlock(spinlock_t *l){assert(l && locked);locked=0;}
void socket_copy(void *d,const void *s,size_t n){if(n)memmove(d,s,n);}
void *vmalloc(size_t n){
 assert(!locked);++allocations;if(fail)return NULL;
 unsigned action=race;race=0;
 if(action==1)atomic_store(&socket_private(subject)->closed,true);
 if(action==2)socket_private(subject)->connected=true;
 if(action==3)assert(socket_stream_reserve(subject,262144)==K_OK);
 void *p=malloc(n);assert(p);memset(p,0xa5,n);
 unsigned slot=0;while(slot<8 && owned[slot])++slot;assert(slot<8);
 owned[slot]=p;++live;return p;
}
void vfree(void *p){
 assert(!locked && p);unsigned slot=0;while(slot<8 && owned[slot]!=p)++slot;
 assert(slot<8 && live);owned[slot]=NULL;--live;++releases;free(p);
}
void kfree(void *p){assert(!locked);free(p);}
kstatus_t socket_close(socket_t *s){assert(!locked);atomic_store(&socket_private(s)->closed,true);return K_OK;}
static void socket_registry_release(void){assert(!locked);}
''' + reserve + ring + destroy + r'''
static socket_t *fresh(unsigned family){
 socket_t *s=calloc(1,sizeof(*s));assert(s);
 s->object.type=KOBJECT_TYPE_SOCKET;s->type=OS_SOCK_STREAM;s->family=family;
 s->protocol_state=calloc(1,sizeof(socket_private_t)+4096);assert(s->protocol_state);
 socket_private_t *p=socket_private(s);p->tcp_rx_present=(uint8_t *)(p+1);
 subject=s;return s;
}
static void check_ring(socket_private_t *p,unsigned cap){
 assert(socket_stream_capacity(p)==cap && p->tcp_rx_present==p->stream_dynamic+cap);
 for(unsigned i=0;i<(cap+7)/8;++i)assert(!p->tcp_rx_present[i]);
 for(unsigned i=0;i<cap;++i)assert(p->stream_dynamic[i]==0xa5);
 unsigned char *in=malloc(cap),*out=malloc(cap);assert(in && out);
 for(unsigned i=0;i<cap;++i)in[i]=(unsigned char)(i*13+i/251);
 const unsigned cursors[]={0,1,cap-17,cap-1};
 const unsigned lengths[]={0,1,17,32769,cap-1,cap};
 for(unsigned a=0;a<4;++a)for(unsigned b=0;b<6;++b){
  p->stream_read=p->stream_write=cursors[a];unsigned n=lengths[b];
  assert(socket_stream_write_locked(p,in,n)==(cursors[a]+n)%cap);
  assert(socket_stream_read_locked(p,out,n)==(cursors[a]+n)%cap);
  assert(!memcmp(in,out,n));
  assert(p->stream_read==cursors[a] && p->stream_write==cursors[a]);
 }
 /* Retain a suffix past the old 32KiB boundary, including sequence and
  * ring wrap. Closing its hole must publish one full, ordered buffer. */
 p->stream_read=p->stream_write=cap-17;
 uint32_t base=UINT32_MAX-12345U;
 bool notify=true;
 assert(socket_tcp_reassemble_locked(p,base+32769,base,0,
                                    in+32769,cap-32769,&notify)==K_OK);
 assert(!notify && !atomic_load(&p->stream_count));
 assert(p->tcp_rx_future_count==cap-32769);
 assert(socket_tcp_reassemble_locked(p,base,base,0,in,32769,&notify)==K_OK);
 assert(notify && atomic_load(&p->stream_count)==cap && !p->tcp_rx_future_count);
 assert(p->tcp_next_sequence==base+cap);
 socket_stream_read_locked(p,out,cap);assert(!memcmp(in,out,cap));
 for(unsigned i=0;i<(cap+7)/8;++i)assert(!p->tcp_rx_present[i]);
 atomic_store(&p->stream_count,0);
 free(in);free(out);
}
int main(void){
 assert(socket_stream_reserve(NULL,65536)==K_EINVAL);
 for(unsigned family=0;family<2;++family){
  socket_t *s=fresh(family?OS_AF_INET6:OS_AF_INET4);socket_private_t *p=socket_private(s);
  const unsigned invalid[]={0,1,32767,32769,65535,8388608,UINT32_MAX};
  for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i)
   assert(socket_stream_reserve(s,invalid[i])==K_EINVAL);
  assert(!live);unsigned before=allocations;
  assert(socket_stream_reserve(s,32768)==K_OK && allocations==before);
  s->type=OS_SOCK_DGRAM;assert(socket_stream_reserve(s,65536)==K_EINVAL);s->type=OS_SOCK_STREAM;
  s->family=UINT16_MAX;assert(socket_stream_reserve(s,65536)==K_EINVAL);s->family=family?OS_AF_INET6:OS_AF_INET4;
  fail=true;uint8_t *bits=p->tcp_rx_present;
  assert(socket_stream_reserve(s,65536)==K_ENOMEM && !live && p->tcp_rx_present==bits);
  fail=false;
  for(unsigned state=0;state<6;++state){
   if(state==0)p->connected=true;
   if(state==1)p->listening=true;
   if(state==2)p->tcp_state=SOCKET_TCP_SYN_SENT;
   if(state==3)atomic_store(&p->stream_count,1);
   if(state==4)p->tcp_rx_future_count=1;
   if(state==5)p->tcp_rx_fin_pending=true;
   before=allocations;assert(socket_stream_reserve(s,65536)==K_EBUSY && allocations==before);
   p->connected=p->listening=p->tcp_rx_fin_pending=false;
   p->tcp_state=SOCKET_TCP_CLOSED;p->tcp_rx_future_count=0;atomic_store(&p->stream_count,0);
  }
  const unsigned sizes[]={65536,262144,SOCKET_STREAM_MAX_CAPACITY};
  for(unsigned i=0;i<3;++i){
   assert(socket_stream_reserve(s,sizes[i])==K_OK && live==1);check_ring(p,sizes[i]);
   before=allocations;assert(socket_stream_reserve(s,sizes[i])==K_OK && allocations==before);
  }
  socket_destroy(s);assert(!live);
 }
 for(unsigned action=1;action<=3;++action){
  socket_t *s=fresh(OS_AF_INET4);race=action;
  kstatus_t rc=socket_stream_reserve(s,65536);
  if(action==1)assert(rc==K_EDEVREMOVED && !live);
  if(action==2)assert(rc==K_EBUSY && !live);
  if(action==3){assert(rc==K_OK && live==1);check_ring(socket_private(s),262144);}
  socket_destroy(s);assert(!live);
 }
 assert(!locked);socket_destroy(NULL);
 printf("Dynamic stream reserve: IPv4/IPv6 64KiB..4MiB wrap, bitmap, OOM, growth, close/connect/competing-grow races and exact frees PASS (%u allocations, %u frees)\n",allocations,releases);
}
'''
with tempfile.TemporaryDirectory(prefix='socket-reserve-') as directory:
    binary = directory + '/test'
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-I'+str(root/'include'), '-I'+str(root/'kernel/net'),
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    for name, old, new in (
        ('bitmap initialization omitted',
         'memset(storage + capacity, 0, (capacity + 7U) / 8U);',
         'memset(storage + capacity, 0xa5, (capacity + 7U) / 8U);'),
        ('allocation-time state recheck omitted',
         'status = socket_stream_resize_status(private);\n    if (status == K_OK',
         'status = K_OK;\n    if (status == K_OK'),
        ('retired allocation leaked',
         'if (retired != 0) vfree(retired);',
         'if (false && retired != 0) vfree(retired);'),
        ('dynamic destructor ownership omitted',
         'vfree(private->stream_dynamic);',
         'if (false) vfree(private->stream_dynamic);'),
    ):
        broken = program.replace(old, new)
        assert broken != program, name
        subprocess.run(command, input=broken, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, text=True, timeout=30,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert result.returncode == -signal.SIGABRT and 'Assertion' in result.stderr, (name, result)
        print('Negative control rejected:', name)
