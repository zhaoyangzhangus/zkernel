"""Production UDP/TCP builders: datagram IDs, checksums, wrap and SMP."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <kernel/net_core.h>
static atomic_uint seen[65536];
static const uint8_t mac[6]={2,1,2,3,4,5};
static unsigned make_packet(unsigned variant){
 uint8_t frame[256],payload[33];size_t length=0;
 memset(payload,0x35,sizeof(payload));
 kstatus_t status=variant?
  net_tcp_build_ipv4(frame,sizeof(frame),mac,mac,0xc0a80106,0x279c46ef,
   50000,443,42,43,NET_TCP_FLAG_ACK|NET_TCP_FLAG_PSH,8192,payload,sizeof(payload),&length):
  net_udp_build_ipv4(frame,sizeof(frame),mac,mac,0xc0a80106,0xc0a80101,
   50000,53,payload,sizeof(payload),&length);
 assert(status==K_OK);
 assert(!net_checksum16(frame+14,20));
 if(variant){net_tcp_view_t v;assert(net_tcp_parse_ipv4(frame,length,&v)==K_OK);}
 else{net_udp_view_t v;assert(net_udp_parse_ipv4(frame,length,&v)==K_OK);}
 return ((unsigned)frame[18]<<8)|frame[19];
}
static void *worker(void *arg){
 unsigned variant=(unsigned)(uintptr_t)arg;
 for(unsigned i=0;i<8192;++i){
  unsigned id=make_packet(variant);
  assert(atomic_fetch_add_explicit(&seen[id],1,memory_order_relaxed)==0);
 }
 return 0;
}
int main(void){
 /* More than a full wire-ID cycle verifies natural 16-bit wrap. */
 unsigned previous=make_packet(0);
 for(unsigned i=0;i<131072;++i){
  unsigned current=make_packet(i&1);
  assert(current==((previous+1)&65535));previous=current;
 }
 pthread_t threads[4];
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,worker,(void*)(uintptr_t)(i&1)));
 for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 unsigned count=0;for(unsigned i=0;i<65536;++i)count+=atomic_load(&seen[i]);
 assert(count==32768);
}
'''
with tempfile.TemporaryDirectory(prefix='ipv4-identification-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(program)
    command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-pthread',
               '-I', str(root / 'include'), str(base / 'test.c'),
               str(root / 'kernel/net/core/core.c'), '-o', str(base / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(base / 'test')], check=True)
print('IPv4 identification: real UDP/TCP checksums, 131072 wrap cases, 32768 SMP-unique datagrams PASS')
