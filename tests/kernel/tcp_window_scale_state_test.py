"""Real ring reserve, SYN output, queued SYNACK dispatch and ACK state transitions.

Packet codecs, option lookup/validation, driver segments/replies and receive
window arithmetic are production code. Allocation/device/registry are mocked.
"""
from pathlib import Path
import subprocess
import tempfile
import resource
import signal

root = Path(__file__).resolve().parents[2]


def function(file, signature):
    text = (root/file).read_text()
    start = text.index(signature)
    end = text.index('{', start)+1
    depth = 1
    while depth:
        depth += (text[end] == '{')-(text[end] == '}')
        end += 1
    return text[start:end]


protocol = (root/'kernel/net/socket_protocol.c').read_text()
socket = (root/'kernel/net/socket.c').read_text()
driver = (root/'kernel/drivers/net/protocol.c').read_text()
runtime = (root/'kernel/drivers/net/runtime.c').read_text()
parts = [socket[socket.index('static kstatus_t socket_stream_resize_status('):
                socket.index('kstatus_t socket_allocate(')],
         protocol[protocol.index('static void socket_tcp_apply_mss('):
                  protocol.index('kstatus_t socket_inject_udp(')],
         function('kernel/net/socket_protocol.c', 'static bool socket_tcp_reset_locked('),
         function('kernel/net/socket_protocol.c', 'static kstatus_t socket_tcp_ack_target(')]
parts.insert(0, '\n'.join(line for line in driver.splitlines()
                          if line.startswith(('#define E1000_IPV4_TCP_MSS', '#define E1000_IPV6_TCP_MSS'))))
for family in ('4', '6'):
    parts.append(function('kernel/net/socket_protocol.c',
                          'kstatus_t socket_inject_tcp_syn_ipv'+family+'_scaled('))
    parts.append(function('kernel/net/socket_protocol.c',
                          'kstatus_t socket_inject_tcp_ack_ipv'+family+'_reply('))
for family in ('4', '6'):
    parts.append(function('kernel/drivers/net/protocol.c',
                          'static kstatus_t e1000_send_tcp_ipv'+family+'_segments('))
parts.append(function('kernel/drivers/net/runtime.c', 'bool e1000_send_tcp_reply('))
for signature in ('uint32_t socket_stream_write_locked(', 'uint32_t socket_stream_read_locked('):
    parts.append(function('kernel/net/socket.c', signature))
for signature in ('static void socket_receive_notify_peer(', 'static size_t socket_receive_stream_locked(',
                  'static bool socket_receive_ready(',
                  'static kstatus_t socket_recv_ipv4_impl(', 'static kstatus_t socket_recv_ipv6_impl('):
    parts.append(function('kernel/net/socket_io.c', signature))
for family in ('4', '6'):
    start = runtime.index('packet.protocol == SOCKET_PROTOCOL_TCP && packet.family == OS_AF_INET'+family)
    syn = runtime.index('} else if ((packet.flags & NET_TCP_FLAG_SYN) != 0U &&', start)
    opening = runtime.index('{', syn)
    end = opening+1
    depth = 1
    while depth:
        depth += (runtime[end] == '{')-(runtime[end] == '}')
        end += 1
    parts.append('static kstatus_t dispatch_syn'+family+'(e1000_state_t *state, e1000_rx_packet_t packet){\n'
                 'kstatus_t status;\n'+runtime[opening+1:end-1]+'\nreturn status;\n}')
    start = runtime.index('} else if ((packet.flags & NET_TCP_FLAG_SYN) != 0U ||', start)
    opening = runtime.index('{', start)
    end = opening+1
    depth = 1
    while depth:
        depth += (runtime[end] == '{')-(runtime[end] == '}')
        end += 1
    parts.append('static kstatus_t dispatch'+family+'(e1000_state_t *state, e1000_rx_packet_t packet){\n'
                 'kstatus_t status;\n'+runtime[opening+1:end-1]+'\nreturn status;\n}')
    parse = runtime.index('net_tcp_parse_ipv'+family+'(buffer, length,')
    start = runtime.index('memset(&packet,', parse)
    end = runtime.index('if (tcp_view', start)
    view = 'tcp_view' if family == '4' else 'tcp_view6'
    parts.append('static kstatus_t receive'+family+'(e1000_state_t *state, void *buffer, size_t length){\n'
                 'net_tcp_view_t '+view+';e1000_rx_packet_t packet;\n'
                 'assert(net_tcp_parse_ipv'+family+'(buffer,length,&'+view+')==K_OK);\n'
                 +runtime[start:end]+'return (packet.flags & NET_TCP_FLAG_ACK) ? dispatch'+family+
                 '(state,packet) : dispatch_syn'+family+'(state,packet);\n}')

