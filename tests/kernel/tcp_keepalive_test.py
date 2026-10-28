"""Exercise actual keepalive options, transport and IPv4/IPv6 ACK handlers."""
from pathlib import Path
import resource
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
def function(file,signature):
    text=(root/file).read_text()
    start=text.index(signature)
    end=text.index('{',start)+1
    depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]
model=(root/'kernel/net/socket_model.h').read_text()
private=model[model.index('typedef struct socket_private {'):model.index('} socket_private_t;')+len('} socket_private_t;')]
capacity_helpers=model[model.index('#define SOCKET_STREAM_MAX_CAPACITY'):
                       model.index('typedef struct socket_binding {')]
public=(root/'include/kernel/socket.h').read_text()
output_types=public[public.index('typedef struct socket_tcp_reply {'):public.index('/* UDP 输出')]
batch_output='\n'.join(line for line in (root/'kernel/net/tcp_output.h').read_text().splitlines()
                       if not line.startswith(('#include', '#pragma')))
local_retry='\n'.join(line for line in (root/'kernel/net/tcp_retry.h').read_text().splitlines()
                      if not line.startswith(('#include', '#pragma')))
protocol=(root/'kernel/net/socket_protocol.c').read_text()
# Both wire families must dispatch RST before SYN, ACK and payload branches.
runtime=(root/'kernel/drivers/net/runtime.c').read_text()
for family in ('4','6'):
    start=runtime.index('packet.protocol == SOCKET_PROTOCOL_TCP && packet.family == OS_AF_INET'+family)
    reset_branch=runtime.index('if ((packet.flags & NET_TCP_FLAG_RST) != 0U)',start)
    syn_branch=runtime.index('if ((packet.flags & NET_TCP_FLAG_SYN) != 0U',start)
    assert reset_branch < syn_branch
    assert 'socket_inject_tcp_ack_ipv'+family+'_reply(' in runtime[reset_branch:syn_branch]
inheritance=[]
for family in ('4','6'):
    start=protocol.index('child_private = socket_private(child);',
                         protocol.index('kstatus_t socket_inject_tcp_syn_ipv'+family+'('))
    start=protocol.index('socket_lock(&listener->lock);',start)
    end=protocol.index('socket_unlock(&listener->lock);',start)+len('socket_unlock(&listener->lock);')
    inheritance.append(f'void inherit{family}(socket_t *listener, socket_private_t *listener_private, socket_private_t *child_private) {{\n'+protocol[start:end]+'\n}')
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "uapi/socket.h"
#define OS_DEBUG_SERIAL 0
#define K_OK 0
#define K_EINVAL -22
#define K_EIO -5
#define K_ECONNRESET -104
#define K_ECONNREFUSED -111
#define K_EDEVREMOVED -2001
#define K_ETIMEDOUT -110
#define NET_TCP_FLAG_RST 4
#define K_ENOENT -2
#define K_EAGAIN -11
#define KOBJECT_TYPE_SOCKET 1
#define SOCKET_PROTOCOL_TCP 6
#define SOCKET_TCP_CLOSED 0
#define SOCKET_TCP_SYN_RECEIVED 1
#define SOCKET_TCP_ESTABLISHED 2
#define SOCKET_TCP_SYN_SENT 3
#define SOCKET_BINDING_COUNT 1
#define SOCKET_MAX_PAYLOAD 1440
#define SOCKET_RX_QUEUE_DEPTH 4
#define SOCKET_STREAM_BUFFER_SIZE 8192
#define SOCKET_LISTEN_BACKLOG 4
#define SOCKET_TCP_MAX_RETRANSMITS 3
#define SOCKET_POLL_READ 1
#define SOCKET_POLL_HUP 2
#define SOCKET_POLL_ERROR 4
#define SOCKET_POLL_WRITE 8
#define NET_TCP_FLAG_ACK 16
#define NET_TCP_FLAG_SYN 2
#define NET_TCP_FLAG_FIN 1
#define NET_TCP_FLAG_PSH 8
typedef int kstatus_t;
typedef int spinlock_t;
typedef struct socket {
 struct {int type;} object;
 unsigned type,family,protocol;int lock,waitq,refs;void *protocol_state;
} socket_t;
typedef struct {int unused;} socket_datagram_t;
''' + private + '\n' + capacity_helpers + output_types + batch_output + r'''
typedef struct {socket_t *socket;} socket_tcp_connection_t;
typedef struct {socket_t *socket;} socket_tcp6_connection_t;
static socket_t sockets[2];static socket_private_t states[2];
static socket_tcp_connection_t conn4;static socket_tcp6_connection_t conn6;
static uint64_t now=1000000000ULL;
static int locked,wakes,notifications,outputs[2];
static bool fail_outputs, missing_outputs;
static int fatal_output;
static bool capture_data;
static uint8_t captured[SOCKET_MAX_PAYLOAD];
static size_t captured_length;
static uint32_t last_sequence[2],last_ack[2];
static uint64_t x86_read_tsc(void){return now;}
static uint64_t x86_timeout_ns_to_tsc(uint64_t ns){return ns;}
static uint64_t socket_tcp_retransmit_deadline(uint64_t tick){return tick+1000000000ULL;}
static void socket_initialize_globals(void){}
static void socket_lock(spinlock_t *lock){assert(!locked);(void)lock;locked=1;}
static void socket_unlock(spinlock_t *lock){assert(locked);(void)lock;locked=0;}
static int binding_lock;
static spinlock_t *socket_binding_lock(void){return &binding_lock;}
static uint32_t socket_registry_capacity_locked(void){assert(locked);return SOCKET_BINDING_COUNT;}
static const socket_tcp_connection_t *socket_tcp_connection_at(uint32_t i){assert(i==0);return &conn4;}
static const socket_tcp6_connection_t *socket_tcp6_connection_at(uint32_t i){assert(i==0);return &conn6;}
static int32_t socket_find_tcp_locked(uint32_t a,uint16_t b,uint32_t c,uint16_t d){assert(locked);(void)a;(void)b;(void)c;(void)d;return conn4.socket?0:-1;}
static int32_t socket_find_tcp6_locked(const uint8_t *a,uint16_t b,const uint8_t *c,uint16_t d){assert(locked);(void)a;(void)b;(void)c;(void)d;return conn6.socket?0:-1;}
static bool socket_address6_zero(const uint8_t *a){uint8_t v=0;for(int i=0;i<16;++i)v|=a[i];return !v;}
static socket_private_t *socket_private(socket_t *s){return s?s->protocol_state:0;}
static const socket_private_t *socket_private_const(const socket_t *s){return s?s->protocol_state:0;}
typedef struct { socket_private_t *private; } socket_wait_context_t;
static bool object_try_get(socket_t *s){++s->refs;return true;}
static void object_put(socket_t *s){assert(s->refs>0);--s->refs;}
static void object_notify_changed(socket_t *s){assert(!locked && s);++notifications;}
static void object_notify_signaled(socket_t *s){object_notify_changed(s);}
static int wake_all(int *q){assert(!locked && q);++wakes;return 0;}
static void socket_copy(void *a,const void *b,size_t n){memmove(a,b,n);}
static void socket_zero(void *a,size_t n){memset(a,0,n);}
static kstatus_t output4(void *ctx,uint32_t src,uint16_t sp,uint32_t dst,uint16_t dp,
 uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *payload,size_t length){
 (void)ctx;assert(!locked && src && dst && sp && dp);
 if(capture_data){
  assert(flags==(NET_TCP_FLAG_ACK|NET_TCP_FLAG_PSH) && length<=sizeof(captured));
  memcpy(captured,payload,length);captured_length=length;
 }else assert(flags==NET_TCP_FLAG_ACK && length==0);
 assert(window==8192);
 ++outputs[0];last_sequence[0]=seq;last_ack[0]=ack;return fatal_output?fatal_output:(fail_outputs?K_EAGAIN:0);
}
static kstatus_t output6(void *ctx,const uint8_t *src,uint16_t sp,const uint8_t *dst,uint16_t dp,
 uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *payload,size_t length){
 (void)src;(void)dst;int status=output4(ctx,1,sp,2,dp,seq,ack,flags,window,payload,length);
 --outputs[0];++outputs[1];last_sequence[1]=seq;last_ack[1]=ack;return status;
}
static void socket_tcp_output_get(socket_tcp_ipv4_output_fn *fn,void **ctx){*fn=missing_outputs?0:output4;*ctx=0;}
static void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *fn,void **ctx){*fn=missing_outputs?0:output6;*ctx=0;}
''' + function('kernel/net/socket_model.h','static inline void socket_tcp_fail_locked(') + function('kernel/net/socket_model.h','static inline void socket_tcp_ack_retry_locked(') + local_retry + '\n'.join(function('kernel/net/socket.c',signature) for signature in (
    'static uint64_t socket_tcp_keepalive_deadline(',
    'void socket_tcp_keepalive_activity_locked(',
    'bool socket_tcp_keepalive_poll_locked(',
    'kstatus_t socket_get_option(', 'kstatus_t socket_set_option(',
    'static bool socket_is_signaled(',
    'uint32_t socket_poll_state(', 'kstatus_t socket_connect_status(')) + '\n' + function('kernel/net/socket_io.c','static bool socket_receive_ready(') + '\n' + '\n'.join(line for line in (root/'kernel/net/socket_transport.c').read_text().splitlines() if not line.startswith('#include')) + '\n' + '\n'.join(
    function('kernel/net/socket_protocol.c',signature) for signature in (
        'static bool socket_tcp_reset_locked(',
        'static kstatus_t socket_tcp_ack_target(',
        'kstatus_t socket_inject_tcp_ack_ipv4_reply(',
        'kstatus_t socket_inject_tcp_ack_ipv6_reply(')) + '\n' + '\n'.join(inheritance) + r'''
