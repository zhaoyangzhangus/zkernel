"""Real TCP WS packets: both families, checksums, bounds and legacy parity.

This tests wire support, not live connection negotiation or throughput.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/net/core/core.c"
static uint8_t mac[6]={2,1,2,3,4,5},address[16]={0x20,1};
static kstatus_t build(unsigned family,uint8_t *frame,size_t capacity,
                      uint8_t flags,int scale,unsigned bytes,size_t *length) {
 const uint8_t payload[]={0,0xff,17,3,0};
 atomic_store(&g_net_ipv4_identification,0);
 return family?net_tcp_build_ipv6_scaled(frame,capacity,mac,mac,address,address,
    443,50000,UINT32_MAX-3,9,flags,65535,scale,payload,bytes,length):
    net_tcp_build_ipv4_scaled(frame,capacity,mac,mac,1,2,443,50000,
    UINT32_MAX-3,9,flags,65535,scale,payload,bytes,length);
}
static kstatus_t parse(unsigned family,const uint8_t *frame,size_t length,
                       net_tcp_view_t *view) {
 return family?net_tcp_parse_ipv6(frame,length,view):
               net_tcp_parse_ipv4(frame,length,view);
}
static void checksum(unsigned family,uint8_t *frame,unsigned tcp_length) {
 uint8_t *ip=frame+14,*tcp=ip+(family?40:20);
 if(family)net_store_be16(ip+4,tcp_length);
 else {
  net_store_be16(ip+2,20+tcp_length);net_store_be16(ip+10,0);
  net_store_be16(ip+10,net_checksum16(ip,20));
 }
 net_store_be16(tcp+16,0);
 net_store_be16(tcp+16,family?net_transport_checksum_ipv6(address,address,6,tcp,tcp_length):
    net_transport_checksum_ipv4(1,2,6,tcp,tcp_length));
}
int main(void) {
 unsigned cases=0;
 for(unsigned family=0;family<2;++family)
 for(unsigned mode=0;mode<4;++mode)
 for(int scale=-1;scale<=14;++scale)
 for(unsigned bytes=0;bytes<=5;bytes+=5){
  uint8_t flags=mode==0?NET_TCP_FLAG_SYN:mode==1?NET_TCP_FLAG_SYN|NET_TCP_FLAG_ACK:
    mode==2?NET_TCP_FLAG_ACK:NET_TCP_FLAG_RST|NET_TCP_FLAG_ACK;
  unsigned header=mode<2?(scale<0?24:28):20;
  unsigned expected=14+(family?40:20)+header+bytes;
  for(unsigned capacity=expected-1;capacity<=expected;++capacity){
   uint8_t frame[256];memset(frame,0xa5,sizeof(frame));size_t length=123;
   kstatus_t result=build(family,frame,capacity,flags,scale,bytes,&length);
   if(capacity<expected){
    assert(result==K_EINVAL && length==123);
    for(unsigned i=0;i<sizeof(frame);++i)assert(frame[i]==0xa5);
    continue;
   }
   assert(result==K_OK && length==expected && frame[expected]==0xa5);
   net_tcp_view_t v;memset(&v,0xa5,sizeof(v));
   assert(parse(family,frame,length,&v)==K_OK);
   assert(v.window==65535 && v.sequence==UINT32_MAX-3 && v.acknowledgement==9);
   assert(v.header_length==header && v.flags==flags && v.payload_length==bytes);
   const uint8_t payload[]={0,0xff,17,3,0};assert(!memcmp(v.payload,payload,bytes));
   assert(v.peer_mss==(mode<2?(family?1440:1460):0));
   assert(v.peer_window_scale_present==(mode<2 && scale>=0));
   assert(v.peer_window_scale==(mode<2 && scale>=0?scale:0));
   if(mode>=2 || scale==-1){
    uint8_t legacy[256];size_t old_length=0;atomic_store(&g_net_ipv4_identification,0);
    assert((family?net_tcp_build_ipv6(legacy,sizeof(legacy),mac,mac,address,address,
      443,50000,UINT32_MAX-3,9,flags,65535,payload,bytes,&old_length):
      net_tcp_build_ipv4(legacy,sizeof(legacy),mac,mac,1,2,443,50000,
      UINT32_MAX-3,9,flags,65535,payload,bytes,&old_length))==K_OK);
    assert(old_length==length && !memcmp(frame,legacy,length));
   }
   ++cases;
  }
 }
 // Invalid outgoing scales must fail before writing any byte, for all flags.
 for(unsigned family=0;family<2;++family)
 for(int scale=-128;scale<=127;++scale){
  if(scale>=-1 && scale<=14)continue;
  for(unsigned mode=0;mode<2;++mode){
   uint8_t frame[256];memset(frame,0xa5,sizeof(frame));size_t length=123;
   assert(build(family,frame,sizeof(frame),mode?NET_TCP_FLAG_ACK:NET_TCP_FLAG_SYN,
                scale,0,&length)==K_EINVAL && length==123);
   for(unsigned i=0;i<sizeof(frame);++i)assert(frame[i]==0xa5);
  }
 }
 // Received values over 14 are clamped; WS on non-SYN is never negotiated.
 for(unsigned family=0;family<2;++family)
 for(unsigned syn=0;syn<2;++syn)
 for(unsigned shift=0;shift<256;++shift){
  uint8_t frame[256];size_t length=0;
  assert(build(family,frame,sizeof(frame),NET_TCP_FLAG_SYN,3,0,&length)==K_OK);
  uint8_t *tcp=frame+14+(family?40:20);tcp[27]=shift;
  if(!syn)tcp[13]=NET_TCP_FLAG_ACK;
  checksum(family,frame,28);
  net_tcp_view_t v;assert(parse(family,frame,length,&v)==K_OK);
  assert(v.peer_window_scale_present==!!syn);
  assert(v.peer_window_scale==(syn?(shift>14?14:shift):0));
 }
 // Actual checksummed packets with malformed WS lengths, including truncation.
 for(unsigned family=0;family<2;++family)
 for(unsigned bad=0;bad<8;++bad){
  if(bad==3)continue;
  uint8_t frame[256];size_t length=0;
  assert(build(family,frame,sizeof(frame),NET_TCP_FLAG_SYN,3,0,&length)==K_OK);
  uint8_t *tcp=frame+14+(family?40:20);tcp[26]=bad;checksum(family,frame,28);
  net_tcp_view_t v;assert(parse(family,frame,length,&v)==K_EIO);
 }
 // First valid WS wins; NOP/EOL/unknown options and unaligned parsing are bounded.
 uint8_t tcp[60]={0};uint16_t mss;uint8_t shift;bool present;
 tcp[13]=NET_TCP_FLAG_SYN;
 tcp[20]=3;tcp[21]=3;tcp[22]=0;tcp[23]=1;
 tcp[24]=3;tcp[25]=3;tcp[26]=14;tcp[27]=0;
 assert(net_tcp_parse_options(tcp,28,&mss,&shift,&present) && present && shift==0);
 tcp[20]=254;tcp[21]=2;tcp[22]=1;tcp[23]=3;tcp[24]=3;tcp[25]=7;tcp[26]=0;
 assert(net_tcp_parse_options(tcp,28,&mss,&shift,&present) && present && shift==7);
 tcp[20]=0;tcp[21]=3;tcp[22]=1;
 assert(net_tcp_parse_options(tcp,28,&mss,&shift,&present) && !present && shift==0);
 tcp[20]=3;assert(!net_tcp_parse_options(tcp,21,&mss,&shift,&present));
 printf("TCP WS wire: %u IPv4/IPv6 packet cases; scales 0..14, incoming clamp, SYN-only semantics, legacy parity, payload/checksums, exact bounds, invalid lengths/scales and option ordering PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='tcp-ws-wire-') as directory:
    path = Path(directory)
    unit = path / 'test.c'
    unit.write_text(program)
    command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I'+str(root), '-I'+str(root/'include')]
    subprocess.run(command + [str(unit), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=20)
    # An empty implementation must not pass merely because legacy packets work.
    source = (root/'kernel/net/core/core.c').read_text()
    broken = source.replace('} else if (kind == 3U) {', '} else if (false && kind == 3U) {')
    assert broken != source
    # Keep the original file's relative include valid in the temporary copy.
    broken = broken.replace('#include "internal.h"',
                            '#include "kernel/net/core/internal.h"')
    (path/'broken.c').write_text(broken)
    unit.write_text(program.replace('"kernel/net/core/core.c"', '"broken.c"'))
    subprocess.run(command + [str(unit), '-o', str(path/'broken')], check=True)
    result = subprocess.run([str(path/'broken')], capture_output=True, timeout=20)
    assert result.returncode != 0 and b'v.peer_window_scale_present' in result.stderr
    print('Negative control rejected: ignoring WS loses the negotiated offer')
