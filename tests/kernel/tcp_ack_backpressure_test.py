"""Compile production ACK injection, driver replies, receive and transport code.

No guest/kernel build or network access. The negative control removes only the
new notification hooks and ACK poll, restoring the original lost-ACK behavior.
"""
from pathlib import Path
import resource
import subprocess
import tempfile

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


program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arch/x86_64/cpu.h>
#include <kernel/net_core.h>
#include "socket_model.h"
#include "kernel/drivers/net/core_internal.h"
#define DELAY 20000000ULL
static socket_t sock[2]; static socket_private_t state[2];
static uint8_t present[2][SOCKET_STREAM_BUFFER_SIZE/8];
static socket_tcp_connection_t conn4; static socket_tcp6_connection_t conn6;
static spinlock_t g_binding_lock;
static unsigned locked, refs[2], outputs[2], attempts, pins, poll_locks[2];
static uint64_t now=1000, conversion=1;
static kstatus_t wire_status;
static uint32_t last_seq[2], last_ack[2]; static uint16_t last_window[2];
static uint8_t last_flags[2]; static size_t last_length[2];
static unsigned active_family;
static bool pin_allowed=true, missing_output;
static unsigned interleave;
static unsigned which(const void *p){assert(p==&sock[0] || p==&sock[1]);return p==&sock[1];}
uint64_t x86_read_tsc(void){return now;}
uint64_t x86_timeout_ns_to_tsc(uint64_t ns){return ns*conversion;}
void socket_initialize_globals(void){}
void socket_lock(spinlock_t *l){
 assert(!locked);locked=1;
 for(unsigned f=0;f<2;++f)if(l==&sock[f].lock)++poll_locks[f];
}
void socket_unlock(spinlock_t *l){(void)l;assert(locked);locked=0;}
spinlock_t *socket_binding_lock(void){return &g_binding_lock;}
uint32_t socket_registry_capacity_locked(void){assert(locked);return 1;}
const socket_tcp_connection_t *socket_tcp_connection_at(uint32_t i){assert(locked && !i);return &conn4;}
const socket_tcp6_connection_t *socket_tcp6_connection_at(uint32_t i){assert(locked && !i);return &conn6;}
int32_t socket_find_tcp_locked(uint32_t src,uint16_t sp,uint32_t dst,uint16_t dp){
 assert(locked);return conn4.socket && src==state[0].peer_address && sp==456 &&
  dst==state[0].local_address && dp==123?0:-1;
}
int32_t socket_find_tcp6_locked(const uint8_t src[16],uint16_t sp,const uint8_t dst[16],uint16_t dp){
 assert(locked);return conn6.socket && !memcmp(src,state[1].peer_address6,16) && sp==456 &&
  !memcmp(dst,state[1].local_address6,16) && dp==123?0:-1;
}
socket_private_t *socket_private(socket_t *s){return s?&state[which(s)]:NULL;}
bool object_try_get(void *s){assert(locked);++pins;if(!pin_allowed)return false;++refs[which(s)];return true;}
void object_put(void *s){assert(!locked && refs[which(s)]);--refs[which(s)];}
void socket_copy(void *out,const void *in,size_t n){memmove(out,in,n);}
void socket_zero(void *out,size_t n){memset(out,0,n);}
bool socket_address6_zero(const uint8_t a[16]){uint8_t v=0;for(unsigned i=0;i<16;i++)v|=a[i];return !v;}
uint32_t wake_all(wait_queue_t *q){(void)q;assert(!locked);return 0;}
void object_notify_changed(void *p){(void)p;assert(!locked);}
void object_notify_signaled(void *p){object_notify_changed(p);}
uint64_t socket_tcp_retransmit_deadline(uint64_t t){return t+1000000000ULL;}
bool socket_tcp_keepalive_poll_locked(socket_private_t *p,uint64_t t,bool *failed){
 (void)p;(void)t;assert(locked);*failed=false;return false;
}
void socket_tcp_keepalive_activity_locked(socket_private_t *p,uint64_t t){(void)p;(void)t;}
kstatus_t wait_on_queue(wait_queue_t *q,bool (*ready)(void *),void *ctx,uint64_t timeout){
 (void)q;(void)timeout;assert(!locked && ready(ctx));return K_OK;
}
static void report(unsigned f,kstatus_t status,uint8_t flags){
 socket_private_t *p=&state[f];
 if(f)socket_tcp_ack_output_ipv6(p->local_address6,123,p->peer_address6,456,flags,status);
 else socket_tcp_ack_output_ipv4(p->local_address,123,p->peer_address,456,flags,status);
}
static kstatus_t emit(unsigned f,uint32_t seq,uint32_t ack,uint8_t flags,uint16_t win,size_t n){
 assert(!locked);++outputs[f];last_seq[f]=seq;last_ack[f]=ack;
 last_window[f]=win;last_flags[f]=flags;last_length[f]=n;
 /* Deterministic completion interleavings, while the real output is unlocked. */
 unsigned action=interleave;interleave=0;
 if(action==1){now+=3;report(f,K_EAGAIN,NET_TCP_FLAG_ACK);}
 if(action==2){assert(socket_close(&sock[f])==K_OK);}
 if(action==3){socket_lock(&sock[f].lock);socket_tcp_fail_locked(&state[f],K_EIO);socket_unlock(&sock[f].lock);}
 if(action==4){socket_tcp_poll(now);}
 return wire_status;
}
static kstatus_t output4(void *ctx,uint32_t src,uint16_t sp,uint32_t dst,uint16_t dp,
 uint32_t seq,uint32_t ack,uint8_t flags,uint16_t win,const void *data,size_t n){
 (void)ctx;(void)data;assert(src==1 && dst==2 && sp==123 && dp==456);
 return emit(0,seq,ack,flags,win,n);
}
static kstatus_t output6(void *ctx,const uint8_t src[16],uint16_t sp,const uint8_t dst[16],uint16_t dp,
 uint32_t seq,uint32_t ack,uint8_t flags,uint16_t win,const void *data,size_t n){
 (void)ctx;(void)data;assert(!memcmp(src,state[1].local_address6,16) &&
  !memcmp(dst,state[1].peer_address6,16) && sp==123 && dp==456);
 return emit(1,seq,ack,flags,win,n);
}
void socket_tcp_output_get(socket_tcp_ipv4_output_fn *fn,void **ctx){*fn=missing_output?NULL:output4;*ctx=NULL;}
void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *fn,void **ctx){*fn=missing_output?NULL:output6;*ctx=NULL;}
/* Close uses the production function below; registry and unrelated peer/listener
 * teardown are fixture services. No pending ACK retains a reference or work item. */