static void reset(void) {
 memset(states,0,sizeof(states));memset(sockets,0,sizeof(sockets));
 now=1000000000ULL;wakes=notifications=outputs[0]=outputs[1]=0;fail_outputs=false;missing_outputs=false;fatal_output=0;capture_data=false;captured_length=0;
 for(int i=0;i<2;++i){
  sockets[i]=(socket_t){.object={KOBJECT_TYPE_SOCKET},.family=i?OS_AF_INET6:OS_AF_INET4,
   .type=OS_SOCK_STREAM,.protocol=SOCKET_PROTOCOL_TCP,.protocol_state=&states[i]};
  states[i].tcp_state=SOCKET_TCP_ESTABLISHED;states[i].connected=true;
  states[i].tcp_send_next_sequence=1000;states[i].tcp_next_sequence=2000;
  states[i].tcp_keepidle=7200;states[i].tcp_keepintvl=75;states[i].tcp_keepcnt=9;
  states[i].local_address=1;states[i].peer_address=2;
  states[i].local_port=123;states[i].peer_port=456;
  states[i].tcp_peer_window=8192;
 }
 conn4.socket=&sockets[0];conn6.socket=&sockets[1];
}
static void configure(void){
 for(int i=0;i<2;++i){
  assert(socket_set_option(&sockets[i],OS_SOCKET_OPTION_KEEPALIVE,1)==0);
  assert(socket_set_option(&sockets[i],OS_SOCKET_OPTION_KEEPIDLE,2)==0);
  assert(socket_set_option(&sockets[i],OS_SOCKET_OPTION_KEEPINTVL,1)==0);
  assert(socket_set_option(&sockets[i],OS_SOCKET_OPTION_KEEPCNT,2)==0);
 }
}
int main(void){
 // Stale/future ACKs and reordered segments cannot overwrite a newer window.
 // Exercise both ingress families, zero/max windows and both sequence spaces
 // wrapping independently. Valid ACK progress still retires retained data
 // even when its older window advertisement must be ignored.
 unsigned window_cases=0;
 for(unsigned family=0;family<2;++family)
 for(unsigned wrap=0;wrap<4;++wrap)
 for(int seq_step=-1;seq_step<=1;++seq_step)
 for(unsigned ack_case=0;ack_case<5;++ack_case)
 for(unsigned window_case=0;window_case<3;++window_case){
  reset();socket_private_t *p=&states[family];
  uint32_t una=(wrap&1)?UINT32_MAX-49U:1000U;
  uint32_t last_sequence=(wrap&2)?UINT32_MAX:2000U;
  uint32_t acks[]={una-1U,una,una+50U,una+100U,una+101U};
  uint16_t windows[]={0,8192,UINT16_MAX};
  uint32_t seq=last_sequence+(uint32_t)seq_step,ack=acks[ack_case];
  p->tcp_send_pending=true;p->tcp_send_length=100;
  p->tcp_send_unacknowledged=una;p->tcp_send_next_sequence=una+100U;
  p->tcp_send_flags=NET_TCP_FLAG_ACK|NET_TCP_FLAG_PSH;
  p->tcp_window_sequence=last_sequence;
  p->tcp_window_acknowledgement=una+50U;
  p->tcp_peer_window=4096;
  const uint8_t src[16]={2},dst[16]={1};socket_tcp_reply_t reply={0};
  int rc=family?socket_inject_tcp_ack_ipv6_reply(src,456,dst,123,seq,ack,NET_TCP_FLAG_ACK,windows[window_case],&reply):
      socket_inject_tcp_ack_ipv4_reply(2,456,1,123,seq,ack,NET_TCP_FLAG_ACK,windows[window_case],&reply);
  bool valid_ack=ack_case>=1 && ack_case<=3;
  bool fresh=seq_step>0 || (seq_step==0 && ack_case>=2);
  bool updated=valid_ack && fresh;
  assert(!rc && !locked && !sockets[family].refs);
  assert(p->tcp_peer_window==(updated?windows[window_case]:4096));
  assert(p->tcp_window_sequence==(updated?seq:last_sequence));
  assert(p->tcp_window_acknowledgement==(updated?ack:una+50U));
  assert(p->tcp_send_unacknowledged==(valid_ack?ack:una));
  if(ack_case==1)assert(notifications==(int)(updated && windows[window_case]>4096));
  ++window_cases;
 }
 printf("TCP window freshness: %u IPv4/IPv6 stale/future/duplicate ACK, reordered/wrapped sequence and zero/max window cases PASS\n",window_cases);
 // Cumulative progress retires only the acknowledged prefix, including
 // sequence wrap, without losing the retained suffix or its local retry.
 for(unsigned family=0;family<2;++family)for(unsigned wrap=0;wrap<2;++wrap)
 for(unsigned amount=1;amount<32;++amount){
  reset();socket_private_t *p=&states[family];
  uint32_t base=wrap?UINT32_MAX-15:1000;
  p->tcp_send_unacknowledged=base;p->tcp_send_next_sequence=base+32;
  p->tcp_send_pending=true;p->tcp_send_length=32;
  p->tcp_send_flags=NET_TCP_FLAG_ACK|NET_TCP_FLAG_PSH;
  p->tcp_send_retransmits=2;p->tcp_send_deadline_tsc=now+1;
  p->tcp_local_retry_tsc=now+17;
  for(unsigned i=0;i<32;++i)p->tcp_send_payload[i]=(uint8_t)(i+3);
  const uint8_t source[16]={2},destination[16]={1};
  int rc=family?socket_inject_tcp_ack_ipv6_reply(source,456,destination,123,2000,base+amount,NET_TCP_FLAG_ACK,8192,0):
   socket_inject_tcp_ack_ipv4_reply(2,456,1,123,2000,base+amount,NET_TCP_FLAG_ACK,8192,0);
  assert(!rc && p->tcp_send_pending && p->tcp_send_length==32-amount);
  assert(p->tcp_send_unacknowledged==base+amount && p->tcp_send_next_sequence==base+32);
  for(unsigned i=0;i<32-amount;++i)assert(p->tcp_send_payload[i]==i+amount+3);
  assert(!p->tcp_send_retransmits && p->tcp_send_deadline_tsc==socket_tcp_retransmit_deadline(now));
  assert(p->tcp_local_retry_tsc==now+17 && wakes==1 && notifications==1);
  assert(!sockets[family].refs && !locked);
  socket_private_t saved=*p;
  const uint32_t rejected[]={base-1,base+amount,base+33};
  for(unsigned i=0;i<3;++i){
   rc=family?socket_inject_tcp_ack_ipv6_reply(source,456,destination,123,2000,rejected[i],NET_TCP_FLAG_ACK,8192,0):
    socket_inject_tcp_ack_ipv4_reply(2,456,1,123,2000,rejected[i],NET_TCP_FLAG_ACK,8192,0);
   assert(!rc && !memcmp(p,&saved,sizeof(saved)) && wakes==1);
  }
  // A completion after ACK progress still owns the remaining suffix. An
  // already-ACKed or future span cannot fail it or alter the retry timer.
  assert(!socket_tcp_local_output_locked(p,base,base+amount,K_EIO,now));
  assert(!socket_tcp_local_output_locked(p,base,base+33,K_EIO,now));
  assert(socket_tcp_local_output_locked(p,base,base+32,K_EIO,now));
  assert(atomic_load(&p->tcp_error)==K_EIO && p->tcp_state==SOCKET_TCP_CLOSED);
  *p=saved;
  p->tcp_local_retry_tsc=0;
  assert(!socket_tcp_local_output_locked(p,base,base+32,K_EAGAIN,now));
  assert(p->tcp_local_retry_tsc==now+20000000ULL);
  p->tcp_local_retry_tsc=now+17;
  assert(!socket_tcp_local_output_locked(p,base,base+32,K_EAGAIN,now));
  assert(p->tcp_local_retry_tsc==now+17 && !atomic_load(&p->tcp_error));
  if(amount<31){
   assert(!socket_tcp_local_output_locked(p,base,base+31,K_OK,now));
   assert(p->tcp_local_retry_tsc==now+17);
  }
  capture_data=true;socket_tcp_poll(now+17);
  assert(captured_length==32-amount && last_sequence[family]==base+amount);
  for(unsigned i=0;i<captured_length;++i)assert(captured[i]==i+amount+3);
  assert(!p->tcp_local_retry_tsc && !p->tcp_send_retransmits);
  rc=family?socket_inject_tcp_ack_ipv6_reply(source,456,destination,123,2000,base+32,NET_TCP_FLAG_ACK,8192,0):
   socket_inject_tcp_ack_ipv4_reply(2,456,1,123,2000,base+32,NET_TCP_FLAG_ACK,8192,0);
  assert(!rc && !p->tcp_send_pending && !p->tcp_send_length && !p->tcp_local_retry_tsc);
  assert(wakes==2 && notifications==2 && !sockets[family].refs && !locked);
 }
 // An expired local retry is consumed, not retained as a tight-loop deadline
 // when another output completion reports backpressure later.
 reset();states[0].tcp_send_pending=true;
 states[0].tcp_send_unacknowledged=900;states[0].tcp_send_next_sequence=1000;
 states[0].tcp_send_deadline_tsc=now+1000000000ULL;
 states[0].tcp_local_retry_tsc=now-1;
 assert(!socket_tcp_local_output_locked(&states[0],900,1000,K_EAGAIN,now));
 assert(states[0].tcp_local_retry_tsc==now+20000000ULL);
 // Both wire entry points must preserve handshake, completion and reference
 // ownership, with/without reply storage and across sequence-number wrap.
 for(unsigned family=0;family<2;++family)
 for(unsigned phase=0;phase<3;++phase)
 for(unsigned valid=0;valid<2;++valid)
 for(unsigned wrap=0;wrap<2;++wrap)
 for(unsigned window_case=0;window_case<3;++window_case)
 for(unsigned storage=0;storage<2;++storage){
  reset();socket_private_t *p=&states[family];
  const uint16_t windows[]={0,8192,UINT16_MAX};
  uint16_t window=windows[window_case];
  p->tcp_peer_window=window;
  p->tcp_state=phase==0?SOCKET_TCP_SYN_SENT:phase==1?SOCKET_TCP_SYN_RECEIVED:SOCKET_TCP_ESTABLISHED;
  p->tcp_initial_sequence=wrap?UINT32_MAX:999;
  p->tcp_peer_initial_sequence=wrap?UINT32_MAX:1999;
  p->tcp_send_next_sequence=p->tcp_initial_sequence+1U;
  p->tcp_next_sequence=p->tcp_peer_initial_sequence+1U;
  p->tcp_send_pending=true;p->tcp_send_length=17;
  p->tcp_send_flags=NET_TCP_FLAG_ACK;p->tcp_send_retransmits=2;
  p->tcp_send_deadline_tsc=now+100;
  if(phase==1){p->listener=&sockets[1-family];sockets[1-family].refs=1;}
  uint32_t seq=phase==0?p->tcp_peer_initial_sequence:p->tcp_next_sequence;
  uint32_t ack=p->tcp_send_next_sequence-(valid?0U:1U);
  uint8_t flags=NET_TCP_FLAG_ACK|(phase==0?NET_TCP_FLAG_SYN:0);
  const uint8_t source[16]={2},destination[16]={1};
  socket_tcp_reply_t reply={0};socket_tcp_reply_t *out=storage?&reply:0;
  int rc=family?socket_inject_tcp_ack_ipv6_reply(source,456,destination,123,seq,ack,flags,window,out):
   socket_inject_tcp_ack_ipv4_reply(2,456,1,123,seq,ack,flags,window,out);
  assert(rc==((valid||phase==2)?K_OK:K_EAGAIN));
  assert(!locked && !sockets[family].refs);
  assert(p->tcp_peer_window==window);
  assert(wakes==(int)valid && notifications==(int)valid);
  assert(p->tcp_send_pending==!valid);
  if(valid && phase<2){
   assert(p->tcp_window_sequence==seq && p->tcp_window_acknowledgement==ack);
   assert(p->tcp_send_unacknowledged==ack);
  }
  if(valid){
   assert(p->tcp_state==SOCKET_TCP_ESTABLISHED);
   assert(!p->tcp_send_length && !p->tcp_send_flags && !p->tcp_send_retransmits && !p->tcp_send_deadline_tsc);
   if(phase==1)assert(!p->listener && !sockets[1-family].refs);
   if(phase==0 && storage)assert(reply.valid && reply.sequence==p->tcp_send_next_sequence &&
    reply.acknowledgement==seq+1U && reply.flags==NET_TCP_FLAG_ACK && reply.window==8192);
  }else{
   assert(p->tcp_send_length==17 && p->tcp_send_retransmits==2 && p->tcp_send_deadline_tsc==now+100);
   if(phase==1)assert(p->listener==&sockets[1-family] && sockets[1-family].refs==1);
  }
  if(phase!=0 || !valid)assert(!reply.valid);
 }
 // A registry lookup may pin a socket just before close() unregisters it.
 // Late ACK/SYN-ACK/RST packets must not alter the now-closed socket.
 for(unsigned family=0;family<2;++family)for(unsigned phase=0;phase<4;++phase){
  reset();socket_private_t *p=&states[family];
  atomic_store(&p->closed,true);
  p->tcp_state=phase==0?SOCKET_TCP_SYN_SENT:phase==1?SOCKET_TCP_SYN_RECEIVED:SOCKET_TCP_ESTABLISHED;
  p->tcp_initial_sequence=999;p->tcp_peer_initial_sequence=1999;
  p->tcp_send_next_sequence=1000;p->tcp_next_sequence=2000;
  p->tcp_send_pending=true;p->tcp_send_length=17;p->tcp_peer_window=0;
  socket_private_t saved=*p;socket_tcp_reply_t reply={0};
  uint8_t flags=NET_TCP_FLAG_ACK|(phase==0?NET_TCP_FLAG_SYN:phase==3?NET_TCP_FLAG_RST:0);
  const uint8_t source[16]={2},destination[16]={1};
  int rc=family?socket_inject_tcp_ack_ipv6_reply(source,456,destination,123,2000,1000,flags,8192,&reply):
   socket_inject_tcp_ack_ipv4_reply(2,456,1,123,2000,1000,flags,8192,&reply);
  assert(!rc && !reply.valid && !memcmp(p,&saved,sizeof(saved)));
  assert(!wakes && !notifications && !sockets[family].refs && !locked);
 }
 // Local EAGAIN retries without waiting for RTO or spending its retry budget.
 reset();fail_outputs=true;
 for(unsigned i=0;i<2;++i) {
  states[i].tcp_send_pending=true;states[i].tcp_send_flags=NET_TCP_FLAG_ACK;
  states[i].tcp_send_unacknowledged=1000;
  states[i].tcp_send_next_sequence=1001;
  states[i].tcp_send_deadline_tsc=now+1000000000ULL;
  socket_tcp_local_output_locked(&states[i],1000,1001,K_EAGAIN,now);
  assert(states[i].tcp_local_retry_tsc==now+20000000ULL);
 }
 socket_tcp_poll(now+19999999ULL);assert(!outputs[0]&&!outputs[1]);
 now+=20000000ULL;socket_tcp_poll(now);
 for(unsigned i=0;i<2;++i)assert(outputs[i]==1 && !states[i].tcp_send_retransmits &&
  states[i].tcp_send_deadline_tsc==2000000000ULL && states[i].tcp_local_retry_tsc==now+20000000ULL);
 fail_outputs=false;now+=20000000ULL;socket_tcp_poll(now);
 for(unsigned i=0;i<2;++i)assert(outputs[i]==2 && !states[i].tcp_local_retry_tsc && !states[i].tcp_send_retransmits);
 // Later output completion cannot alter an ACKed or different flight.
 states[0].tcp_send_pending=false;
 socket_tcp_local_output_locked(&states[0],1000,1001,K_EAGAIN,now);
 assert(!states[0].tcp_local_retry_tsc);
 assert(!socket_tcp_local_output_locked(&states[0],1000,1001,K_EIO,now));
 assert(!atomic_load(&states[0].tcp_error));
 states[0].tcp_send_pending=true;states[0].tcp_send_unacknowledged=1001;
 states[0].tcp_send_next_sequence=1100;
 socket_tcp_local_output_locked(&states[0],1000,1001,K_EAGAIN,now);
 assert(!states[0].tcp_local_retry_tsc);
 assert(!socket_tcp_local_output_locked(&states[0],1000,1001,K_EIO,now));
 assert(states[0].tcp_send_pending && !atomic_load(&states[0].tcp_error));
 // A permanent local output failure aborts the current flight and notifies
 // immediately, preserving its errno instead of consuming the RTO budget.
 reset();fatal_output=K_EIO;
 for(unsigned i=0;i<2;++i){
  states[i].tcp_send_pending=true;states[i].tcp_send_flags=NET_TCP_FLAG_ACK;
  states[i].tcp_send_unacknowledged=1000;states[i].tcp_send_deadline_tsc=now+1;
  states[i].tcp_send_next_sequence=1001;
 }
 ++now;socket_tcp_poll(now);
 for(unsigned i=0;i<2;++i){
  assert(atomic_load(&states[i].tcp_error)==K_EIO);
  assert(states[i].tcp_state==SOCKET_TCP_CLOSED && !states[i].tcp_send_pending);
  assert(!states[i].tcp_send_deadline_tsc && !states[i].tcp_local_retry_tsc);
 }
 assert(wakes==2 && notifications==2 && outputs[0]==1 && outputs[1]==1);
 socket_tcp_poll(now+10000000000ULL);
 assert(wakes==2 && outputs[0]==1 && outputs[1]==1);
 // Permanent local failure remains bounded by the existing RTO budget.
 reset();fail_outputs=true;
 for(unsigned i=0;i<2;++i) {
  states[i].tcp_send_pending=true;states[i].tcp_send_flags=NET_TCP_FLAG_ACK;
  states[i].tcp_send_unacknowledged=1000;
  states[i].tcp_send_next_sequence=1001;
  states[i].tcp_send_deadline_tsc=now+1000000000ULL;
  socket_tcp_local_output_locked(&states[i],1000,1001,K_EAGAIN,now);
 }
 for(unsigned tick=1;tick<=200;++tick)socket_tcp_poll(now+tick*20000000ULL);
 for(unsigned i=0;i<2;++i)assert(states[i].tcp_send_failed && !states[i].tcp_send_pending &&
  states[i].tcp_send_retransmits==SOCKET_TCP_MAX_RETRANSMITS);
 assert(wakes==2 && notifications==2);
 reset();int32_t value;
 assert(socket_get_option(&sockets[0],OS_SOCKET_OPTION_KEEPALIVE,&value)==0 && !value);
 assert(socket_get_option(&sockets[0],OS_SOCKET_OPTION_KEEPIDLE,&value)==0 && value==7200);
 assert(socket_get_option(&sockets[0],OS_SOCKET_OPTION_KEEPINTVL,&value)==0 && value==75);
 assert(socket_get_option(&sockets[0],OS_SOCKET_OPTION_KEEPCNT,&value)==0 && value==9);
 socket_tcp_poll(now+10000000000000ULL);assert(!outputs[0] && !outputs[1]);
 configure();
 assert(socket_set_option(&sockets[0],OS_SOCKET_OPTION_KEEPIDLE,0)==K_EINVAL);
 assert(socket_set_option(&sockets[0],OS_SOCKET_OPTION_KEEPIDLE,32768)==K_EINVAL);
 assert(socket_set_option(&sockets[0],OS_SOCKET_OPTION_KEEPCNT,128)==K_EINVAL);
 assert(socket_set_option(&sockets[0],OS_SOCKET_OPTION_KEEPCNT,-1)==K_EINVAL);
 socket_tcp_poll(now+1999999999ULL);assert(!outputs[0] && !outputs[1]);
 now+=2000000000ULL;socket_tcp_poll(now);
 for(int i=0;i<2;++i){assert(outputs[i]==1 && last_sequence[i]==999 && last_ack[i]==2000);assert(states[i].tcp_send_next_sequence==1000 && !states[i].tcp_send_pending);}
 // Valid peer ACK resets idle/probe budget. Wrong sequence/future ACK does not.
 uint8_t a[16]={1},b[16]={2};
 assert(socket_inject_tcp_ack_ipv4_reply(2,456,1,123,2001,1000,NET_TCP_FLAG_ACK,8192,0)==0);
 assert(states[0].tcp_keep_probes==1);
 assert(socket_inject_tcp_ack_ipv4_reply(2,456,1,123,2000,1001,NET_TCP_FLAG_ACK,8192,0)==0);
 assert(states[0].tcp_keep_probes==1);
 assert(socket_inject_tcp_ack_ipv4_reply(2,456,1,123,2000,1000,NET_TCP_FLAG_ACK,8192,0)==0);
 assert(socket_inject_tcp_ack_ipv6_reply(b,456,a,123,2000,1000,NET_TCP_FLAG_ACK,8192,0)==0);
 assert(!states[0].tcp_keep_probes && !states[1].tcp_keep_probes);
 socket_tcp_poll(now+1000000000ULL);assert(outputs[0]==1 && outputs[1]==1);
 now+=2000000000ULL;socket_tcp_poll(now);assert(outputs[0]==2 && outputs[1]==2);
 now+=1000000000ULL;socket_tcp_poll(now);assert(outputs[0]==3 && outputs[1]==3);
 now+=1000000000ULL;socket_tcp_poll(now);
 for(int i=0;i<2;++i){assert(atomic_load(&states[i].tcp_error)==K_ETIMEDOUT);assert(socket_get_option(&sockets[i],OS_SOCKET_OPTION_ERROR,&value)==0 && value==110);assert(socket_poll_state(&sockets[i])&SOCKET_POLL_ERROR);assert(sockets[i].refs==0);}
 assert(wakes==2 && notifications==2);socket_tcp_poll(now+10000000000ULL);assert(wakes==2 && outputs[0]==3 && outputs[1]==3);
 assert(socket_is_signaled(&sockets[0]) && socket_is_signaled(&sockets[1]));
 socket_wait_context_t waiting={.private=&states[0]};
 assert(socket_receive_ready(&waiting));
 // Reset precedes ACK/window processing and wakes waiters without a timer.
 for(int family=0;family<2;++family){
  for(unsigned opening=0;opening<2;++opening){
   for(unsigned wrap=0;wrap<2;++wrap){
    reset();configure();socket_private_t *p=&states[family];
    p->tcp_state=opening?SOCKET_TCP_SYN_SENT:SOCKET_TCP_ESTABLISHED;
    p->tcp_send_next_sequence=wrap?0:1000;
    p->tcp_next_sequence=wrap?UINT32_MAX:2000;
    p->tcp_send_pending=true;p->tcp_send_length=100;
    p->tcp_send_deadline_tsc=now+100;p->tcp_local_retry_tsc=now+20;
    socket_tcp_reply_t reply={0};
    uint32_t seq=p->tcp_next_sequence,ack=p->tcp_send_next_sequence;
    // An in-window non-exact sequence needs a challenge, not a reset.
    if(!opening){
     int rc=family?socket_inject_tcp_ack_ipv6_reply(b,456,a,123,seq+1,ack,NET_TCP_FLAG_RST,0,&reply):
       socket_inject_tcp_ack_ipv4_reply(2,456,1,123,seq+1,ack,NET_TCP_FLAG_RST,0,&reply);
     assert(!rc && reply.valid && reply.flags==NET_TCP_FLAG_ACK && reply.acknowledgement==seq);
     assert(!atomic_load(&p->tcp_error) && p->tcp_send_pending && !wakes);
    }
    uint8_t flags=NET_TCP_FLAG_RST|(opening?NET_TCP_FLAG_ACK:0);
    int rc=family?socket_inject_tcp_ack_ipv6_reply(b,456,a,123,seq,ack,flags,0,&reply):
      socket_inject_tcp_ack_ipv4_reply(2,456,1,123,seq,ack,flags,0,&reply);
    assert(!rc && !reply.valid && p->tcp_state==SOCKET_TCP_CLOSED);
    assert(p->tcp_send_failed && !p->tcp_send_pending && !p->tcp_send_length);
    assert(!p->tcp_send_deadline_tsc && !p->tcp_local_retry_tsc && !p->tcp_keep_deadline_tsc);
    assert(atomic_load(&p->tcp_error)==(opening?K_ECONNREFUSED:K_ECONNRESET));
    assert(socket_connect_status(&sockets[family])==(opening?K_ECONNREFUSED:K_ECONNRESET));
    assert(socket_get_option(&sockets[family],OS_SOCKET_OPTION_ERROR,&value)==0 && value==(opening?111:104));
    waiting.private=p;assert(socket_receive_ready(&waiting) && socket_is_signaled(&sockets[family]));
    assert(socket_poll_state(&sockets[family])&SOCKET_POLL_ERROR);
    assert(wakes==1 && notifications==1 && sockets[family].refs==0);
    socket_tcp_poll(now+10000000000ULL);assert(!outputs[family]);
   }
  }
  reset();configure();states[family].tcp_state=SOCKET_TCP_SYN_SENT;
  socket_tcp_reply_t ignored={0};
  for(unsigned i=0;i<2;++i){
   uint8_t flags=NET_TCP_FLAG_RST|(i?NET_TCP_FLAG_ACK:0);
   int rc=family?socket_inject_tcp_ack_ipv6_reply(b,456,a,123,2000,999,flags,0,&ignored):
     socket_inject_tcp_ack_ipv4_reply(2,456,1,123,2000,999,flags,0,&ignored);
   assert(!rc && !ignored.valid && !atomic_load(&states[family].tcp_error));
   assert(!wakes && !notifications && states[family].tcp_state==SOCKET_TCP_SYN_SENT);
  }
 }
 // Ordinary SYN/data/FIN retransmission exhaustion must wake receivers too,
 // not merely signal poll while its blocking receive predicate stays false.
 for(unsigned phase=0;phase<3;++phase){
  reset();configure();
  for(unsigned family=0;family<2;++family){
   socket_private_t *p=&states[family];
   p->tcp_state=phase==0?SOCKET_TCP_SYN_SENT:SOCKET_TCP_ESTABLISHED;
   p->tcp_send_pending=true;p->tcp_send_length=17;
   p->tcp_fin_pending=phase==2;
   p->tcp_send_retransmits=SOCKET_TCP_MAX_RETRANSMITS;
   p->tcp_send_deadline_tsc=now+100;p->tcp_local_retry_tsc=now+20;
  }
  now+=100;socket_tcp_poll(now);
  for(unsigned family=0;family<2;++family){
   waiting.private=&states[family];
   assert(socket_receive_ready(&waiting));
   assert(socket_is_signaled(&sockets[family]));
   assert(socket_connect_status(&sockets[family])==-110);
   assert(socket_get_option(&sockets[family],OS_SOCKET_OPTION_ERROR,&value)==0 && value==110);
   assert(!states[family].tcp_send_pending && !states[family].tcp_send_deadline_tsc);
   assert(!states[family].tcp_local_retry_tsc && !states[family].tcp_fin_pending);
  }
  assert(wakes==2 && notifications==2);
  socket_tcp_poll(now+10000000000ULL);
  assert(wakes==2 && notifications==2 && !outputs[0] && !outputs[1]);
 }
 reset();configure();waiting.private=&states[0];
 assert(!socket_receive_ready(0));waiting.private=0;assert(!socket_receive_ready(&waiting));
 reset();configure();
 // Disable, local peers, FIN and closed sockets must not be probed.
 socket_set_option(&sockets[0],OS_SOCKET_OPTION_KEEPALIVE,0);
 states[1].peer=&sockets[0];socket_tcp_poll(now+10000000000ULL);assert(!outputs[0] && !outputs[1]);
 states[1].peer=0;states[1].tcp_fin_sent=true;socket_tcp_poll(now+10000000000ULL);assert(!outputs[1]);
 states[1].tcp_fin_sent=false;atomic_store(&states[1].peer_closed,true);socket_tcp_poll(now+10000000000ULL);assert(!outputs[1]);
 reset();configure();states[0].tcp_send_pending=true;states[1].tcp_send_pending=true;
 socket_tcp_poll(now+10000000000ULL);assert(!outputs[0] && !outputs[1] && !states[0].tcp_keep_probes);
 reset();configure();states[0].tcp_send_next_sequence=0;states[1].tcp_send_next_sequence=0;
 socket_tcp_poll(now+2000000000ULL);assert(last_sequence[0]==UINT32_MAX && last_sequence[1]==UINT32_MAX);
 assert(socket_tcp_keepalive_deadline(UINT64_MAX-10,1)==UINT64_MAX);
 reset();configure();fail_outputs=true;
 now+=2000000000ULL;socket_tcp_poll(now);
 assert(outputs[0]==1 && outputs[1]==1 && !states[0].tcp_keep_probes && !states[1].tcp_keep_probes);
 assert(!atomic_load(&states[0].tcp_error) && !wakes);
 fail_outputs=false;now+=1000000000ULL;socket_tcp_poll(now);
 assert(outputs[0]==2 && outputs[1]==2 && states[0].tcp_keep_probes==1 && states[1].tcp_keep_probes==1);
 uint64_t old_deadline=states[0].tcp_keep_deadline_tsc;
 socket_tcp_keepalive_activity_locked(&states[0],now);
 socket_tcp_keepalive_output_failed_locked(&states[0],old_deadline,now);
 assert(!states[0].tcp_keep_probes && states[0].tcp_keep_deadline_tsc==now+2000000000ULL);
 reset();configure();missing_outputs=true;
 now+=2000000000ULL;socket_tcp_poll(now);
 assert(!outputs[0] && !outputs[1] && !states[0].tcp_keep_probes && !states[1].tcp_keep_probes);
 assert(!socket_is_signaled(&sockets[0]) && !socket_is_signaled(&sockets[1]));
 waiting.private=&states[0];assert(!socket_receive_ready(&waiting));
 missing_outputs=false;now+=1000000000ULL;socket_tcp_poll(now);
 assert(outputs[0]==1 && outputs[1]==1 && states[0].tcp_keep_probes==1 && states[1].tcp_keep_probes==1);
 reset();configure();
 states[1].tcp_keepidle=11;states[1].tcp_keepintvl=12;states[1].tcp_keepcnt=13;
 states[0].tcp_keepalive=false;
 inherit4(&sockets[1],&states[1],&states[0]);
 assert(states[0].tcp_keepalive && states[0].tcp_keepidle==11 && states[0].tcp_keepintvl==12 && states[0].tcp_keepcnt==13);
 states[0].tcp_keepidle=0;inherit6(&sockets[1],&states[1],&states[0]);assert(states[0].tcp_keepidle==11);
 sockets[0].type=OS_SOCK_DGRAM;
 assert(socket_set_option(&sockets[0],OS_SOCKET_OPTION_KEEPIDLE,1)==K_EINVAL);
 assert(socket_get_option(&sockets[0],OS_SOCKET_OPTION_KEEPIDLE,&value)==K_EINVAL);
 assert(!locked && sockets[0].refs==0 && sockets[1].refs==0);
}
'''
# Exercise the actual pure-ACK driver branches as well as the ACK handlers:
# a correct protocol reply is useless if ingress still passes no reply buffer.
dispatch = r'''
typedef struct {unsigned unused;} e1000_state_t;
typedef struct {
 uint32_t source_address,destination_address,sequence,acknowledgement;
 uint8_t source_address6[16],destination_address6[16],flags;
 uint16_t source_port,destination_port,window;
 bool peer_window_scale_present;
 int peer_window_scale;
 uint16_t peer_mss;
} probe_packet_t;
/* Pure ACKs must never renegotiate SYN options in the shared ACK branch. */
#define socket_tcp_scale_ipv4(...) assert(0)
#define socket_tcp_scale_ipv6(...) assert(0)
#define socket_tcp_mss_ipv4(...) assert(0)
#define socket_tcp_mss_ipv6(...) assert(0)
static unsigned probe_replies;
static bool probe_output_failure;
static socket_tcp_reply_t probe_reply;
static bool e1000_send_tcp_reply(e1000_state_t *s,probe_packet_t *p,socket_tcp_reply_t *r){
 assert(s && p && r->valid && !locked);++probe_replies;probe_reply=*r;return !probe_output_failure;
}
'''
for family in ('4', '6'):
    start = runtime.index('packet.protocol == SOCKET_PROTOCOL_TCP && packet.family == OS_AF_INET'+family)
    start = runtime.index('packet.payload_length == 0U', start)
    opening = runtime.index('{', start)
    cursor, depth = opening+1, 1
    while depth:
        depth += (runtime[cursor] == '{')-(runtime[cursor] == '}')
        cursor += 1
    dispatch += ('static int probe_dispatch'+family+'(probe_packet_t packet){\n'
                 'e1000_state_t device={0},*state=&device;int status;\n'+
                 runtime[opening+1:cursor-1]+'\nreturn status;\n}\n')
probe_cases = r'''
 unsigned probe_cases=0;
 for(unsigned family=0;family<2;++family)for(unsigned wrap=0;wrap<2;++wrap)
 for(unsigned fill=0;fill<5;++fill)for(unsigned offset=0;offset<4;++offset)
 for(unsigned closed=0;closed<2;++closed)for(unsigned valid=0;valid<2;++valid)
 for(unsigned phase=0;phase<3;++phase)for(unsigned storage=0;storage<2;++storage){
  reset();probe_replies=0;probe_output_failure=false;
  socket_private_t *p=&states[family];
  const uint32_t counts[]={0,1,4096,8191,8192};
  const uint32_t offsets[]={UINT32_MAX,0,1,UINT32_MAX-1};
  p->tcp_next_sequence=wrap?0:2000;p->tcp_sequence_valid=valid;
  p->tcp_send_unacknowledged=p->tcp_send_next_sequence;
  p->tcp_state=phase==0?SOCKET_TCP_ESTABLISHED:phase==1?SOCKET_TCP_SYN_SENT:SOCKET_TCP_CLOSED;
  atomic_store(&p->stream_count,counts[fill]);atomic_store(&p->closed,closed);
  probe_packet_t packet={.source_address=2,.destination_address=1,
   .source_address6={2},.destination_address6={1},.source_port=456,.destination_port=123,
   .sequence=p->tcp_next_sequence+offsets[offset],.acknowledgement=p->tcp_send_next_sequence,
   .flags=NET_TCP_FLAG_ACK,.window=8192};
  int status;
  if(storage)status=family?probe_dispatch6(packet):probe_dispatch4(packet);
  else status=family?socket_inject_tcp_ack_ipv6_reply(packet.source_address6,456,
    packet.destination_address6,123,packet.sequence,packet.acknowledgement,packet.flags,8192,0):
    socket_inject_tcp_ack_ipv4_reply(2,456,1,123,packet.sequence,packet.acknowledgement,packet.flags,8192,0);
  bool reply=storage && !closed && valid && phase==0 && offset==0;
  assert(status==(closed || phase==0?K_OK:K_EAGAIN));
  assert(probe_replies==(unsigned)reply);
  if(reply){
   assert(probe_reply.flags==NET_TCP_FLAG_ACK && probe_reply.sequence==1000);
   assert(probe_reply.acknowledgement==p->tcp_next_sequence);
   assert(probe_reply.window==8192-counts[fill]);
   /* Repeated probes remain answerable, and report the reopened window
    * even if an earlier window-update ACK was lost. Output failure cannot
    * consume receive data or prevent answering the next probe. */
   probe_output_failure=true;
   assert((family?probe_dispatch6(packet):probe_dispatch4(packet))==K_EIO);
   probe_output_failure=false;atomic_store(&p->stream_count,0);
   assert((family?probe_dispatch6(packet):probe_dispatch4(packet))==K_OK);
   assert(probe_replies==3 && probe_reply.window==8192);
  }
  assert(!wakes && !notifications && !locked && !sockets[family].refs);
  assert(!p->tcp_send_pending && p->tcp_send_next_sequence==1000);
  ++probe_cases;
 }
 printf("TCP probe ingress: %u family/wrap/window/sequence/closed/state/storage cases; no ACK loops, output failure and reopened-window recovery PASS\n",probe_cases);
