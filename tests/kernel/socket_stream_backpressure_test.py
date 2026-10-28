"""Production stream send accepts retained data exactly once on TX EAGAIN."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/net/socket_io.c').read_text()
function = source[source.index('static kstatus_t socket_send_stream('):
                  source.index('kstatus_t socket_send(')]
program = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <arch/x86_64/cpu.h>
#include <kernel/net_core.h>
#include "socket_internal.h"
#include "socket_model.h"
#include "tcp_output.h"
#include "tcp_retry.h"
static socket_t sock;
static socket_private_t state;
static unsigned outputs;
static unsigned wakes,notifications,locked;
static kstatus_t output_status;
static bool missing_output;
static const uint8_t *expected;
static size_t expected_length;
static uint32_t expected_sequence;
static size_t emitted;
static kstatus_t output4(void *ctx,uint32_t src,uint16_t sp,uint32_t dst,uint16_t dp,
 uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *data,size_t n){
 (void)ctx;(void)src;(void)sp;(void)dst;(void)dp;(void)ack;(void)window;
 assert(flags==(NET_TCP_FLAG_ACK|(emitted+n==expected_length?NET_TCP_FLAG_PSH:0)));
 assert(seq==(uint32_t)(expected_sequence+emitted)&&n<=expected_length-emitted&&
        !memcmp(data,expected+emitted,n));
 assert(state.tcp_send_pending&&state.tcp_send_next_sequence==(uint32_t)(expected_sequence+expected_length));
 emitted+=n;++outputs;return output_status;
}
static kstatus_t output6(void *ctx,const uint8_t src[16],uint16_t sp,
 const uint8_t dst[16],uint16_t dp,uint32_t seq,uint32_t ack,uint8_t flags,
 uint16_t window,const void *data,size_t n){
 (void)src;(void)dst;return output4(ctx,0,sp,0,dp,seq,ack,flags,window,data,n);
}
void socket_tcp_output_get(socket_tcp_ipv4_output_fn *fn,void **ctx){*fn=missing_output?0:output4;*ctx=0;}
void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *fn,void **ctx){*fn=missing_output?0:output6;*ctx=0;}
void socket_lock(spinlock_t *l){(void)l;assert(!locked);locked=1;}
void socket_unlock(spinlock_t *l){(void)l;assert(locked);locked=0;}
void socket_copy(void *out,const void *in,size_t n){memmove(out,in,n);}
uint64_t socket_tcp_retransmit_deadline(uint64_t now){return now+100;}
uint64_t x86_read_tsc(void){return 1000;}
uint64_t x86_timeout_ns_to_tsc(uint64_t ns){return ns;}
socket_private_t *socket_private(socket_t *s){assert(s==&sock);return &state;}
bool object_try_get(void *p){(void)p;assert(0);return false;}
void object_put(void *p){(void)p;assert(0);}
uint32_t socket_stream_write_locked(socket_private_t *p,const void *b,size_t n){(void)p;(void)b;(void)n;assert(0);return 0;}
uint32_t wake_all(wait_queue_t *q){assert(q==&sock.waitq && !locked);++wakes;return 0;}
void object_notify_changed(void *p){assert(p==&sock && !locked);++notifications;}
void object_notify_signaled(void *p){(void)p;assert(0);}
''' + function + r'''
int main(void){
 for(unsigned family=0;family<2;++family){
  memset(&state,0,sizeof(state));sock.family=family?OS_AF_INET6:OS_AF_INET4;
  state.tcp_state=SOCKET_TCP_CLOSED;state.tcp_send_failed=true;
  const int errors[]={K_ECONNREFUSED,K_ECONNRESET};
  for(unsigned i=0;i<2;++i){
   atomic_store(&state.tcp_error,errors[i]);uint64_t bytes=99;
   assert(socket_send_stream(&sock,&state,"x",1,&bytes)==errors[i] && bytes==0);
  }
 }
 uint8_t data[SOCKET_MAX_PAYLOAD],saved[SOCKET_MAX_PAYLOAD];
 const unsigned lengths[]={1,512,1460,2023,SOCKET_MAX_PAYLOAD};
 for(unsigned family=0;family<2;++family)for(unsigned i=0;i<5;++i){
  memset(&state,0,sizeof(state));memset(&sock,0,sizeof(sock));
  sock.family=family?OS_AF_INET6:OS_AF_INET4;
  state.tcp_state=SOCKET_TCP_ESTABLISHED;state.tcp_peer_window=8192;
  state.tcp_peer_mss=SOCKET_MAX_PAYLOAD;
  expected_sequence=UINT32_MAX-1024;state.tcp_send_next_sequence=expected_sequence;
  expected_length=lengths[i];expected=data;emitted=0;
  state.tcp_peer_window=(uint16_t)expected_length;
  for(unsigned j=0;j<expected_length;++j)data[j]=(uint8_t)(j*37+i);
  memcpy(saved,data,expected_length);uint64_t bytes=99;
  output_status=K_EAGAIN;unsigned before=outputs;
  assert(socket_send_stream(&sock,&state,data,expected_length,&bytes)==K_OK);
  assert(bytes==expected_length&&outputs==before+1&&state.tcp_send_pending);
  assert(state.tcp_send_unacknowledged==expected_sequence);
  assert(state.tcp_send_length==expected_length&&!state.tcp_send_failed);
  assert(!memcmp(state.tcp_send_payload,saved,expected_length));
  memset(data,0xcc,sizeof(data));bytes=99;
  assert(socket_send_stream(&sock,&state,data,1,&bytes)==K_EAGAIN);
  assert(!bytes&&outputs==before+1&&!memcmp(state.tcp_send_payload,saved,expected_length));
  /* After ACK, only a genuinely new write gets the next sequence. */
  state.tcp_send_pending=false;expected_sequence=state.tcp_send_next_sequence;
  expected_length=1;output_status=K_OK;emitted=0;
  assert(socket_send_stream(&sock,&state,data,1,&bytes)==K_OK&&bytes==1);
  state.tcp_send_pending=false;expected_sequence=state.tcp_send_next_sequence;
  output_status=K_EIO;emitted=0;
  assert(socket_send_stream(&sock,&state,data,1,&bytes)==K_EIO&&!bytes);
  assert(state.tcp_send_failed&&!state.tcp_send_pending);
  assert(atomic_load(&state.tcp_error)==K_EIO && state.tcp_state==SOCKET_TCP_CLOSED);
  assert(!state.tcp_send_deadline_tsc && !state.tcp_local_retry_tsc);
  assert(wakes==outputs/3 && notifications==wakes);
 }
 for(unsigned family=0;family<2;++family){
  const unsigned limits[]={0,1,536,1220,1452,1460};
  for(unsigned i=0;i<6;++i){
   memset(&state,0,sizeof(state));sock.family=family?OS_AF_INET6:OS_AF_INET4;
   state.tcp_state=SOCKET_TCP_ESTABLISHED;state.tcp_peer_window=8192;
   state.tcp_peer_mss=limits[i];state.tcp_send_next_sequence=100;
   expected_sequence=100;expected=data;
   unsigned mss=limits[i]?limits[i]:(family?1220:536);
   expected_length=mss*3U; if(expected_length>2023)expected_length=2023;
   emitted=0;
   output_status=K_OK;uint64_t bytes=0;
   assert(socket_send_stream(&sock,&state,data,2023,&bytes)==K_OK);
   assert(bytes==expected_length&&state.tcp_send_length==expected_length);
  }
 }
 /* A 16 KiB TLS record formerly required 12 stop-and-wait ACK rounds.
  * Model one complete cumulative ACK per bounded retained flight. */
 for(unsigned family=0;family<2;++family) {
  memset(&state,0,sizeof(state));sock.family=family?OS_AF_INET6:OS_AF_INET4;
  state.tcp_state=SOCKET_TCP_ESTABLISHED;state.tcp_peer_window=8192;
  state.tcp_peer_mss=1460;output_status=K_OK;
  size_t remaining=16384;unsigned rounds=0;
  while(remaining) {
   size_t request=remaining<sizeof(data)?remaining:sizeof(data);
   expected_length=request;expected=data;emitted=0;
   expected_sequence=state.tcp_send_next_sequence;
   uint64_t bytes=0;
   assert(socket_send_stream(&sock,&state,data,request,&bytes)==K_OK && bytes==request);
   assert(emitted==bytes);remaining-=bytes;++rounds;state.tcp_send_pending=false;
  }
  assert(rounds==4);
 }
 /* A nonzero peer window smaller than MSS still permits a short write. */
 memset(&state,0,sizeof(state));sock.family=OS_AF_INET4;
 state.tcp_state=SOCKET_TCP_ESTABLISHED;state.tcp_peer_window=37;state.tcp_peer_mss=1460;
 expected_length=37;expected=data;expected_sequence=0;emitted=0;output_status=K_OK;
 uint64_t accepted=0;
 assert(socket_send_stream(&sock,&state,data,2023,&accepted)==K_OK && accepted==37);
 puts("Stream send: bounded MSS flights, 16KiB TLS record 12->4 ACK rounds, sub-MSS window short writes, retained EAGAIN, wrap and fatal errors PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='stream-backpressure-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(root / 'include'), '-I', str(root / 'kernel/net'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
    broken = program.replace(
        '        if (error != 0) {',
        '        if (output_status == K_EAGAIN) { *bytes=0; return K_EAGAIN; }\n'
        '        if (error != 0) {', 1)
    assert broken != program
    (path / 'old.c').write_text(broken)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-I', str(root / 'include'),
                    '-I', str(root / 'kernel/net'), str(path / 'old.c'),
                    '-o', str(path / 'old')], check=True)
    import resource
    limit = resource.getrlimit(resource.RLIMIT_CORE)
    try:
        resource.setrlimit(resource.RLIMIT_CORE, (0, limit[1]))
        assert subprocess.run([str(path / 'old')], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode != 0
    finally:
        resource.setrlimit(resource.RLIMIT_CORE, limit)
print('Negative control rejected: old EAGAIN reports retained write unaccepted')
