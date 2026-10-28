"""Real TCP option parsing and handshake-gated peer MSS selection."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/net/socket_protocol.c').read_text()
apply = source[source.index('static void socket_tcp_apply_mss('):
               source.index('void socket_tcp_mss_ipv4(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/net/core/core.c"
#include "kernel/net/socket_model.h"
static socket_t target;
static socket_private_t state;
static unsigned releases;
socket_private_t *socket_private(socket_t *s){assert(s==&target);return &state;}
void socket_lock(spinlock_t *l){(void)l;}
void socket_unlock(spinlock_t *l){(void)l;}
void object_put(void *s){assert(s==&target);++releases;}
''' + apply + r'''
int main(void){
 uint8_t frame[256],mac[6]={2,1,2,3,4,5},address[16]={0x20,1};
 for(unsigned family=0;family<2;++family){
  size_t length=0;unsigned ip_length=family?40:20;
  if(family)assert(net_tcp_build_ipv6(frame,sizeof(frame),mac,mac,address,address,
    443,50000,1,2,NET_TCP_FLAG_SYN|NET_TCP_FLAG_ACK,8192,0,0,&length)==0);
  else assert(net_tcp_build_ipv4(frame,sizeof(frame),mac,mac,1,2,443,50000,1,2,
    NET_TCP_FLAG_SYN|NET_TCP_FLAG_ACK,8192,0,0,&length)==0);
  uint8_t *ip=frame+14,*tcp=ip+ip_length;
  for(unsigned mss=1;mss<=65535;mss+=257){
   tcp[12]=6<<4;tcp[20]=2;tcp[21]=4;tcp[22]=mss>>8;tcp[23]=mss;
   if(family)net_store_be16(ip+4,24);
   else{net_store_be16(ip+2,44);net_store_be16(ip+10,0);net_store_be16(ip+10,net_checksum16(ip,20));}
   net_store_be16(tcp+16,0);
   net_store_be16(tcp+16,family?net_transport_checksum_ipv6(address,address,6,tcp,24):
     net_transport_checksum_ipv4(1,2,6,tcp,24));
   net_tcp_view_t view;
   assert((family?net_tcp_parse_ipv6(frame,length,&view):
     net_tcp_parse_ipv4(frame,length,&view))==0);
   assert(view.peer_mss==mss&&view.payload_length==0);
  }
 }
 uint8_t tcp[60]={0};uint16_t mss=99;uint8_t shift;bool present;
#define net_tcp_parse_mss(t,h,m) net_tcp_parse_options(t,h,m,&shift,&present)
 assert(net_tcp_parse_mss(tcp,20,&mss)&&mss==0);
 tcp[20]=1;tcp[21]=2;tcp[22]=4;tcp[23]=5;tcp[24]=172;tcp[25]=0;
 assert(net_tcp_parse_mss(tcp,28,&mss)&&mss==1452);
 tcp[22]=1;assert(!net_tcp_parse_mss(tcp,28,&mss));
 tcp[22]=10;assert(!net_tcp_parse_mss(tcp,28,&mss));
 tcp[22]=4;tcp[23]=tcp[24]=0;assert(!net_tcp_parse_mss(tcp,28,&mss));
 tcp[20]=2;assert(!net_tcp_parse_mss(tcp,21,&mss));
 for(unsigned family=0;family<2;++family){
  target.family=family?OS_AF_INET6:OS_AF_INET4;
  const unsigned values[]={0,1,536,1220,1452,1460,65535};
  for(unsigned i=0;i<7;++i){
   memset(&state,0,sizeof(state));state.tcp_state=SOCKET_TCP_SYN_SENT;
   state.tcp_send_next_sequence=0;state.tcp_peer_mss=77;
   socket_tcp_apply_mss(&target,123,1,NET_TCP_FLAG_SYN|NET_TCP_FLAG_ACK,values[i]);
   assert(state.tcp_peer_mss==77);
   socket_tcp_apply_mss(&target,123,0,NET_TCP_FLAG_SYN|NET_TCP_FLAG_ACK,values[i]);
   unsigned limit=family?1440:1460;
   unsigned expected=values[i]?values[i]:(family?1220:536);
   if(expected>limit)expected=limit;
   assert(state.tcp_peer_mss==expected);
   state.tcp_state=SOCKET_TCP_ESTABLISHED;
   socket_tcp_apply_mss(&target,123,0,NET_TCP_FLAG_SYN|NET_TCP_FLAG_ACK,1);
   assert(state.tcp_peer_mss==expected);
   state.tcp_state=SOCKET_TCP_SYN_RECEIVED;state.tcp_peer_initial_sequence=123;
   socket_tcp_apply_mss(&target,124,0,NET_TCP_FLAG_SYN,1);
   assert(state.tcp_peer_mss==expected);
   socket_tcp_apply_mss(&target,123,0,NET_TCP_FLAG_SYN,1452);
   assert(state.tcp_peer_mss==(family?1440:1452));
  }
 }
 assert(releases==70);
}
'''
with tempfile.TemporaryDirectory(prefix='tcp-mss-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(root), '-I', str(root / 'include'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('TCP MSS: actual IPv4/IPv6 packets, malformed options, defaults, clamp, handshake sequence and reference release PASS')