'''
program = program.replace('int main(void){', dispatch+'\nint main(void){\n'+probe_cases, 1)
with tempfile.TemporaryDirectory(prefix='liteos-keepalive-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-no-pie','-I',str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
    # Negative control: reproduce the old ingress ignoring RST entirely.
    broken=program.replace('if ((flags & NET_TCP_FLAG_RST) != 0U) {',
                           'if (false) {')
    assert broken != program
    (path/'test.c').write_text(broken)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-no-pie',
                    '-I',str(root/'include'),str(path/'test.c'),'-o',str(path/'old')],check=True)
    rejected=subprocess.run([str(path/'old')],capture_output=True)
    assert rejected.returncode != 0, 'Ignoring resets must fail the regression'
    for name, old, new in (
        ('old and future ACK replace the send window',
         'acknowledgement - private->tcp_send_unacknowledged <=\n            private->tcp_send_next_sequence - private->tcp_send_unacknowledged',
         'true'),
        ('reordered segment replaces a newer window',
         '((int32_t)(sequence - private->tcp_window_sequence) > 0 ||',
         '(true || (int32_t)(sequence - private->tcp_window_sequence) > 0 ||'),
        ('peer probes silently ignored',
         'sequence == private->tcp_next_sequence - 1U && reply != 0',
         'false && sequence == private->tcp_next_sequence - 1U && reply != 0'),
        ('driver discards a valid probe reply',
         'status == K_OK && reply.valid &&',
         'status == K_OK && false && reply.valid &&'),
        ('ordinary ACK generates an ACK loop',
         'sequence == private->tcp_next_sequence - 1U && reply != 0',
         'sequence == private->tcp_next_sequence && reply != 0'),
    ):
        broken=program.replace(old,new)
        assert broken != program,name
        (path/'test.c').write_text(broken)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-no-pie',
                        '-I',str(root/'include'),str(path/'test.c'),'-o',str(path/'old')],check=True)
        rejected=subprocess.run([str(path/'old')],capture_output=True,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert rejected.returncode != 0 and b'Assertion' in rejected.stderr,name
        print('Negative control rejected:',name)
print('TCP transport: 288 IPv4/IPv6 handshake/completion/reply-storage/wrap/zero-and-maximum-window cases; RST/refusal, exact sequence, challenge ACK, cancellation of retries, connect/error/poll/receive wake; local retry, bounded RTO, keepalive and FIN passed (ASan/UBSan)')
print('TCP partial ACK: 124 family/wrap/prefix cases; retained suffix retransmission, duplicate/stale/future ACK rejection, late completion ownership and bounded local retry PASS')