program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/net/core/core.c"
#include "kernel/net/socket_model.h"
#include "kernel/drivers/net/core_internal.h"
static socket_t target;
static socket_t listener;
static socket_private_t listener_private;
static socket_binding_t listen_binding;
static bool listening_mode;
static unsigned listener_refs;
static socket_private_t *priv;
static socket_tcp_connection_t conn4;
static socket_tcp6_connection_t conn6;
static spinlock_t binding;
static unsigned held,refs,allocations,frees,emitted;
static bool oom,pin_allowed=true;
static e1000_state_t nic;
static net_tcp_view_t last;
static uint16_t read_window;
static unsigned read_updates;
static bool track_read_copies;
static unsigned read_address_copies;
socket_private_t *socket_private(socket_t *s){
 assert(!s || s==&target || s==&listener);return s==&listener?&listener_private:s?priv:NULL;
}
void socket_lock(spinlock_t *l){assert(l && !held);held=1;}
void socket_unlock(spinlock_t *l){assert(l && held);held=0;}
void socket_initialize_globals(void){assert(!held);}
spinlock_t *socket_binding_lock(void){return &binding;}
int32_t socket_find_tcp_locked(uint32_t a,uint16_t b,uint32_t c,uint16_t d){
 assert(held);return conn4.socket && a==2 && b==443 && c==1 && d==50000?0:-1;
}
int32_t socket_find_tcp6_locked(const uint8_t a[16],uint16_t b,const uint8_t c[16],uint16_t d){
 assert(held);return conn6.socket && !memcmp(a,priv->peer_address6,16) && b==443 &&
  !memcmp(c,priv->local_address6,16) && d==50000?0:-1;
}
const socket_tcp_connection_t *socket_tcp_connection_at(uint32_t i){assert(held && !i);return &conn4;}
const socket_tcp6_connection_t *socket_tcp6_connection_at(uint32_t i){assert(held && !i);return &conn6;}
bool object_try_get(void *p){
 assert((p==&target || p==&listener) && held);if(!pin_allowed)return false;
 if(p==&listener)++listener_refs;else ++refs;return true;
}
void object_get(void *p){assert(p==&listener);++listener_refs;}
void object_put(void *p){
 assert(!held);if(p==&listener){assert(listener_refs);--listener_refs;}
 else{assert(p==&target && refs);--refs;}
}
int32_t socket_find_binding_locked(uint32_t a,uint16_t port,uint16_t type,bool only_listener){
 (void)a;assert(held && port==50000 && type==OS_SOCK_STREAM && only_listener);return listening_mode?0:-1;
}
int32_t socket_find_binding6_locked(const uint8_t a[16],uint16_t port,uint16_t type,bool only_listener){
 (void)a;return socket_find_binding_locked(1,port,type,only_listener);
}
const socket_binding_t *socket_binding_at(uint32_t i){assert(held && !i);return &listen_binding;}
bool socket_tcp_slots_available_locked(uint32_t n){assert(held && n==1);return true;}
bool socket_tcp6_slots_available_locked(uint32_t n){return socket_tcp_slots_available_locked(n);}
kstatus_t socket_allocate(uint16_t family,uint16_t type,uint16_t protocol,socket_t **out){
 assert(!held && listening_mode && family==target.family && type==OS_SOCK_STREAM && protocol==SOCKET_PROTOCOL_TCP);
 *out=&target;return K_OK;
}
bool socket_register_tcp_locked(socket_t *s,socket_private_t *p){
 assert(held && s==&target && p==priv);
 if(s->family==OS_AF_INET4)conn4.socket=s;else conn6.socket=s;return true;
}
void socket_abort_child(socket_t *s){(void)s;assert(0);}
bool e1000_tcp_loopback_peer_ack(e1000_state_t *s,const e1000_rx_packet_t *p,const socket_tcp_reply_t *r){
 (void)s;(void)p;(void)r;assert(0);return false;
}
bool e1000_tcp_loopback_peer_ack6(e1000_state_t *s,const e1000_rx_packet_t *p,const socket_tcp_reply_t *r){
 return e1000_tcp_loopback_peer_ack(s,p,r);
}
void *vmalloc(size_t n){assert(!held);if(oom)return NULL;void *p=malloc(n);assert(p);++allocations;return p;}
void vfree(void *p){assert(!held && p);++frees;free(p);}
void socket_copy(void *d,const void *s,size_t n){
 if(track_read_copies && held && n==16)++read_address_copies;
 memmove(d,s,n);
}
void socket_zero(void *p,size_t n){memset(p,0,n);}
bool socket_address6_zero(const uint8_t a[16]){uint8_t n=0;for(unsigned i=0;i<16;++i)n|=a[i];return !n;}
uint64_t x86_read_tsc(void){return 100;}
uint64_t socket_tcp_retransmit_deadline(uint64_t n){return n+1000;}
void socket_tcp_keepalive_activity_locked(socket_private_t *p,uint64_t n){assert(p==priv && held);(void)n;}
uint32_t wake_all(wait_queue_t *q){assert((q==&target.waitq || q==&listener.waitq) && !held);return 0;}
void object_notify_changed(void *p){assert((p==&target || p==&listener) && !held);}
void object_notify_signaled(void *p){object_notify_changed(p);}
/* Preserve the real external wait boundary. Flattening all test mocks and
 * both receive implementations into one TU lets GCC IPA miscompile the
 * guard assertion (GDB shows q == &target.waitq and held == 0 at failure). */
