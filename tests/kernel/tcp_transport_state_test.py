"""Compile real transport code; verify family parity and optional old/new traces."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--baseline', type=Path,
                    help='Compare unchanged open-socket traces; run without this option to verify close guards')
parser.add_argument('--serial', action='store_true')
args = parser.parse_args()
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


prefix = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <arch/x86_64/cpu.h>
#include <kernel/net_core.h>
#include "socket_model.h"
#include "tcp_output.h"
#include "tcp_retry.h"
static socket_t sockets[2];static socket_private_t states[2];
static socket_private_t initial;
static socket_tcp_connection_t table4[SOCKET_BINDING_COUNT];
static socket_tcp6_connection_t table6[SOCKET_BINDING_COUNT];
static spinlock_t binding;
static unsigned locked,refs[2],wakes[2],notifications[2],outputs[2];
static uint64_t tick,output_hash[2];
static int wire_status;static bool missing_output;
static unsigned which(const void *s){assert(s==&sockets[0] || s==&sockets[1]);return s==&sockets[1];}
static uint64_t hash(uint64_t h,const void *p,size_t n){const unsigned char *b=p;while(n--)h=(h^*b++)*1099511628211ULL;return h;}
uint64_t x86_read_tsc(void){return tick;}
uint64_t x86_timeout_ns_to_tsc(uint64_t ns){return ns;}
void socket_initialize_globals(void){}
spinlock_t *socket_binding_lock(void){return &binding;}
void socket_lock(spinlock_t *l){(void)l;assert(!locked);locked=1;}
void socket_unlock(spinlock_t *l){(void)l;assert(locked);locked=0;}
const socket_tcp_connection_t *socket_tcp_connection_at(uint32_t i){assert(locked && i<SOCKET_BINDING_COUNT);return &table4[i];}
uint32_t socket_registry_capacity_locked(void){assert(locked);return SOCKET_BINDING_COUNT;}
const socket_tcp6_connection_t *socket_tcp6_connection_at(uint32_t i){assert(locked && i<SOCKET_BINDING_COUNT);return &table6[i];}
socket_private_t *socket_private(socket_t *s){return s?s->protocol_state:0;}
bool object_try_get(void *s){assert(locked);++refs[which(s)];return true;}
void object_put(void *s){assert(!locked && refs[which(s)]);--refs[which(s)];}
uint32_t wake_all(wait_queue_t *q){assert(!locked);unsigned i=q==&sockets[1].waitq;assert(q==&sockets[i].waitq);++wakes[i];return 0;}
void object_notify_changed(void *s){assert(!locked);++notifications[which(s)];}
int kernel_serial_printf_serial_only(const char *format,...){(void)format;assert(!locked);return 0;}
void socket_copy(void *out,const void *in,size_t n){memcpy(out,in,n);}
static int emit(unsigned family,uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *data,size_t n){
 assert(!locked && refs[family]==1);++outputs[family];
 uint64_t values[]={seq,ack,flags,window,n};
 output_hash[family]=hash(output_hash[family],values,sizeof(values));
 if(n)output_hash[family]=hash(output_hash[family],data,n);
 return wire_status;
}
static kstatus_t output4(void *ctx,uint32_t src,uint16_t sp,uint32_t dst,uint16_t dp,uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *data,size_t n){
 assert(!ctx && src==1 && dst==2 && sp==123 && dp==456);return emit(0,seq,ack,flags,window,data,n);
}
static kstatus_t output6(void *ctx,const uint8_t src[16],uint16_t sp,const uint8_t dst[16],uint16_t dp,uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *data,size_t n){
 assert(!ctx && src[15]==1 && dst[15]==2 && sp==123 && dp==456);
 assert(!memcmp(src,states[1].local_address6,16) && !memcmp(dst,states[1].peer_address6,16));
 return emit(1,seq,ack,flags,window,data,n);
}
void socket_tcp_output_get(socket_tcp_ipv4_output_fn *fn,void **ctx){*fn=missing_output?0:output4;*ctx=0;}
void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *fn,void **ctx){*fn=missing_output?0:output6;*ctx=0;}
'''
prefix += '\n'.join(function('kernel/net/socket.c', signature) for signature in (
    'uint64_t socket_tcp_retransmit_deadline(',
    'static uint64_t socket_tcp_keepalive_deadline(',
    'void socket_tcp_keepalive_activity_locked(',
    'bool socket_tcp_keepalive_poll_locked('))
suffix = r'''
static uint32_t seed=813;
static uint32_t random32(void){seed=seed*1664525U+1013904223U;return seed;}
int main(void){
 unsigned state_cases[4]={0};
 unsigned fin_cases=0,retry_cases=0,timeout_cases=0;
 const uint16_t lengths[]={0,1,536,1460,SOCKET_MAX_PAYLOAD};
 const uint16_t sizes[]={0,536,1220,1460};
 const int errors[]={K_OK,K_EAGAIN,K_EIO,K_ENETUNREACH};
 const uint64_t times[]={1000000000ULL,0,UINT64_MAX-1000000000ULL};
 for(unsigned test=0;test<24000;++test){
  assert(!locked && !refs[0] && !refs[1]);
  memset(states,0,sizeof(states));memset(sockets,0,sizeof(sockets));
  memset(table4,0,sizeof(table4));memset(table6,0,sizeof(table6));
  memset(wakes,0,sizeof(wakes));memset(notifications,0,sizeof(notifications));memset(outputs,0,sizeof(outputs));
  output_hash[0]=output_hash[1]=1469598103934665603ULL;
  tick=times[test%3];wire_status=errors[test%4];missing_output=test%13==0;
  socket_private_t *p=&states[0];
  p->tcp_state=(random32()>>16)%4;++state_cases[p->tcp_state];
  // This oracle models ordinary loss/RTO, not deliberate receiver backpressure.
  // Zero-window persistence has its own send/ACK/poll integration cases.
  p->tcp_peer_window=8192;
  // Use independent bits; the low bit of an LCG alternates deterministically.
  p->tcp_send_pending=(random32()>>8)&1;p->tcp_fin_pending=(random32()>>8)&1;p->tcp_fin_sent=(random32()>>8)&1;
  p->tcp_send_next_sequence=random32();p->tcp_send_unacknowledged=random32();
  p->tcp_next_sequence=random32();p->tcp_peer_initial_sequence=random32();
  p->tcp_send_length=lengths[test%5];p->tcp_peer_mss=sizes[test%4];
  p->tcp_send_flags=p->tcp_state==SOCKET_TCP_SYN_SENT?NET_TCP_FLAG_SYN:NET_TCP_FLAG_ACK|NET_TCP_FLAG_PSH;
  p->tcp_send_retransmits=random32()%(SOCKET_TCP_MAX_RETRANSMITS+2);
  p->tcp_send_deadline_tsc=test%7==0?0:tick+(test%2?1000000ULL:-1000000ULL);
  p->tcp_local_retry_tsc=test%3==0?tick-1:0;
  p->tcp_keepalive=(random32()>>8)&1;p->tcp_keepidle=2;p->tcp_keepintvl=1;p->tcp_keepcnt=3;
  p->tcp_keep_probes=random32()%5;p->tcp_keep_deadline_tsc=test%3==0?0:tick+(test%2?1ULL:-1ULL);
  atomic_store(&p->stream_count,random32()%(SOCKET_STREAM_BUFFER_SIZE+1));
  for(unsigned i=0;i<SOCKET_MAX_PAYLOAD;++i)p->tcp_send_payload[i]=(uint8_t)(i*17+test);
  memcpy(&states[1],p,sizeof(*p));
  for(unsigned i=0;i<2;++i){
   sockets[i].family=i?OS_AF_INET6:OS_AF_INET4;sockets[i].protocol_state=test%37?&states[i]:0;
   states[i].local_address=1;states[i].peer_address=2;
   for(unsigned byte=0;byte<16;++byte){states[i].local_address6[byte]=(uint8_t)(byte*13+test);states[i].peer_address6[byte]=(uint8_t)(byte*11+test+1);}
   states[i].local_address6[15]=1;states[i].peer_address6[15]=2;
   states[i].local_port=123;states[i].peer_port=456;
  }
  unsigned slot=random32()%SOCKET_BINDING_COUNT;table4[slot].socket=&sockets[0];table6[slot].socket=&sockets[1];
  memcpy(&initial,&states[0],sizeof(initial));
  socket_tcp_poll(tick);
  if(sockets[0].protocol_state){
   bool fin=initial.tcp_state==SOCKET_TCP_ESTABLISHED && initial.tcp_fin_pending &&
    !initial.tcp_fin_sent && !initial.tcp_send_pending;
   bool valid_state=initial.tcp_state==SOCKET_TCP_SYN_SENT || initial.tcp_state==SOCKET_TCP_SYN_RECEIVED ||
    initial.tcp_state==SOCKET_TCP_ESTABLISHED;
   bool pending=valid_state && initial.tcp_send_pending && initial.tcp_send_deadline_tsc;
   bool due=pending && (int64_t)(tick-initial.tcp_send_deadline_tsc)>=0;
   bool local=pending && initial.tcp_local_retry_tsc && (int64_t)(tick-initial.tcp_local_retry_tsc)>=0;
   bool timeout=!fin && due && initial.tcp_send_retransmits>=SOCKET_TCP_MAX_RETRANSMITS;
   bool retry=!fin && !timeout && (due || local);
   if(fin){
    ++fin_cases;assert(p->tcp_send_next_sequence==initial.tcp_send_next_sequence+1U);
    assert(p->tcp_send_unacknowledged==initial.tcp_send_next_sequence && p->tcp_fin_sent);
    assert(!p->tcp_fin_pending && !p->tcp_send_retransmits && !p->tcp_send_length);
   }
   if(retry){
    ++retry_cases;assert(p->tcp_send_next_sequence==initial.tcp_send_next_sequence);
    assert(p->tcp_send_retransmits==initial.tcp_send_retransmits+(unsigned)due);
   }
   if(timeout){
    ++timeout_cases;assert(p->tcp_state==SOCKET_TCP_CLOSED && atomic_load(&p->tcp_error)==K_ETIMEDOUT);
    assert(!p->tcp_send_pending && !p->tcp_send_deadline_tsc && !p->tcp_local_retry_tsc);
    assert(wakes[0]==1 && notifications[0]==1 && !outputs[0]);
   }
   if((fin || retry) && !missing_output && wire_status!=K_OK && wire_status!=K_EAGAIN){
    assert(p->tcp_state==SOCKET_TCP_CLOSED && atomic_load(&p->tcp_error)==wire_status);
    assert(!p->tcp_send_pending && wakes[0]==1 && notifications[0]==1);
   }
   if((fin || retry) && missing_output)assert(p->tcp_send_pending && !outputs[0] && !wakes[0]);
  }
  size_t begin=offsetof(socket_private_t,tcp_next_sequence),end=offsetof(socket_private_t,tcp_send_payload);
  assert(!memcmp((char *)&states[0]+begin,(char *)&states[1]+begin,end-begin));
  size_t tail=offsetof(socket_private_t,tcp_keepalive),tail_end=offsetof(socket_private_t,peer);
  assert(!memcmp((char *)&states[0]+tail,(char *)&states[1]+tail,tail_end-tail));
  if(p->tcp_peer_mss)assert(output_hash[0]==output_hash[1] && outputs[0]==outputs[1]);
  assert(wakes[0]==wakes[1] && notifications[0]==notifications[1]);
  assert(!locked && !refs[0] && !refs[1]);
  uint64_t h=hash(1469598103934665603ULL,(char *)&states[0]+begin,end-begin);
  h=hash(h,(char *)&states[0]+tail,tail_end-tail);h=hash(h,output_hash,sizeof(output_hash));
  h=hash(h,outputs,sizeof(outputs));h=hash(h,wakes,sizeof(wakes));h=hash(h,notifications,sizeof(notifications));
  printf("%u %016llx\n",test,(unsigned long long)h);
 }
 for(unsigned state=0;state<4;++state)assert(state_cases[state]>1000);
 assert(fin_cases>100 && retry_cases>100 && timeout_cases>100);
#if VERIFY_CLOSE_GUARD
 // close() publishes closed before taking the registry lock to unregister.
 // A poll may have pinned the socket in that interval; its state must remain
 // untouched when it subsequently acquires the socket lock.
 for(unsigned phase=0;phase<6;++phase){
  memset(states,0,sizeof(states));memset(sockets,0,sizeof(sockets));
  memset(table4,0,sizeof(table4));memset(table6,0,sizeof(table6));
  memset(outputs,0,sizeof(outputs));memset(wakes,0,sizeof(wakes));memset(notifications,0,sizeof(notifications));
  tick=1000000000ULL;wire_status=K_OK;missing_output=false;
  for(unsigned family=0;family<2;++family){
   socket_private_t *p=&states[family];atomic_store(&p->closed,true);
   sockets[family].family=family?OS_AF_INET6:OS_AF_INET4;sockets[family].protocol_state=p;
   p->local_address=1;p->peer_address=2;p->local_address6[15]=1;p->peer_address6[15]=2;
   p->local_port=123;p->peer_port=456;
   p->tcp_state=phase==1?SOCKET_TCP_SYN_SENT:phase==2?SOCKET_TCP_SYN_RECEIVED:SOCKET_TCP_ESTABLISHED;
   p->tcp_peer_window=8192;
   p->tcp_send_next_sequence=1000;p->tcp_send_unacknowledged=999;
   p->tcp_next_sequence=2000;p->tcp_peer_mss=1460;
   p->tcp_fin_pending=phase==0;p->tcp_send_pending=phase>=1 && phase<=4;
   p->tcp_send_flags=NET_TCP_FLAG_ACK;p->tcp_send_length=phase==3;
   p->tcp_send_deadline_tsc=tick-1;p->tcp_local_retry_tsc=tick-1;
   p->tcp_send_retransmits=phase==4?SOCKET_TCP_MAX_RETRANSMITS:0;
   p->tcp_keepalive=true;p->tcp_keepidle=2;p->tcp_keepintvl=1;p->tcp_keepcnt=3;
   p->tcp_keep_deadline_tsc=tick-1;
  }
  memcpy(&initial,&states[0],sizeof(initial));
  table4[0].socket=&sockets[0];table6[0].socket=&sockets[1];
  socket_tcp_poll(tick);
  assert(!memcmp(&initial,&states[0],sizeof(initial)) && !memcmp(&initial,&states[1],sizeof(initial)));
  assert(!outputs[0] && !outputs[1] && !wakes[0] && !wakes[1] && !notifications[0] && !notifications[1]);
  assert(!locked && !refs[0] && !refs[1]);
 }
 // A probe sent before close/reset may finish afterwards. That completion
 // must not roll back its probe budget or install another retry deadline.
 for(unsigned terminal=0;terminal<2;++terminal){
  memset(&states[0],0,sizeof(states[0]));
  states[0].tcp_state=terminal?SOCKET_TCP_CLOSED:SOCKET_TCP_ESTABLISHED;
  atomic_store(&states[0].closed,!terminal);
  states[0].tcp_keep_probes=1;states[0].tcp_keep_deadline_tsc=123;
  memcpy(&initial,&states[0],sizeof(initial));
  socket_lock(&sockets[0].lock);
  socket_tcp_keepalive_output_failed_locked(&states[0],123,tick);
  socket_unlock(&sockets[0].lock);
  assert(!memcmp(&initial,&states[0],sizeof(initial)));
 }
#endif
}
'''
with tempfile.TemporaryDirectory(prefix='tcp-transport-state-') as directory:
    path = Path(directory)
    results = []
    sources = [root/'kernel/net/socket_transport.c']
    if args.baseline:
        sources.append(args.baseline)
    for file in sources:
        transport = '\n'.join(line for line in file.read_text().splitlines()
                              if not line.startswith('#include'))
        if file != sources[0]:
            # Old snapshots passed a start sequence to the completion helper.
            # This fixture has no concurrent ACK/output mutation, so its
            # captured exclusive end is exactly SND.NXT. Normalize that ABI
            # only; the real concurrent completion cases are gated separately.
            if 'socket_tcp_local_output_locked(private, sequence, send_end_sequence,' not in transport:
                transport = transport.replace(
                    'socket_tcp_local_output_locked(private, send_end_sequence,',
                    'socket_tcp_local_output_locked(private, sequence, send_end_sequence,')
                if 'uint32_t send_end_sequence' not in transport:
                    transport = transport.replace(
                        'socket_tcp_local_output_locked(private, sequence,',
                        'socket_tcp_local_output_locked(private, sequence, private->tcp_send_next_sequence,')
        (path/'test.c').write_text(prefix+transport+suffix)
        subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                        '-DOS_DEBUG_SERIAL='+str(int(args.serial)),
                        '-DVERIFY_CLOSE_GUARD='+str(int(not args.baseline)),
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I',str(root/'include'),'-I',str(root/'kernel/net'),
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        results.append(subprocess.check_output([str(path/'test')],text=True,timeout=45).splitlines())
        assert len(results[-1])==24000
    if args.baseline:
        assert results[0]==results[1],next((pair for pair in zip(*results) if pair[0]!=pair[1]),'length mismatch')
        print('TCP old/new differential: 24000 identical packet/state/wakeup traces with normalized completion-token ABI PASS')
print('TCP transport: 24000 IPv4/IPv6 state parity cases, FIN/SYN/retry/keepalive, output faults, sparse slots, wrap and balanced references PASS (ASan/UBSan)')
if not args.baseline:
    print('Closed transport: IPv4/IPv6 FIN/SYN/data/timeout/probe snapshots and late keepalive completion remain unchanged PASS')
