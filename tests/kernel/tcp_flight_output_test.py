"""First sends and retransmits must segment the retained flight identically."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tcp_output.h"
static unsigned calls,fail_at,mss,seen,wire_flags;
static uint32_t base_seq;
static uint8_t bytes[SOCKET_MAX_PAYLOAD];
static size_t total;
static kstatus_t output4(void *ctx,uint32_t src,uint16_t sp,uint32_t dst,uint16_t dp,
 uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *payload,size_t n) {
 (void)ctx;assert(src==1 && dst==2 && sp==3 && dp==4 && ack==5 && window==6);
 assert(n<=mss && n<=total-seen && seq==(uint32_t)(base_seq+seen));
 assert(flags==(seen+n<total?(wire_flags&~(NET_TCP_FLAG_PSH|NET_TCP_FLAG_FIN)):wire_flags));
 assert(!n || (payload==bytes+seen && !memcmp(payload,bytes+seen,n)));
 ++calls;
 if(calls==fail_at)return K_EAGAIN;
 seen+=n;return K_OK;
}
static kstatus_t output6(void *ctx,const uint8_t *src,uint16_t sp,const uint8_t *dst,uint16_t dp,
 uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *payload,size_t n) {
 assert(src[0]==1 && dst[0]==2);
 return output4(ctx,1,sp,2,dp,seq,ack,flags,window,payload,n);
}
int main(void) {
 /* The negotiated MSS never overflows the retained storage or exceeds
  * the current three-segment initial congestion budget. This is a memory
  * and protocol bound, not a claim about peer delayed-ACK performance. */
 for(unsigned family=0;family<2;++family){
  unsigned maximum=family?1440:1460;
  for(unsigned negotiated=1;negotiated<=maximum;++negotiated){
   size_t flight=socket_tcp_flight_limit(family?OS_AF_INET6:OS_AF_INET4,negotiated);
   assert(flight>0 && flight<=SOCKET_MAX_PAYLOAD && flight<=3U*negotiated);
   assert(flight>=negotiated);
  }
  unsigned fallback=family?1220:536;
  assert(socket_tcp_flight_limit(family?OS_AF_INET6:OS_AF_INET4,0)==3U*fallback);
 }
 uint8_t source[16]={1},destination[16]={2};unsigned cases=0;
 const uint16_t sizes[]={0,1,2,64,536,1220,1452,1460};
 const unsigned lengths[]={0,1,63,536,1452,1460,2023,4096};
 for(unsigned family=0;family<2;++family)
  for(unsigned s=0;s<8;++s)for(unsigned l=0;l<8;++l)
   for(unsigned wrap=0;wrap<2;++wrap)for(unsigned failure=0;failure<5;++failure) {
    total=lengths[l];mss=sizes[s]?sizes[s]:(family?1220:536);
    base_seq=wrap?UINT32_MAX-100:100;wire_flags=NET_TCP_FLAG_ACK|NET_TCP_FLAG_PSH;
    if(!total)wire_flags=NET_TCP_FLAG_SYN;
    for(unsigned retry=0;retry<2;++retry) {
     seen=calls=0;fail_at=failure;
     kstatus_t status=family?
      socket_tcp_output6_batch(output6,0,source,3,destination,4,base_seq,5,wire_flags,6,bytes,total,sizes[s]):
      socket_tcp_output4_batch(output4,0,1,3,2,4,base_seq,5,wire_flags,6,bytes,total,sizes[s]);
     unsigned packets=total?(total+mss-1)/mss:1;
     bool failed=failure && failure<=packets;
     assert(status==(failed?K_EAGAIN:K_OK));
     assert(calls==(failed?failure:packets));
     if(!failed)assert(seen==total);
     ++cases;
    }
   }
 printf("TCP flight segmentation: 2900 negotiated-MSS memory/congestion bounds and %u first/retransmit IPv4/IPv6 MSS/default/short/wrap/backpressure cases PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='tcp-flight-output-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/net'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
