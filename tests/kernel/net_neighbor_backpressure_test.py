"""Actual protocol output + builders/caches + TCP completion on neighbor TX busy."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/net/protocol.c').read_text()
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "core_internal.h"
#include "socket_model.h"
#include "tcp_retry.h"
static unsigned preempt;
void sched_preempt_disable(void){++preempt;}
void sched_preempt_enable(void){assert(preempt);--preempt;}
uint64_t x86_timeout_ns_to_tsc(uint64_t ns){assert(ns==20000000ULL);return 20;}
int socket_tcp_syn_scale_ipv4(uint32_t a,uint16_t b,uint32_t c,uint16_t d,uint32_t e,uint32_t f){
 (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return -1;
}
int socket_tcp_syn_scale_ipv6(const uint8_t a[16],uint16_t b,const uint8_t c[16],uint16_t d,uint32_t e,uint32_t f){
 (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return -1;
}
kstatus_t e1000_transmit_status(e1000_state_t *s,const uint8_t *p,size_t n){
 (void)s;(void)p;(void)n;assert(0);return K_EIO;
}
bool e1000_transmit(e1000_state_t *s,const uint8_t *p,size_t n){
 (void)s;(void)p;(void)n;assert(0);return false;
}
void e1000_record_error(uint32_t error){(void)error;assert(0);}
'''
suffix=r'''
static e1000_state_t state;
static const uint8_t own[6]={2,1,2,3,4,5},peer[6]={2,6,7,8,9,10};
static const uint8_t s6[16]={0x20,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
static const uint8_t d6[16]={0x20,1,0,0,0,0,0,0,0,0,0,0,0,0,0,2};
static unsigned attempts,kind,mode;
static uint32_t expected_neighbor;
static kstatus_t tx_status;
static kstatus_t capture(void *context,const void *frame,size_t length){
 assert(context==&state && !preempt);++attempts;
 if(mode==0){
  if(kind<2){
   net_arp_view_t arp;
   assert(net_arp_parse_ipv4(frame,length,&arp)==K_OK);
   assert(arp.operation==NET_ARP_OPERATION_REQUEST && arp.target_address==expected_neighbor);
   assert(arp.sender_address==state.ipv4_address && !memcmp(arp.sender_mac,own,6));
  }else{
   net_ndp_view_t ndp;
   assert(net_ndp_parse_neighbor_solicitation(frame,length,&ndp)==K_OK);
   assert(!memcmp(ndp.source_address,s6,16) && !memcmp(ndp.target_address,d6,16));
  }
 }else{
  assert(!memcmp(frame,mode==2?own:peer,6));
  net_udp_view_t udp;net_ipv6_udp_view_t udp6;net_tcp_view_t tcp;
  if(kind==0)assert(net_udp_parse_ipv4(frame,length,&udp)==K_OK);
  else if(kind==2)assert(net_udp_parse_ipv6(frame,length,&udp6)==K_OK);
  else assert((kind==1?net_tcp_parse_ipv4(frame,length,&tcp):
                      net_tcp_parse_ipv6(frame,length,&tcp))==K_OK);
 }
 return tx_status;
}
int main(void){
 const kstatus_t statuses[]={K_OK,K_EAGAIN,K_EIO,K_EDEVREMOVED,K_ENOMEM,K_EINVAL};
 const uint8_t payload[30]={1,2,3};unsigned cases=0;
 net_device_init(&state.device,"test",own,1500,capture,&state);
 state.initialized=state.link_up=state.device.link_up=true;
 memcpy(state.mac,own,6);memcpy(state.ipv6_address,s6,16);
 state.ipv6_address_configured=true;state.ipv4_address=0xc0a80106;
 state.ipv4_prefix_length=24;state.ipv4_gateway=0xc0a80101;
 for(kind=0;kind<4;++kind)for(mode=0;mode<3;++mode)
 for(unsigned routed=0;routed<2;++routed)
 for(unsigned status=0;status<sizeof(statuses)/sizeof(*statuses);++status){
  net_arp_cache_init(&state.arp_cache);net_ipv6_neighbor_cache_init(&state.ndp_cache);
  uint32_t destination=mode==2?state.ipv4_address:(routed?0x01010101:0xc0a80102);
  expected_neighbor=routed?state.ipv4_gateway:destination;
  const uint8_t *destination6=mode==2?s6:d6;
  if(mode==1){
   assert(net_arp_cache_update(&state.arp_cache,expected_neighbor,peer,1)==K_OK);
   assert(net_ipv6_neighbor_cache_update(&state.ndp_cache,d6,peer,1)==K_OK);
  }
  attempts=0;tx_status=statuses[status];kstatus_t rc;
  if(kind==0)rc=e1000_udp_ipv4_output(&state,state.ipv4_address,1234,destination,53,payload,sizeof(payload));
  else if(kind==1)rc=e1000_tcp_ipv4_output(&state,state.ipv4_address,1234,destination,443,100,200,NET_TCP_FLAG_ACK,4096,payload,sizeof(payload));
  else if(kind==2)rc=e1000_udp_ipv6_output(&state,s6,1234,destination6,53,payload,sizeof(payload));
  else rc=e1000_tcp_ipv6_output(&state,s6,1234,destination6,443,100,200,NET_TCP_FLAG_ACK,4096,payload,sizeof(payload));
  assert(attempts==1 && !preempt);
  assert(rc==(mode==0 && tx_status==K_OK?K_EAGAIN:tx_status));
  if(kind&1){
   static socket_private_t flight;
   memset(&flight,0,sizeof(flight));
   atomic_init(&flight.closed,false);atomic_init(&flight.tcp_error,0);
   flight.tcp_state=SOCKET_TCP_ESTABLISHED;flight.tcp_send_pending=true;
   flight.tcp_send_unacknowledged=100;flight.tcp_send_next_sequence=130;
   flight.tcp_send_length=30;flight.tcp_send_deadline_tsc=10000;
   memcpy(flight.tcp_send_payload,payload,30);
   bool failed=socket_tcp_local_output_locked(&flight,100,130,rc,1000);
   assert(failed==(rc!=K_OK && rc!=K_EAGAIN));
   if(!failed){
    assert(flight.tcp_send_pending && !flight.tcp_send_failed);
    assert(flight.tcp_send_length==30 && !memcmp(flight.tcp_send_payload,payload,30));
    assert(flight.tcp_send_retransmits==0 && flight.tcp_send_deadline_tsc==10000);
    assert(flight.tcp_local_retry_tsc==(rc==K_EAGAIN?1020:0));
   }else assert(flight.tcp_send_failed && atomic_load(&flight.tcp_error)==rc);
  }
  ++cases;
 }
 puts("Neighbor output: 144 IPv4/IPv6 UDP/TCP route/cache/status cases; real ARP/NDP packets, retryable backpressure and retained TCP flight PASS");
 assert(cases==144);
}
'''
with tempfile.TemporaryDirectory(prefix='net-neighbor-busy-') as temp:
    flags=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
           '-fsanitize=address,undefined','-fno-pie','-no-pie',
           '-I'+str(root/'include'),'-I'+str(root/'kernel/drivers/net'),
           '-I'+str(root/'kernel/net')]
    obj=str(Path(temp)/'core.o')
    subprocess.run([*flags,'-c',str(root/'kernel/net/core/core.c'),'-o',obj],check=True)
    binary=str(Path(temp)/'test')
    def run(body,capture=False):
        subprocess.run([*flags,'-x','c','-','-x','none',obj,'-o',binary],
                       input=prefix+body+suffix,text=True,check=True)
        return subprocess.run([binary],capture_output=capture,timeout=30)
    run(source).check_returncode()
    anchor='return status == K_OK ? K_EAGAIN : status;'
    assert source.count(anchor)==2
    for index,label in enumerate(('ARP','NDP')):
        # Replace exactly one helper so each family's regression is independent.
        offset=source.index(anchor) if index==0 else source.rindex(anchor)
        broken=source[:offset]+source[offset:].replace(anchor,
                         'return status == K_OK ? K_EAGAIN : K_EIO;',1)
        assert run(broken,True).returncode != 0
        print('Negative control rejected: '+label+' backpressure converted to permanent EIO')
