"""Real receive parser: initialize metadata, never clear unused packet capacity."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/runtime.c').read_text()
receive = source[source.index('bool e1000_process_rx_frame('):
                 source.index('bool e1000_poll_receive(')]
enqueue = source[source.index('static bool e1000_enqueue_packet('):
                 source.index('static bool e1000_send_tcp_reply(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/net_core.h>
#include "core_internal.h"
static size_t zeroed, copied;
static unsigned queued;
static e1000_rx_packet_t captured;
static e1000_state_t *expected_state;
void *net_kernel_memset(void *,int,size_t);
void *net_kernel_memcpy(void *,const void *,size_t);
static void *tracked_zero(void *p,int v,size_t n){zeroed+=n;return net_kernel_memset(p,v,n);}
static void *tracked_copy(void *d,const void *s,size_t n){copied+=n;return net_kernel_memcpy(d,s,n);}
static const uint8_t *dma_start,*dma_end;
uint32_t e1000_select_software_queue(uint32_t count,uint32_t hash){
 assert(count==4);return hash%count;
}
bool e1000_queue_push_payload(e1000_software_queue_t *q,const e1000_rx_packet_t *p,
                              const uint8_t *payload){
 assert(expected_state && q>=expected_state->software_queues &&
        q<expected_state->software_queues+4);++queued;
 assert(payload>=dma_start && payload+p->payload_length<=dma_end);
 memcpy(&captured,p,offsetof(e1000_rx_packet_t,payload));
 memcpy(captured.payload,payload,p->payload_length);return true;
}
uint32_t e1000_flow_hash_ipv4(uint32_t a,uint32_t b,uint16_t c,uint16_t d,uint8_t p){
 return a^b^c^d^p;
}
uint32_t e1000_flow_hash_ipv6(const uint8_t a[16],const uint8_t b[16],uint16_t c,uint16_t d){
 return a[0]^b[0]^c^d;
}
bool e1000_address6_zero(const uint8_t a[16]){return !a[0];}
bool e1000_address6_equal(const uint8_t a[16],const uint8_t b[16]){return !memcmp(a,b,16);}
bool e1000_send_neighbor_advertisement(e1000_state_t *s,const net_ndp_view_t *v){(void)s;(void)v;return true;}
bool e1000_arp_reply(e1000_state_t *s,const net_arp_view_t *v){(void)s;(void)v;return true;}
void e1000_record_error(uint32_t e){(void)e;assert(0);}
uint64_t telemetry_timestamp(void){return 1;}
static uint64_t x86_read_tsc(void){return 1;}
void socket_tcp_poll(uint64_t t){(void)t;}
void sched_preempt_disable(void){}
void sched_preempt_enable(void){}
#define memset tracked_zero
#define memcpy tracked_copy
''' + enqueue + receive + r'''
#undef memset
#undef memcpy
int main(void){
 static e1000_state_t state;
 expected_state=&state;state.software_queue_count=4;
 static uint8_t payload[SOCKET_MAX_PAYLOAD+2],frame[SOCKET_MAX_PAYLOAD+128];
 const uint8_t sm[6]={2,1,2,3,4,5},dm[6]={2,6,7,8,9,10};
 const uint8_t s6[16]={0x20,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
 const uint8_t d6[16]={0x20,1,0,0,0,0,0,0,0,0,0,0,0,0,0,2};
 for(unsigned i=0;i<sizeof(payload);++i)payload[i]=(uint8_t)(i*17+3);
 unsigned cases=0,rejected=0;
 for(unsigned v=0;v<4;++v)for(unsigned n=0;n<sizeof(payload);++n){
  size_t length=0;kstatus_t rc;
  if(v==0)rc=net_udp_build_ipv4(frame,sizeof(frame),sm,dm,0xc0a80102,0xc0a80101,1234,4321,payload,n,&length);
  else if(v==1)rc=net_udp_build_ipv6(frame,sizeof(frame),sm,dm,s6,d6,1234,4321,payload,n,&length);
  else if(v==2)rc=net_tcp_build_ipv4(frame,sizeof(frame),sm,dm,0xc0a80102,0xc0a80101,1234,4321,123,456,NET_TCP_FLAG_ACK,4096,payload,n,&length);
  else rc=net_tcp_build_ipv6(frame,sizeof(frame),sm,dm,s6,d6,1234,4321,123,456,NET_TCP_FLAG_ACK,4096,payload,n,&length);
  assert(rc==K_OK);zeroed=copied=queued=0;memset(&captured,0xa5,sizeof(captured));
  dma_start=frame;dma_end=frame+length;
  bool ok=e1000_process_rx_frame(&state,frame,length);
  assert(zeroed==offsetof(e1000_rx_packet_t,payload));
  if(n>SOCKET_MAX_PAYLOAD){
   assert(!ok && !queued && copied==((v&1)?32U:0U)+(v>=2?12U:0U));
   ++rejected;continue;
  }
  assert(ok && queued==1 && captured.payload_length==n);
  assert(captured.family==((v&1)?OS_AF_INET6:OS_AF_INET4));
  assert(captured.protocol==(v<2?SOCKET_PROTOCOL_UDP:SOCKET_PROTOCOL_TCP));
  assert(captured.source_port==1234 && captured.destination_port==4321);
  e1000_rx_packet_t expected;
  memset(&expected,0,offsetof(e1000_rx_packet_t,payload));
  expected.family=(v&1)?OS_AF_INET6:OS_AF_INET4;
  expected.protocol=v<2?SOCKET_PROTOCOL_UDP:SOCKET_PROTOCOL_TCP;
  expected.source_port=1234;expected.destination_port=4321;
  expected.payload_length=n;
  if(v&1){
   memcpy(expected.source_address6,s6,16);
   memcpy(expected.destination_address6,d6,16);
  }else{
   expected.source_address=0xc0a80102;
   expected.destination_address=0xc0a80101;
  }
  if(v>=2){
   expected.flags=NET_TCP_FLAG_ACK;
   expected.sequence=123;expected.acknowledgement=456;expected.window=4096;
   memcpy(expected.source_mac,sm,6);memcpy(expected.destination_mac,dm,6);
  }
  assert(!memcmp(&captured,&expected,offsetof(e1000_rx_packet_t,payload)));
  assert(!memcmp(captured.payload,payload,n));
  assert(copied==((v&1)?32U:0U)+(v>=2?12U:0U));
  if(v<2)assert(!captured.flags && !captured.sequence && !captured.acknowledgement);
  else assert(captured.flags==NET_TCP_FLAG_ACK && captured.sequence==123 && captured.acknowledgement==456);
  for(unsigned i=n;i<sizeof(captured.payload);++i)assert(captured.payload[i]==0xa5);
  ++cases;
 }
 assert(rejected==4);
 unsigned before=queued;
 assert(!e1000_process_rx_frame(0,frame,14));
 assert(!e1000_process_rx_frame(&state,0,14));
 assert(!e1000_process_rx_frame(&state,frame,0) && queued==before);
 printf("RX initialization: %u actual IPv4/IPv6 UDP/TCP packets; direct DMA payload forwarding without staging copy, oversized rejection and tail canaries PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='net-rx-init-') as directory:
    binary = str(Path(directory) / 'test')
    bulk = str(Path(directory) / 'string.o')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fno-builtin', '-mno-sse', '-mno-mmx',
                    '-fsanitize=address,undefined', '-fno-pie',
                    '-I'+str(root/'include'),
                    '-Dmemcpy=net_kernel_memcpy', '-Dmemset=net_kernel_memset',
                    '-Dmemmove=net_kernel_memmove', '-Dmemcmp=net_kernel_memcmp',
                    '-c', str(root/'kernel/string.c'), '-o', bulk], check=True)
    command = ['cc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root/'include'), '-I'+str(root/'kernel/drivers/net'),
                    '-x', 'c', '-', str(root/'kernel/net/core/core.c'),
                    '-x', 'none', bulk, '-o', binary]
    subprocess.run(command,
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    old = program.replace(
        'memset(&packet, 0, offsetof(e1000_rx_packet_t, payload));',
        'memset(&packet, 0, sizeof(packet));')
    assert old != program
    subprocess.run(command, input=old, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, text=True, timeout=30,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode == -signal.SIGABRT and 'zeroed==' in result.stderr, result
    print('Negative control rejected: clearing the full receive payload exceeds initialization budget')
    # Preserve copy sizes but corrupt a metadata field: byte-count checks alone
    # must not let an address regression pass the receive optimization gate.
    corrupt = program.replace('packet.source_address = view.source_address;',
                              'packet.source_address = view.destination_address;')
    assert corrupt != program
    subprocess.run(command, input=corrupt, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, text=True, timeout=30,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode == -signal.SIGABRT and '&captured,&expected' in result.stderr, result
    print('Negative control rejected: corrupted source address despite unchanged copy budget')
    staged = program.replace('state, &packet, view.payload,',
        'state, &packet, packet.payload,')
    assert staged != program
    subprocess.run(command, input=staged, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, text=True, timeout=30,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode == -signal.SIGABRT and 'payload>=dma_start' in result.stderr, result
    print('Negative control rejected: forwarding uninitialized staging payload instead of DMA data')