static void socket_remove_from_accept_queue_locked(socket_t *p,socket_t *s){(void)p;(void)s;assert(0);}
static void socket_unbind_locked(socket_t *s){(void)s;assert(locked);}
static bool socket_unregister_tcp_locked(socket_t *s){
 assert(locked);if(which(s))conn6.socket=NULL;else conn4.socket=NULL;return true;
}
static void socket_detach_peer(socket_t *s,socket_private_t *p){(void)s;assert(!locked && !p->peer);}
'''
program += '\n'.join(function('kernel/net/socket.c', s) for s in (
    'uint32_t socket_stream_write_locked(', 'uint32_t socket_stream_read_locked(',
    'kstatus_t socket_close('))
replies = function('kernel/drivers/net/runtime.c', 'static bool e1000_send_tcp_reply(')
program += replies
program += r'''
static kstatus_t device_output(void *ctx,const void *frame,size_t length){
 (void)ctx;net_tcp_view_t view;unsigned f=active_family;
 assert((f?net_tcp_parse_ipv6(frame,length,&view):net_tcp_parse_ipv4(frame,length,&view))==K_OK);
 ++attempts;return emit(f,view.sequence,view.acknowledgement,view.flags,view.window,view.payload_length);
}
static void reset(unsigned f){
 assert(!locked && !refs[0] && !refs[1]);
 memset(sock,0,sizeof(sock));memset(state,0,sizeof(state));memset(present,0,sizeof(present));
 memset(outputs,0,sizeof(outputs));conn4.socket=NULL;conn6.socket=NULL;
 wire_status=K_OK;now=1000;conversion=1;interleave=0;attempts=0;missing_output=false;pin_allowed=true;pins=0;
 active_family=f;
 for(unsigned i=0;i<2;i++){
  sock[i].object.type=KOBJECT_TYPE_SOCKET;sock[i].type=OS_SOCK_STREAM;
  sock[i].family=i?OS_AF_INET6:OS_AF_INET4;
  state[i].tcp_state=SOCKET_TCP_ESTABLISHED;state[i].tcp_sequence_valid=true;
  state[i].tcp_next_sequence=1000;state[i].tcp_send_next_sequence=500;
  state[i].tcp_peer_window=32768;state[i].local_address=1;state[i].peer_address=2;
  state[i].local_address6[15]=1;state[i].peer_address6[15]=2;
  state[i].local_port=123;state[i].peer_port=456;state[i].tcp_rx_present=present[i];
 }
 if(f)conn6.socket=&sock[1];else conn4.socket=&sock[0];
}
static bool inject(unsigned f,uint32_t sequence,size_t length){
 static unsigned char data[1416];socket_tcp_reply_t reply;
 socket_private_t *p=&state[f];
 kstatus_t status=f?socket_inject_tcp_ipv6_reply(p->peer_address6,456,p->local_address6,123,
  sequence,NET_TCP_FLAG_ACK,data,length,&reply):socket_inject_tcp_ipv4_reply(2,456,1,123,
  sequence,NET_TCP_FLAG_ACK,data,length,&reply);
 assert(status==K_OK && reply.valid);
 e1000_state_t driver={0};e1000_rx_packet_t packet={0};
 const uint8_t mac[6]={2,1,2,3,4,5};
 net_device_init(&driver.device,"fixture",mac,1500,device_output,NULL);driver.device.link_up=true;
 memcpy(packet.source_mac,mac,6);memcpy(packet.destination_mac,mac,6);
 packet.source_address=2;packet.destination_address=1;packet.source_port=456;packet.destination_port=123;
 memcpy(packet.source_address6,p->peer_address6,16);memcpy(packet.destination_address6,p->local_address6,16);
 packet.payload_length=length;
 packet.family=f?OS_AF_INET6:OS_AF_INET4;
 return e1000_send_tcp_reply(&driver,&packet,&reply);
}
static void poll_at(uint64_t t){now=t;socket_tcp_poll(now);assert(!locked && !refs[0] && !refs[1]);}
static int receive(unsigned f,size_t n){
 char buffer[32768];uint64_t bytes=0;
 socket_ipv4_endpoint_t ep4;socket_ipv6_endpoint_t ep6;
 int result=f?socket_recv_ipv6(&sock[f],buffer,n,&ep6,1,&bytes):
  socket_recv(&sock[f],buffer,n,&ep4,1,&bytes);
 assert(bytes==n);return result;
}
int main(void){
 /* 空闲和未到期连接每轮只获取一次目标锁，且仍由真实轮询释放引用和锁。 */
 for(unsigned f=0;f<2;++f) {
  reset(f);unsigned before=poll_locks[f];poll_at(now);
  assert(poll_locks[f]==before+1 && !outputs[f]);
  report(f,K_EAGAIN,NET_TCP_FLAG_ACK);before=poll_locks[f];
  poll_at(state[f].tcp_ack_retry_tsc-1);
  assert(poll_locks[f]==before+1 && !outputs[f]);
 }
 puts("TCP 空闲及未到期 ACK：IPv4/IPv6 每连接一轮一次目标锁通过");
#ifdef NEGATIVE_CONTROL
 reset(0);wire_status=K_EAGAIN;assert(!inject(0,1000,1416));
 wire_status=K_OK;poll_at(now+DELAY);
 fprintf(stderr,"Original behavior: ACK attempts after TX recovery without ingress = %u (expected 2)\n",outputs[0]);
 assert(outputs[0]==2);
#endif
 for(unsigned f=0;f<2;f++){
  /* Ordered data + real driver ACK failure; no more incoming packets. */
  reset(f);wire_status=K_EAGAIN;assert(!inject(f,1000,1416));
  assert(state[f].tcp_ack_pending && state[f].tcp_ack_retry_tsc==now+DELAY);
  uint64_t due=state[f].tcp_ack_retry_tsc;unsigned before=outputs[f];
  poll_at(due-1);assert(outputs[f]==before);
  wire_status=K_OK;poll_at(due);assert(outputs[f]==before+1 && !state[f].tcp_ack_pending);
  assert(last_ack[f]==2416 && last_window[f]==32768-1416 && !last_length[f]);
  poll_at(due+DELAY);assert(outputs[f]==before+1);
  /* Out of order, repeated out of order and entirely duplicate segments. */
  reset(f);wire_status=K_EAGAIN;assert(!inject(f,2416,1416));assert(!inject(f,2416,1416));
  assert(state[f].tcp_rx_future_count==1416 && !atomic_load(&state[f].stream_count));
  due=state[f].tcp_ack_retry_tsc;wire_status=K_OK;poll_at(due);
  assert(last_ack[f]==1000 && last_window[f]==32768);
  wire_status=K_EAGAIN;assert(!inject(f,1000,1416));assert(state[f].tcp_next_sequence==3832);
  assert(!inject(f,1000,1416));due=state[f].tcp_ack_retry_tsc;
  /* Snapshot comes from current socket, including application consumption. */
  atomic_store(&state[f].stream_count,17);state[f].tcp_send_next_sequence=777;
  wire_status=K_OK;poll_at(due);
  assert(last_seq[f]==777 && last_ack[f]==3832 && last_window[f]==32751);
  /* Repeated failures coalesce without moving the first deadline. */
  reset(f);report(f,K_EAGAIN,NET_TCP_FLAG_ACK);due=state[f].tcp_ack_retry_tsc;
  now+=100;report(f,K_EAGAIN,NET_TCP_FLAG_ACK);assert(state[f].tcp_ack_retry_tsc==due);
  wire_status=K_EAGAIN;poll_at(due);assert(outputs[f]==1 && state[f].tcp_ack_retry_tsc==due+DELAY);
  poll_at(due+DELAY-1);assert(outputs[f]==1);wire_status=K_OK;poll_at(due+DELAY);assert(outputs[f]==2);
  /* Zero conversion, deadline == 0 and wrap are defined, not sentinel bugs. */
  reset(f);conversion=0;report(f,K_EAGAIN,NET_TCP_FLAG_ACK);poll_at(1000);assert(!outputs[f]);poll_at(1001);assert(outputs[f]==1);
  reset(f);now=UINT64_MAX-DELAY+1;report(f,K_EAGAIN,NET_TCP_FLAG_ACK);assert(state[f].tcp_ack_retry_tsc==0);
  poll_at(UINT64_MAX);assert(!outputs[f]);poll_at(0);assert(outputs[f]==1);
  reset(f);now=UINT64_MAX-DELAY/2;report(f,K_EAGAIN,NET_TCP_FLAG_ACK);due=state[f].tcp_ack_retry_tsc;
  poll_at(due-1);assert(!outputs[f]);poll_at(due);assert(outputs[f]==1);
  reset(f);conversion=3;report(f,K_EAGAIN,NET_TCP_FLAG_ACK);due=1000+3*DELAY;
  poll_at(due-1);assert(!outputs[f]);poll_at(due);assert(outputs[f]==1);
  /* Old success/permanent completion may not erase a concurrent new failure. */
  for(unsigned fatal=0;fatal<2;fatal++){
   reset(f);report(f,K_EAGAIN,NET_TCP_FLAG_ACK);due=state[f].tcp_ack_retry_tsc;
   interleave=1;wire_status=fatal?K_EIO:K_OK;poll_at(due);
   assert(state[f].tcp_ack_pending && state[f].tcp_ack_retry_tsc==due+3+DELAY);
   wire_status=K_OK;poll_at(state[f].tcp_ack_retry_tsc);assert(outputs[f]==2 && !state[f].tcp_ack_pending);
  }
  /* Reentrant poll cannot resubmit the old pending ACK during its output. */
  reset(f);report(f,K_EAGAIN,NET_TCP_FLAG_ACK);due=state[f].tcp_ack_retry_tsc;
  interleave=4;poll_at(due);assert(outputs[f]==1 && !state[f].tcp_ack_pending);
  reset(f);report(f,K_EAGAIN,NET_TCP_FLAG_ACK);due=state[f].tcp_ack_retry_tsc;
  interleave=2;wire_status=K_EAGAIN;poll_at(due);assert(!state[f].tcp_ack_pending);
  reset(f);report(f,K_EAGAIN,NET_TCP_FLAG_ACK);due=state[f].tcp_ack_retry_tsc;
  interleave=3;wire_status=K_EAGAIN;poll_at(due);assert(!state[f].tcp_ack_pending && state[f].tcp_state==SOCKET_TCP_CLOSED);
  reset(f);report(f,K_EAGAIN,NET_TCP_FLAG_ACK);due=state[f].tcp_ack_retry_tsc;
  assert(socket_close(&sock[f])==K_OK);poll_at(due);assert(!outputs[f]);report(f,K_EAGAIN,NET_TCP_FLAG_ACK);assert(!refs[f]);
  /* Permanent errors do not become ACK retry or poison ordinary send state. */
  const int errors[]={K_EIO,K_EINVAL,K_ENOENT,K_EBUSY};
  for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++){
   reset(f);report(f,errors[i],NET_TCP_FLAG_ACK);assert(!state[f].tcp_ack_pending);
   report(f,K_EAGAIN,NET_TCP_FLAG_ACK);due=state[f].tcp_ack_retry_tsc;
   wire_status=errors[i];poll_at(due);assert(!state[f].tcp_ack_pending && !atomic_load(&state[f].tcp_error));
   poll_at(due+DELAY);assert(outputs[f]==1);
  }
  const uint8_t flags[]={NET_TCP_FLAG_SYN|NET_TCP_FLAG_ACK,NET_TCP_FLAG_FIN|NET_TCP_FLAG_ACK,NET_TCP_FLAG_RST,NET_TCP_FLAG_ACK|NET_TCP_FLAG_PSH};
  for(unsigned i=0;i<sizeof(flags);i++){reset(f);report(f,K_EAGAIN,flags[i]);assert(!state[f].tcp_ack_pending);}
  reset(f);pin_allowed=false;report(f,K_EAGAIN,NET_TCP_FLAG_ACK);assert(!state[f].tcp_ack_pending && pins==1 && !refs[f]);
  reset(f);report(f,K_EAGAIN,NET_TCP_FLAG_ACK);missing_output=true;poll_at(state[f].tcp_ack_retry_tsc);assert(!state[f].tcp_ack_pending && !outputs[f]);
  /* Production recv -> window update backpressure -> latest window on poll. */
  reset(f);atomic_store(&state[f].stream_count,30000);wire_status=K_EAGAIN;
  assert(receive(f,1000)==K_OK && state[f].tcp_ack_pending);
  due=state[f].tcp_ack_retry_tsc;assert(receive(f,1000)==K_OK && state[f].tcp_ack_retry_tsc==due);
  wire_status=K_OK;before=outputs[f];poll_at(due);assert(outputs[f]==before+1 && last_window[f]==4768);
  /* Pending ACK and ordinary data retry can coexist in the same poll. */
  reset(f);report(f,K_EAGAIN,NET_TCP_FLAG_ACK);due=state[f].tcp_ack_retry_tsc;
  state[f].tcp_send_pending=true;state[f].tcp_send_unacknowledged=490;
  state[f].tcp_send_length=10;state[f].tcp_send_flags=NET_TCP_FLAG_ACK|NET_TCP_FLAG_PSH;
  memset(state[f].tcp_send_payload,'x',10);state[f].tcp_send_deadline_tsc=due+1000000000;
  state[f].tcp_local_retry_tsc=due;wire_status=K_EAGAIN;poll_at(due);
  assert(outputs[f]==2 && state[f].tcp_ack_pending && state[f].tcp_send_pending &&
   state[f].tcp_send_length==10 && !state[f].tcp_send_retransmits && last_length[f]==10);
  wire_status=K_OK;poll_at(due+DELAY);assert(outputs[f]==4 && !state[f].tcp_ack_pending &&
   state[f].tcp_send_pending && !state[f].tcp_local_retry_tsc && !state[f].tcp_send_retransmits);
  poll_at(state[f].tcp_send_deadline_tsc);assert(state[f].tcp_send_retransmits==1 && outputs[f]==5);
  reset(f);report(f,K_EAGAIN,NET_TCP_FLAG_ACK);socket_lock(&sock[f].lock);
  socket_tcp_fail_locked(&state[f],K_EIO);socket_unlock(&sock[f].lock);
  assert(!state[f].tcp_ack_pending && !state[f].tcp_ack_retry_tsc);
 }
 puts("ACK backpressure: production IPv4/IPv6 driver/injection/recv/poll; ordered/reordered/duplicate ACKs, no ingress recovery, current state, 20ms boundaries/wrap, interleavings, close/errors and data RTO PASS");
}
'''

sources = ['kernel/net/socket_protocol.c', 'kernel/net/socket_transport.c',
           'kernel/net/socket_io.c', 'kernel/net/core/core.c']
with tempfile.TemporaryDirectory(prefix='tcp-ack-backpressure-') as directory:
    path = Path(directory)
    fixture = path / 'test.c'
    fixture.write_text(program)
    common = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
              '-DOS_DEBUG_SERIAL=0', '-ffunction-sections', '-fdata-sections',
              '-Wl,--gc-sections', '-fno-pie', '-no-pie',
              '-I'+str(root), '-I'+str(root/'include'), '-I'+str(root/'kernel/net')]
    subprocess.run(common + ['-fsanitize=address,undefined', str(fixture)] +
                   [str(root/s) for s in sources] + ['-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=30)
    # 重放旧的重复加锁成本，必须被同一空闲连接预算拒绝，而不是靠时间阈值判断。
    transport = (root/sources[1]).read_text()
    duplicate = transport.replace(
        '        (int64_t)(now - private->tcp_ack_retry_tsc) < 0) {\n'
        '        return;',
        '        (int64_t)(now - private->tcp_ack_retry_tsc) < 0) {\n'
        '        socket_unlock(&target->lock);\n'
        '        socket_lock(&target->lock);\n'
        '        return;', 1)
    assert duplicate != transport
    (path/'duplicate_transport.c').write_text(duplicate)
    subprocess.run(common + [str(fixture), str(root/sources[0]),
        str(path/'duplicate_transport.c'), str(root/sources[2]), str(root/sources[3]),
        '-o', str(path/'duplicate')], check=True)
    limit = resource.getrlimit(resource.RLIMIT_CORE)
    try:
        resource.setrlimit(resource.RLIMIT_CORE, (0, limit[1]))
        result = subprocess.run([str(path/'duplicate')], capture_output=True, text=True, timeout=30)
        assert result.returncode != 0 and 'poll_locks[f]==before+1' in result.stderr, result.stderr
    finally:
        resource.setrlimit(resource.RLIMIT_CORE, limit)
    print('重复获取空闲连接锁的负面对照已被预算检查拒绝')
    # Disable the driver's new failure notification and delayed poll only.
    # Keep the same production RX state transitions and regression assertions.
    old_replies = replies.replace('socket_tcp_ack_output_ipv4(', 'ignored4(').replace(
        'socket_tcp_ack_output_ipv6(', 'ignored6(')
    old_fixture = program.replace(replies, '\n#define ignored4(...) ((void)0)\n'
        '#define ignored6(...) ((void)0)\n'+old_replies)
    old_transport = (root/sources[1]).read_text().replace(
        '            socket_tcp_poll_ack_locked(target, private, now_tsc, ipv6,\n'
        '                                       output4, context4, output6, context6);', '')
    assert old_transport != (root/sources[1]).read_text()
    (path/'old.c').write_text(old_fixture)
    (path/'old_transport.c').write_text(old_transport)
    subprocess.run(common + ['-DNEGATIVE_CONTROL', '-Wno-unused-function', str(path/'old.c'),
        str(root/sources[0]), str(path/'old_transport.c'), str(root/sources[2]),
        str(root/sources[3]), '-o', str(path/'old')], check=True)
    limit = resource.getrlimit(resource.RLIMIT_CORE)
    try:
        resource.setrlimit(resource.RLIMIT_CORE, (0, limit[1]))
        result = subprocess.run([str(path/'old')], capture_output=True, text=True, timeout=30)
        assert result.returncode != 0 and 'outputs[0]==2' in result.stderr, result.stderr
    finally:
        resource.setrlimit(resource.RLIMIT_CORE, limit)
    print('Negative control: original driver ACK EAGAIN loses recovery; regression rejects it')
    # A common broken fix clears pending on successful output completion.
    # Reject it using the unlocked old-output/new-failure interleaving above.
    transport = (root/sources[1]).read_text()
    broken = transport.replace('    if (status == K_EAGAIN) {',
        '    if (status == K_OK) {\n'
        '        private->tcp_ack_pending = false;\n'
        '    }\n    if (status == K_EAGAIN) {', 1)
    assert broken != transport
    (path/'broken_transport.c').write_text(broken)
    subprocess.run(common + [str(fixture), str(root/sources[0]),
        str(path/'broken_transport.c'), str(root/sources[2]), str(root/sources[3]),
        '-o', str(path/'broken')], check=True)
    try:
        resource.setrlimit(resource.RLIMIT_CORE, (0, limit[1]))
        result = subprocess.run([str(path/'broken')], capture_output=True, text=True, timeout=30)
        assert result.returncode != 0 and 'due+3+DELAY' in result.stderr, result.stderr
    finally:
        resource.setrlimit(resource.RLIMIT_CORE, limit)
    print('Negative control: old successful completion erases concurrent ACK failure; regression rejects it')
