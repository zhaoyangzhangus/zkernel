"""Real send/ACK/poll/transport code: bounded pipelining and output races."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
def function(file,signature):
    text=(root/file).read_text();start=text.index(signature)
    end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <arch/x86_64/cpu.h>
#include <kernel/net_core.h>
#include "socket_model.h"
#include "tcp_output.h"
#include "tcp_retry.h"
static socket_t sock;static socket_private_t state;
static socket_tcp_connection_t table4[64];static socket_tcp6_connection_t table6[64];
static spinlock_t binding;
static unsigned held,refs,wakes,notifications,outputs,last_length;
static uint64_t tick=1000000000ULL;
static uint32_t base;
static uint8_t data[16384],received[16384],present[16384];
static int output_status,action;
static uint64_t nested_bytes;
static kstatus_t socket_send_stream(socket_t *,socket_private_t *,const void *,size_t,uint64_t *);
static int ack(uint32_t sequence,uint16_t window);
void socket_initialize_globals(void){}
uint64_t x86_read_tsc(void){return tick;}
uint64_t x86_timeout_ns_to_tsc(uint64_t n){return n;}
uint64_t socket_tcp_retransmit_deadline(uint64_t t){return t+1000000000ULL;}
void socket_lock(spinlock_t *l){(void)l;assert(!held);held=1;}
void socket_unlock(spinlock_t *l){(void)l;assert(held);held=0;}
spinlock_t *socket_binding_lock(void){return &binding;}
uint32_t socket_registry_capacity_locked(void){assert(held);return 64;}
const socket_tcp_connection_t *socket_tcp_connection_at(uint32_t i){assert(held&&i<64);return &table4[i];}
const socket_tcp6_connection_t *socket_tcp6_connection_at(uint32_t i){assert(held&&i<64);return &table6[i];}
int32_t socket_find_tcp_locked(uint32_t a,uint16_t b,uint32_t c,uint16_t d){(void)a;(void)b;(void)c;(void)d;assert(held);return 0;}
int32_t socket_find_tcp6_locked(const uint8_t a[16],uint16_t b,const uint8_t c[16],uint16_t d){(void)a;(void)b;(void)c;(void)d;assert(held);return 0;}
socket_private_t *socket_private(socket_t *s){assert(s==&sock);return &state;}
bool object_try_get(void *s){assert(s==&sock&&held);++refs;return true;}
void object_put(void *s){assert(s==&sock&&!held&&refs);--refs;}
void socket_zero(void *d,size_t n){memset(d,0,n);}
void socket_copy(void *d,const void *s,size_t n){memmove(d,s,n);}
uint32_t wake_all(wait_queue_t *q){assert(q==&sock.waitq&&!held);++wakes;return 0;}
void object_notify_changed(void *s){assert(s==&sock&&!held);++notifications;}
void object_notify_signaled(void *s){object_notify_changed(s);}
uint32_t socket_stream_write_locked(socket_private_t *p,const void *b,size_t n){(void)p;(void)b;(void)n;assert(0);return 0;}
static int emit(uint32_t sequence,uint8_t flags,const void *payload,size_t length){
 last_length=(unsigned)length;
 assert(!held && length<=1460 && flags==(NET_TCP_FLAG_ACK|(flags&(NET_TCP_FLAG_PSH|NET_TCP_FLAG_FIN))));
 if(!length){
  assert((flags==NET_TCP_FLAG_ACK || flags==(NET_TCP_FLAG_ACK|NET_TCP_FLAG_FIN)) &&
         sequence==state.tcp_send_next_sequence-1U);
  ++outputs;return output_status;
 }
 unsigned offset=sequence-base;assert(offset+length<=sizeof(data));
 assert(!memcmp(payload,data+offset,length));++outputs;
 int result=output_status;
 if(result==K_OK || action==2){
  memcpy(received+offset,payload,length);memset(present+offset,1,length);
 }
 if(action){
  int mode=action;action=0;
  assert(!ack(sequence+(uint32_t)length,8192));
  int previous=output_status;output_status=mode==1?K_EAGAIN:K_OK;
  size_t next=state.tcp_send_next_sequence-base;
  assert(socket_send_stream(&sock,&state,data+next,1460,&nested_bytes)==K_OK && nested_bytes==1460);
  output_status=previous;
 }
 return result;
}
static kstatus_t output4(void *c,uint32_t s,uint16_t sp,uint32_t d,uint16_t dp,
 uint32_t seq,uint32_t a,uint8_t flags,uint16_t win,const void *p,size_t n){
 (void)c;(void)s;(void)sp;(void)d;(void)dp;(void)a;(void)win;return emit(seq,flags,p,n);
}
static kstatus_t output6(void *c,const uint8_t s[16],uint16_t sp,const uint8_t d[16],uint16_t dp,
 uint32_t seq,uint32_t a,uint8_t flags,uint16_t win,const void *p,size_t n){
 (void)s;(void)d;return output4(c,1,sp,2,dp,seq,a,flags,win,p,n);
}
void socket_tcp_output_get(socket_tcp_ipv4_output_fn *fn,void **c){*fn=output4;*c=0;}
void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *fn,void **c){*fn=output6;*c=0;}
'''
code=prefix+'\n'.join(function('kernel/net/socket.c',s) for s in (
 'bool socket_address6_zero(', 'static uint64_t socket_tcp_keepalive_deadline(',
 'void socket_tcp_keepalive_activity_locked(', 'bool socket_tcp_keepalive_poll_locked(',
 'uint32_t socket_poll_state('))
code+=function('kernel/net/socket_io.c','static kstatus_t socket_send_stream(')
code+='\n'.join(function('kernel/net/socket_protocol.c',s) for s in (
 'static bool socket_tcp_reset_locked(', 'static kstatus_t socket_tcp_ack_target(',
 'kstatus_t socket_inject_tcp_ack_ipv4_reply(', 'kstatus_t socket_inject_tcp_ack_ipv6_reply('))
code+='\n'.join(line for line in (root/'kernel/net/socket_transport.c').read_text().splitlines()
                if not line.startswith('#include'))
code+=r'''
static int ack(uint32_t sequence,uint16_t window){
 const uint8_t src[16]={2},dst[16]={1};
 return sock.family==OS_AF_INET4?
  socket_inject_tcp_ack_ipv4_reply(2,443,1,123,9000,sequence,NET_TCP_FLAG_ACK,window,0):
  socket_inject_tcp_ack_ipv6_reply(src,443,dst,123,9000,sequence,NET_TCP_FLAG_ACK,window,0);
}
static void reset(unsigned family,bool wrap){
 assert(!held&&!refs);memset(&sock,0,sizeof(sock));memset(&state,0,sizeof(state));
 memset(table4,0,sizeof(table4));memset(table6,0,sizeof(table6));
 memset(present,0,sizeof(present));memset(received,0,sizeof(received));
 for(unsigned i=0;i<sizeof(data);++i)data[i]=(uint8_t)(i*17+i/256);
 sock.object.type=KOBJECT_TYPE_SOCKET;sock.type=OS_SOCK_STREAM;
 sock.family=family?OS_AF_INET6:OS_AF_INET4;sock.protocol_state=&state;
 state.tcp_state=SOCKET_TCP_ESTABLISHED;state.tcp_peer_window=8192;state.tcp_peer_mss=1460;
 base=wrap?UINT32_MAX-2000:1000;state.tcp_send_next_sequence=base;state.tcp_send_unacknowledged=base;
 state.tcp_next_sequence=9000;state.local_address=1;state.peer_address=2;
 state.local_port=123;state.peer_port=443;state.local_address6[0]=1;state.peer_address6[0]=2;
 if(family)table6[0].socket=&sock;else table4[0].socket=&sock;
 tick=1000000000ULL;outputs=wakes=notifications=0;output_status=K_OK;action=0;nested_bytes=0;
}
static void verify(unsigned start,unsigned length){
 assert(state.tcp_send_length==length && state.tcp_send_unacknowledged==base+start);
 assert(state.tcp_send_next_sequence==base+start+length);
 assert(!memcmp(state.tcp_send_payload,data+start,length));
 assert(!held&&!refs);
}
int main(void){
 // A dropped window-open ACK must not permanently stall an otherwise idle
 // writer. A blocked write arms the timer once, not on each retry. Probes
 // neither consume sequence space nor spend the bounded loss retry budget.
 for(unsigned family=0;family<2;++family)for(unsigned wrap=0;wrap<2;++wrap){
  reset(family,wrap);uint64_t n=99;
  assert(!ack(base,0));
  assert(socket_send_stream(&sock,&state,data,100,&n)==K_EAGAIN && n==0);
  uint64_t first=state.tcp_persist_deadline_tsc;
  assert(first==tick+1000000000ULL);
  for(unsigned i=0;i<100;++i){
   ++tick;assert(socket_send_stream(&sock,&state,data,100,&n)==K_EAGAIN);
   socket_tcp_poll(tick);assert(!outputs && state.tcp_persist_deadline_tsc==first);
  }
  for(unsigned i=0;i<12;++i){
   tick=state.tcp_persist_deadline_tsc;unsigned before=outputs;
   uint32_t previous_seconds=state.tcp_persist_seconds;
   output_status=i%3==0?K_EAGAIN:i%3==1?K_EIO:K_OK;
   socket_tcp_poll(tick);assert(outputs==before+1);
   assert(last_length==0);
   assert(state.tcp_state==SOCKET_TCP_ESTABLISHED && !atomic_load(&state.tcp_error));
   assert(!state.tcp_send_pending && state.tcp_send_next_sequence==base);
   assert(state.tcp_persist_seconds<=60 && state.tcp_persist_seconds>=2);
   assert(state.tcp_persist_seconds==(previous_seconds<30?previous_seconds*2:60));
   uint64_t next=state.tcp_persist_deadline_tsc;
   assert(next-tick==(uint64_t)state.tcp_persist_seconds*1000000000ULL);
   socket_tcp_poll(tick);assert(outputs==before+1);
  }
  output_status=K_OK;unsigned before=wakes;
  assert(!ack(base,8192) && wakes==before+1);
  socket_tcp_poll(++tick);assert(!state.tcp_persist_deadline_tsc);
  assert(socket_poll_state(&sock)&SOCKET_POLL_WRITE);
  assert(!socket_send_stream(&sock,&state,data,100,&n) && n==100);
  // Already retained data is probed one byte at SND.UNA, without retiring
  // it until an actual ACK arrives. Even an exhausted old RTO budget cannot
  // kill the connection during intentional zero-window flow control.
  assert(!ack(base,0));state.tcp_send_retransmits=SOCKET_TCP_MAX_RETRANSMITS;
  before=outputs;socket_tcp_poll(++tick);assert(outputs==before);
  for(unsigned i=0;i<10;++i){
   tick=state.tcp_persist_deadline_tsc;before=outputs;socket_tcp_poll(tick);
   assert(outputs==before+1 && last_length==1 && state.tcp_send_length==100);
   assert(!atomic_load(&state.tcp_error) && state.tcp_send_pending);
   socket_tcp_poll(tick);assert(outputs==before+1 && !atomic_load(&state.tcp_error));
  }
  assert(!ack(base,8192));before=outputs;socket_tcp_poll(++tick);
  assert(outputs>before && !state.tcp_persist_deadline_tsc);
  assert(!state.tcp_send_retransmits && !state.tcp_local_retry_tsc);
  assert(state.tcp_send_deadline_tsc>tick);
  // A probe ACK and a new write can race the older probe's failed return.
  assert(!ack(base,0));socket_tcp_poll(++tick);
  tick=state.tcp_persist_deadline_tsc;action=2;output_status=K_EIO;
  socket_tcp_poll(tick);verify(1,1559);
  assert(!atomic_load(&state.tcp_error) && nested_bytes==1460);
 }
 // No unbounded busy loop at TSC wrap or at an exact zero deadline.
 reset(0,false);state.tcp_peer_window=0;tick=UINT64_MAX-999999999ULL;
 uint64_t n;assert(socket_send_stream(&sock,&state,data,1,&n)==K_EAGAIN);
 assert(state.tcp_persist_deadline_tsc==1);
 socket_tcp_poll(tick);assert(!outputs);
 tick=1;socket_tcp_poll(tick);assert(outputs==1);
 state.tcp_fin_pending=true;assert(!ack(base,8192));socket_tcp_poll(++tick);
 assert(state.tcp_fin_sent && !state.tcp_persist_deadline_tsc);
 puts("TCP persistence: blocked writes, lost window updates, one-byte retained probes, capped backoff, no busy retry/loss timeout, reopened wake/retransmit, late-output ownership and TSC wrap PASS");
 // A probe's unused end-sequence snapshot is zero. Across sequence wrap,
 // zero can lie inside the live flight, so it must never be treated as an
 // ordinary failed output completion for that flight.
 reset(0,false);base=UINT32_MAX-49U;
 state.tcp_send_next_sequence=state.tcp_send_unacknowledged=base;
 assert(!socket_send_stream(&sock,&state,data,100,&n));assert(!ack(base,0));
 socket_tcp_poll(++tick);tick=state.tcp_persist_deadline_tsc;output_status=K_EIO;
 socket_tcp_poll(tick);assert(last_length==1 && !atomic_load(&state.tcp_error));
 verify(0,100);
 for(unsigned family=0;family<2;++family)for(unsigned wrap=0;wrap<2;++wrap){
  // Drain an entire stream through repeated partial progress; no accepted
  // byte may disappear or be assigned a second sequence. Include a tiny
  // advertised window so admission and writable readiness stay in agreement.
  const unsigned windows[]={8192,37};
  for(unsigned w=0;w<2;++w){
   reset(family,wrap);state.tcp_peer_window=windows[w];
   unsigned accepted=0,acknowledged=0;
   while(accepted<sizeof(data)){
    size_t request=sizeof(data)-accepted;if(request>4096)request=4096;
    uint64_t n=0;assert(socket_poll_state(&sock)&SOCKET_POLL_WRITE);
    assert(!socket_send_stream(&sock,&state,data+accepted,request,&n) && n>0 && n<=request);
    accepted+=(unsigned)n;verify(acknowledged,accepted-acknowledged);
    assert(state.tcp_send_length<=SOCKET_MAX_PAYLOAD && state.tcp_send_length<=windows[w]);
    unsigned progress=state.tcp_send_length<1460?state.tcp_send_length:1460;
    acknowledged+=progress;tick+=1000;
    assert(!ack(base+acknowledged,(uint16_t)windows[w]));
   }
   assert(!ack(base+accepted,(uint16_t)windows[w]) && !state.tcp_send_pending);
   for(unsigned i=0;i<sizeof(data);++i)assert(present[i]&&received[i]==data[i]);
  }
  reset(family,wrap);uint64_t bytes=0;
  assert(!socket_send_stream(&sock,&state,data,4096,&bytes) && bytes==4096);
  assert(!(socket_poll_state(&sock)&SOCKET_POLL_WRITE));
  assert(!ack(base+1460,8192));verify(1460,2636);
  assert(socket_poll_state(&sock)&SOCKET_POLL_WRITE);
  uint64_t deadline=state.tcp_send_deadline_tsc;
  assert(!socket_send_stream(&sock,&state,data+4096,4096,&bytes) && bytes==1460);
  verify(1460,4096);assert(state.tcp_send_deadline_tsc==deadline);
  assert(!(socket_poll_state(&sock)&SOCKET_POLL_WRITE));
  assert(!ack(base+5556,8192));assert(!state.tcp_send_pending);
  for(unsigned i=0;i<5556;++i)assert(present[i]&&received[i]==data[i]);
  // Successful append does not cover an earlier locally rejected prefix.
  reset(family,wrap);output_status=K_EAGAIN;
  assert(!socket_send_stream(&sock,&state,data,1000,&bytes) && bytes==1000);
  uint64_t retry=state.tcp_local_retry_tsc;deadline=state.tcp_send_deadline_tsc;
  output_status=K_OK;tick+=100;
  assert(!socket_send_stream(&sock,&state,data+1000,1000,&bytes) && bytes==1000);
  verify(0,2000);assert(state.tcp_local_retry_tsc==retry && state.tcp_send_deadline_tsc==deadline);
  tick=retry;socket_tcp_poll(tick);assert(!state.tcp_local_retry_tsc);
  for(unsigned i=0;i<2000;++i)assert(present[i]&&received[i]==data[i]);
  // Remaining peer window is measured after subtracting unacknowledged data.
  reset(family,wrap);assert(!socket_send_stream(&sock,&state,data,1000,&bytes));
  assert(!ack(base+500,800));verify(500,500);
  assert(!socket_send_stream(&sock,&state,data+1000,1000,&bytes) && bytes==300);verify(500,800);
  assert(!(socket_poll_state(&sock)&SOCKET_POLL_WRITE));
  unsigned before=wakes;assert(!ack(base+500,1200));assert(wakes==before+1);
  assert(socket_poll_state(&sock)&SOCKET_POLL_WRITE);
  assert(!socket_send_stream(&sock,&state,data+1300,1000,&bytes) && bytes==400);verify(500,1200);
  assert(!ack(base+500,0));assert(!(socket_poll_state(&sock)&SOCKET_POLL_WRITE));
  // 65535 is a valid wire window, not an absent-window sentinel. In
  // particular, a zero-window peer must be able to reopen to this value.
  before=wakes;assert(!ack(base+500,UINT16_MAX));
  assert(state.tcp_peer_window==UINT16_MAX && wakes==before+1);
  assert(socket_poll_state(&sock)&SOCKET_POLL_WRITE);
  before=outputs;assert(!socket_send_stream(&sock,&state,data,0,&bytes) && !bytes && outputs==before);
  state.tcp_fin_pending=true;assert(!(socket_poll_state(&sock)&SOCKET_POLL_WRITE));
  // ACK and a new append arrive before the older send returns successfully.
  // A failure on that new span must retain its timer despite old success.
  reset(family,wrap);action=1;
  assert(!socket_send_stream(&sock,&state,data,4096,&bytes) && bytes==4096 && nested_bytes==1460);
  verify(1460,4096);assert(state.tcp_local_retry_tsc==tick+20000000ULL);
  tick+=20000000ULL;socket_tcp_poll(tick);verify(1460,4096);
  assert(!state.tcp_local_retry_tsc);
  for(unsigned i=0;i<5556;++i)assert(present[i]&&received[i]==data[i]);
  // A fully ACKed old output cannot kill the later write on late EIO.
  reset(family,wrap);action=2;output_status=K_EIO;
  assert(!socket_send_stream(&sock,&state,data,1460,&bytes) && bytes==1460);
  verify(1460,1460);assert(!atomic_load(&state.tcp_error));
 }
 puts("TCP stream pipeline: IPv4/IPv6 wrap, partial-ACK refill, finite buffer/window credit, window-only wake, zero send, FIN exclusion, delayed success/EAGAIN/EIO ownership and retained retransmission PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='tcp-stream-pipeline-') as directory:
    path=Path(directory);(path/'test.c').write_text(code)
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-DOS_DEBUG_SERIAL=0','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/net'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=30)
    retry=(root/'kernel/net/tcp_retry.h').read_text()
    condition='''if (end_sequence == private->tcp_send_next_sequence &&
            (uint32_t)(private->tcp_send_unacknowledged - start_sequence) <
                (uint32_t)(end_sequence - start_sequence))'''
    assert condition in retry
    (path/'retry-end-only.h').write_text(retry.replace(condition,
        '(void)start_sequence;\n        if (end_sequence == private->tcp_send_next_sequence)'))
    marker='size_t room = socket_tcp_send_room_locked(socket->family, private);'
    assert marker in code
    stopped=code.replace(marker,
        'if (private->tcp_send_pending) { socket_unlock(&socket->lock); *bytes=0; return K_EAGAIN; }\n'+marker)
    window_update='''        if (decoded > private->tcp_peer_window) notify = true;
        private->tcp_peer_window = decoded;'''
    assert window_update in code
    sentinel=code.replace(window_update,
        '    if (window != UINT16_MAX) {\n'+window_update+'\n    }')
    for name,candidate in [('stop-and-wait',stopped),('end-only-completion',
            code.replace('"tcp_retry.h"','"retry-end-only.h"')),
            ('maximum-window-is-sentinel',sentinel),
            ('blocked-write-does-not-arm-probe',code.replace(
                'socket_tcp_persist_arm_locked(private, x86_read_tsc());','(void)0;')),
            ('probe-failure-poisons-retained-flight',code.replace(
                'if (!persist && (!keepalive || status != K_OK))',
                'if (!keepalive || status != K_OK)')),
            ('zero-window-spends-loss-budget',code.replace(
                '} else if (!persist_wait && (private->tcp_state == SOCKET_TCP_SYN_SENT ||',
                '} else if ((private->tcp_state == SOCKET_TCP_SYN_SENT ||'))]:
        assert candidate!=code,name
        (path/'test.c').write_text(candidate)
        subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                        '-DOS_DEBUG_SERIAL=0','-I'+str(root/'include'),'-I'+str(root/'kernel/net'),
                        str(path/'test.c'),'-o',str(path/'old')],check=True)
        result=subprocess.run([str(path/'old')],capture_output=True,timeout=30)
        assert result.returncode!=0 and b'Assertion' in result.stderr,(name,result.stderr)
        print('Negative control rejected:',name)