__attribute__((noinline,noipa))
kstatus_t wait_on_queue(wait_queue_t *q,bool (*ready)(void *),void *ctx,uint64_t timeout){
 (void)timeout;assert(q==&target.waitq && !held);
 bool is_ready=ready(ctx);
 if(!is_ready){fprintf(stderr,"Unexpected empty receive wait: family=%u count=%u\n",
     target.family,atomic_load(&priv->stream_count));abort();}
 return K_OK;
}
static kstatus_t read_output4(void *ctx,uint32_t a,uint16_t b,uint32_t c,uint16_t d,
 uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *p,size_t n){
 (void)ctx;(void)a;(void)b;(void)c;(void)d;(void)seq;(void)ack;(void)p;
 assert(!held && flags==NET_TCP_FLAG_ACK && !n);++read_updates;read_window=window;return K_OK;
}
static kstatus_t read_output6(void *ctx,const uint8_t a[16],uint16_t b,const uint8_t c[16],uint16_t d,
 uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *p,size_t n){
 (void)a;(void)c;return read_output4(ctx,1,b,2,d,seq,ack,flags,window,p,n);
}
void socket_tcp_output_get(socket_tcp_ipv4_output_fn *fn,void **ctx){*fn=read_output4;*ctx=NULL;}
void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *fn,void **ctx){*fn=read_output6;*ctx=NULL;}
void socket_tcp_ack_output_ipv4(uint32_t a,uint16_t b,uint32_t c,uint16_t d,uint8_t f,kstatus_t rc){
 (void)a;(void)b;(void)c;(void)d;(void)f;assert(!held && rc==K_OK);
}
void socket_tcp_ack_output_ipv6(const uint8_t a[16],uint16_t b,const uint8_t c[16],uint16_t d,uint8_t f,kstatus_t rc){
 (void)a;(void)b;(void)c;(void)d;(void)f;assert(!held && rc==K_OK);
}
static kstatus_t capture(void *ctx,const void *frame,size_t n){
 assert(ctx==&nic && !held);++emitted;
 assert((target.family==OS_AF_INET4?net_tcp_parse_ipv4(frame,n,&last):
  net_tcp_parse_ipv6(frame,n,&last))==K_OK);return K_OK;
}
''' + '\n'.join(parts) + r'''
static void setup(unsigned family,unsigned capacity,bool fail){
 assert(!refs && !held);memset(&target,0,sizeof(target));
 priv=calloc(1,sizeof(*priv)+SOCKET_STREAM_BUFFER_SIZE/8);assert(priv);
 priv->tcp_rx_present=(uint8_t *)(priv+1);
 target.object.type=KOBJECT_TYPE_SOCKET;target.type=OS_SOCK_STREAM;
 target.family=family?OS_AF_INET6:OS_AF_INET4;target.protocol_state=priv;
 oom=fail;kstatus_t rc=socket_stream_reserve(&target,capacity);
 assert(rc==(fail && capacity>SOCKET_STREAM_BUFFER_SIZE?K_ENOMEM:K_OK));oom=false;
 priv->local_address=1;priv->peer_address=2;priv->local_port=50000;priv->peer_port=443;
 priv->local_address6[0]=priv->peer_address6[0]=0x20;
 priv->local_address6[15]=1;priv->peer_address6[15]=2;
 priv->tcp_state=SOCKET_TCP_SYN_SENT;priv->connected=true;
 priv->tcp_initial_sequence=UINT32_MAX;priv->tcp_send_next_sequence=0;
 priv->tcp_send_unacknowledged=UINT32_MAX;priv->tcp_send_pending=true;
 socket_tcp_prepare_scale(priv,true);
 conn4.socket=family?NULL:&target;conn6.socket=family?&target:NULL;
 net_device_init(&nic.device,"test",nic.mac,1500,capture,&nic);nic.device.link_up=true;
}
static void teardown(void){
 assert(!refs && !held);if(priv->stream_dynamic)vfree(priv->stream_dynamic);
 free(priv);priv=NULL;conn4.socket=conn6.socket=NULL;
}
static kstatus_t ack(unsigned family,uint32_t seq,uint32_t ackno,uint16_t window){
 socket_tcp_reply_t reply;
 return family?socket_inject_tcp_ack_ipv6_reply(priv->peer_address6,443,priv->local_address6,
   50000,seq,ackno,NET_TCP_FLAG_ACK,window,&reply):
  socket_inject_tcp_ack_ipv4_reply(2,443,1,50000,seq,ackno,NET_TCP_FLAG_ACK,window,&reply);
}
static kstatus_t feed(unsigned family,uint32_t seq,uint32_t ackno,uint8_t flags,int scale){
 uint8_t frame[256],mac[6]={2,1,2,3,4,5};size_t n=0;
 assert((family?net_tcp_build_ipv6_scaled(frame,sizeof(frame),mac,mac,
   priv->peer_address6,priv->local_address6,443,50000,seq,ackno,flags,60000,scale,NULL,0,&n):
  net_tcp_build_ipv4_scaled(frame,sizeof(frame),mac,mac,2,1,443,50000,
   seq,ackno,flags,60000,scale,NULL,0,&n))==K_OK);
 return family?receive6(&nic,frame,n):receive4(&nic,frame,n);
}
int main(void){
 unsigned cases=0;
 const unsigned capacities[]={32768,65536,262144,4194304};
 for(unsigned family=0;family<2;++family)
 for(unsigned c=0;c<4;++c)for(int scale=-1;scale<=14;++scale)
 for(unsigned fail=0;fail<2;++fail){
  setup(family,capacities[c],fail);unsigned cap=socket_stream_capacity(priv);
  uint16_t window=socket_tcp_receive_window(priv,true);
  unsigned before=emitted;
  if(family)assert(e1000_send_tcp_ipv6_segments(&nic,nic.mac,priv->local_address6,
   priv->peer_address6,50000,443,UINT32_MAX,0,NET_TCP_FLAG_SYN,window,NULL,0)==K_OK);
  else assert(e1000_send_tcp_ipv4_segments(&nic,nic.mac,1,2,50000,443,
   UINT32_MAX,0,NET_TCP_FLAG_SYN,window,NULL,0)==K_OK);
  assert(emitted==before+1 && last.window==window);
  assert(last.peer_window_scale_present==(cap>32768));
  if(cap>32768)assert(last.peer_window_scale==priv->tcp_receive_scale);
  /* Invalid ACK must neither complete nor commit options. */
  assert(feed(family,UINT32_MAX,1,NET_TCP_FLAG_SYN|NET_TCP_FLAG_ACK,scale)==K_EAGAIN);
  assert(!priv->tcp_scale_seen && priv->tcp_state==SOCKET_TCP_SYN_SENT);
  assert(feed(family,UINT32_MAX,0,NET_TCP_FLAG_SYN|NET_TCP_FLAG_ACK,scale)==K_OK);
  bool negotiated=cap>32768 && scale>=0;
  assert(priv->tcp_scale_seen && priv->tcp_scale_negotiated==negotiated);
  assert(priv->tcp_peer_scale==(negotiated?scale:0));
  assert(priv->tcp_state==SOCKET_TCP_ESTABLISHED && priv->tcp_peer_window==60000);
  assert(last.flags==NET_TCP_FLAG_ACK && !last.peer_window_scale_present);
  uint32_t quantum=negotiated?1U<<priv->tcp_receive_scale:1U;
  const unsigned counts[]={0,1,cap/2,cap-1,cap};
  for(unsigned i=0;i<5;++i){
   uint32_t free_bytes=cap-counts[i];
   uint16_t w=socket_tcp_window_for_count(priv,counts[i],false);
   assert((uint32_t)w*quantum<=free_bytes);
   if(negotiated)assert(free_bytes-(uint32_t)w*quantum<quantum);
   else assert(w==(free_bytes>65535?65535:free_bytes));
  }
  /* Ordinary ACK windows are scaled once, never narrowed to uint16_t. */
  assert(ack(family,0,0,65535)==K_OK);
  assert(priv->tcp_peer_window==(65535U<<(negotiated?scale:0)));
  assert(feed(family,UINT32_MAX,0,NET_TCP_FLAG_SYN|NET_TCP_FLAG_ACK,14)==K_OK);
  assert(priv->tcp_peer_scale==(negotiated?scale:0));
  assert(socket_tcp_syn_scale_ipv4(1,50000,2,443,UINT32_MAX,0)==-1);
  /* Actual read path: peek keeps credit, consumption reopens a full ring.
   * Read beyond the old fixed 32KiB limit in one operation. */
  unsigned char *in=malloc(cap),*out=malloc(cap);assert(in && out);
  for(unsigned i=0;i<cap;++i)in[i]=(unsigned char)(i*17+i/251);
  priv->stream_read=priv->stream_write=cap-13;
  priv->stream_write=socket_stream_write_locked(priv,in,cap);atomic_store(&priv->stream_count,cap);
  unsigned updates=read_updates;uint64_t bytes=0;
  socket_ipv4_endpoint_t endpoint4;socket_ipv6_endpoint_t endpoint6;
  for(unsigned peek=1;;--peek){
   /* 只统计真实接收期间的地址快照；PEEK 不应准备窗口通告。 */
   read_address_copies=0;track_read_copies=true;
   assert((family?socket_recv_ipv6_impl(&target,out,cap,&endpoint6,0,&bytes,peek):
     socket_recv_ipv4_impl(&target,out,cap,&endpoint4,0,&bytes,peek))==K_OK);
   track_read_copies=false;
   assert(read_address_copies==(family?(peek?1U:3U):0U));
   assert(bytes==cap && !memcmp(out,in,cap));
   if(peek)assert(read_updates==updates && atomic_load(&priv->stream_count)==cap);
   else break;
  }
  assert(!atomic_load(&priv->stream_count) && read_updates==updates+1);
  assert(read_window==socket_tcp_receive_window(priv,false));
  /* 未触及窗口通告阈值的普通读取，也只复制调用方需要的源地址。 */
  priv->stream_write=socket_stream_write_locked(priv,in,32);
  atomic_store(&priv->stream_count,32);updates=read_updates;
  read_address_copies=0;track_read_copies=true;
  assert((family?socket_recv_ipv6_impl(&target,out,32,&endpoint6,0,&bytes,false):
    socket_recv_ipv4_impl(&target,out,32,&endpoint4,0,&bytes,false))==K_OK);
  track_read_copies=false;
  assert(bytes==32 && !memcmp(out,in,32) && !atomic_load(&priv->stream_count));
  assert(read_updates==updates && read_address_copies==(family?1U:0U));
  free(in);free(out);
  teardown();++cases;
 }
 unsigned passive_cases=0;
 for(unsigned family=0;family<2;++family)for(int scale=-1;scale<=14;++scale)
 for(unsigned fail=0;fail<2;++fail){
  setup(family,32768,false);
  memset(priv,0,offsetof(socket_private_t,stream_buffer));priv->tcp_rx_present=(uint8_t *)(priv+1);
  conn4.socket=conn6.socket=NULL;listening_mode=true;oom=fail;
  memset(&listener_private,0,sizeof(listener_private));listener_private.listening=true;
  listener_private.tcp_keepidle=7200;listen_binding.socket=&listener;
  /* Source addresses are needed by the peer packet builder before child creation. */
  priv->local_address6[0]=priv->peer_address6[0]=0x20;
  priv->local_address6[15]=1;priv->peer_address6[15]=2;
  assert(feed(family,UINT32_MAX,0,NET_TCP_FLAG_SYN,scale)==K_OK);oom=false;
  unsigned cap=socket_stream_capacity(priv);bool negotiated=!fail && scale>=0;
  assert(priv->tcp_state==SOCKET_TCP_SYN_RECEIVED && priv->tcp_scale_seen);
  assert(priv->tcp_scale_negotiated==negotiated && last.peer_window_scale_present==negotiated);
  assert(last.window==(cap>65535?65535:cap));
  assert(listener_refs==1 && atomic_load(&listener_private.accept_count)==1);
  assert(listener_private.accept_queue[0]==&target && priv->tcp_keepidle==7200);
  /* A retransmitted SYN cannot change an already published negotiation. */
  assert(feed(family,UINT32_MAX,0,NET_TCP_FLAG_SYN,14)==K_OK);
  assert(priv->tcp_peer_scale==(negotiated?scale:0));
  assert(feed(family,1,0,NET_TCP_FLAG_SYN,14)==K_EAGAIN);
  assert(atomic_load(&listener_private.accept_count)==1 && listener_refs==1);
  uint32_t send_next=priv->tcp_send_next_sequence;
  assert(ack(family,0,send_next,65535)==K_OK);
  assert(priv->tcp_state==SOCKET_TCP_ESTABLISHED && !listener_refs);
  assert(priv->tcp_peer_window==(65535U<<(negotiated?scale:0)));
  assert(feed(family,UINT32_MAX,0,NET_TCP_FLAG_SYN,14)==K_EAGAIN);
  assert(atomic_load(&listener_private.accept_count)==1 && !listener_refs);
  listening_mode=false;teardown();++passive_cases;
 }
 assert(allocations==frees);
 printf("Passive handshake: %u actual IPv4/IPv6 queued SYN/create/reserve/SYNACK/ACK cases; first-SYN option ownership, duplicate SYN, legacy/OOM fallback and listener reference release PASS\n",passive_cases);
 printf("TCP window scaling: %u IPv4/IPv6 reserve/real-SYN/parsed-queued-SYNACK/ACK cases; shifts 0..14, absent peer, OOM fallback, wrap, bad ACK, duplicate SYNACK and bounded wire windows PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='tcp-ws-state-') as directory:
    binary = directory+'/test'
    command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-I'+str(root), '-I'+str(root/'include'),
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
    for name, old, new in (
        ('unneeded IPv6 ACK address snapshot',
         'if (window_update) {\n                    socket_copy(update_source,',
         'if (true) {\n                    socket_copy(update_source,'),
        ('queued IPv4 WS metadata lost',
         'packet.peer_window_scale_present = tcp_view.peer_window_scale_present;',
         'packet.peer_window_scale_present = false;'),
        ('queued IPv6 WS metadata lost',
         'packet.peer_window_scale_present = tcp_view6.peer_window_scale_present;',
         'packet.peer_window_scale_present = false;'),
        ('SYN window incorrectly scaled',
         'private->tcp_peer_window = window;',
         'private->tcp_peer_window = socket_tcp_decode_window(private, window, false);'),
        ('ACK window left unscaled',
         'private->tcp_peer_window = decoded;',
         'private->tcp_peer_window = window;'),
        ('duplicate SYNACK renegotiates scale',
         '!private->tcp_scale_seen && (active || passive)',
         '(true || !private->tcp_scale_seen) && (active || passive || private->tcp_state == SOCKET_TCP_ESTABLISHED)'),
    ):
        broken = program.replace(old, new)
        assert broken != program, name
        subprocess.run(command, input=broken, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, text=True, timeout=60,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert result.returncode == -signal.SIGABRT and 'Assertion' in result.stderr, (name, result)
        print('Negative control rejected:', name)
