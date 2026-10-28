"""Unified production reply output: wire fields and exact ACK completion status."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/net/runtime.c').read_text()
reply = source[source.index('static bool e1000_send_tcp_reply('):
               source.index('bool e1000_dispatch_software_queues(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "core_internal.h"
static e1000_state_t driver;
static e1000_rx_packet_t packet;
static socket_tcp_reply_t reply;
static unsigned emitted,notified,family;
static kstatus_t wire_status,reported;
void sched_preempt_disable(void){assert(0);}
void sched_preempt_enable(void){assert(0);}
static kstatus_t capture(void *context,const void *frame,size_t length){
 assert(context==&driver);
 net_tcp_view_t view;
 assert((family ? net_tcp_parse_ipv6(frame,length,&view) :
                  net_tcp_parse_ipv4(frame,length,&view))==K_OK);
 assert(!memcmp(view.source_mac,packet.destination_mac,6));
 assert(!memcmp(view.destination_mac,packet.source_mac,6));
 if(family){
  assert(!memcmp(view.source_address6,packet.destination_address6,16));
  assert(!memcmp(view.destination_address6,packet.source_address6,16));
 }else{
  assert(view.source_address==packet.destination_address);
  assert(view.destination_address==packet.source_address);
 }
 assert(view.source_port==packet.destination_port && view.destination_port==packet.source_port);
 assert(view.sequence==reply.sequence && view.acknowledgement==reply.acknowledgement);
 assert(view.flags==reply.flags && view.window==reply.window && !view.payload_length);
 ++emitted;return wire_status;
}
static void notify(unsigned f,uint16_t lp,uint16_t pp,uint8_t flags,kstatus_t status){
 assert(f==family && lp==packet.destination_port && pp==packet.source_port);
 assert(flags==reply.flags && emitted==1 && !notified);
 ++notified;reported=status;
}
void socket_tcp_ack_output_ipv4(uint32_t local,uint16_t lp,uint32_t peer,uint16_t pp,
                                uint8_t flags,kstatus_t status){
 assert(local==packet.destination_address && peer==packet.source_address);
 notify(0,lp,pp,flags,status);
}
void socket_tcp_ack_output_ipv6(const uint8_t local[16],uint16_t lp,
                                const uint8_t peer[16],uint16_t pp,
                                uint8_t flags,kstatus_t status){
 assert(!memcmp(local,packet.destination_address6,16));
 assert(!memcmp(peer,packet.source_address6,16));
 notify(1,lp,pp,flags,status);
}
''' + reply + r'''
int main(void){
 const kstatus_t statuses[]={K_OK,K_EAGAIN,K_EIO,K_EINVAL,K_ENOMEM};
 const uint8_t own[6]={2,1,2,3,4,5},peer[6]={2,6,7,8,9,10};
 net_device_init(&driver.device,"reply",own,1500,capture,&driver);
 driver.device.link_up=true;
 memcpy(packet.destination_mac,own,6);memcpy(packet.source_mac,peer,6);
 packet.source_address=0xc0a80106;packet.destination_address=0xc0a80101;
 packet.source_address6[0]=0x20;packet.source_address6[15]=6;
 packet.destination_address6[0]=0x20;packet.destination_address6[15]=1;
 packet.source_port=49152;packet.destination_port=443;
 reply.valid=true;unsigned cases=0;
 for(family=0;family<2;++family)for(unsigned flags=0;flags<256;++flags)
 for(unsigned s=0;s<5;++s)for(unsigned mode=0;mode<4;++mode){
  packet.family=family?OS_AF_INET6:OS_AF_INET4;
  packet.payload_length=(mode&1)?4096:0;
  driver.tcp_loopback_peer_enabled=(mode&2)!=0;
  driver.tcp_loopback_data_ack_seen=false;
  reply.flags=flags;reply.sequence=flags?UINT32_MAX-flags:0;
  reply.acknowledgement=flags*65537U;reply.window=flags*257U;
  wire_status=statuses[s];emitted=notified=0;reported=123;
  bool ok=e1000_send_tcp_reply(&driver,&packet,&reply);
  assert(ok==(wire_status==K_OK) && emitted==1 && notified==1 && reported==wire_status);
  assert(driver.tcp_loopback_data_ack_seen==
         (ok && packet.payload_length!=0 && !driver.tcp_loopback_peer_enabled));
  ++cases;
 }
 emitted=notified=0;driver.tcp_loopback_data_ack_seen=false;
 for(unsigned f=0;f<256;++f){
  if(f==OS_AF_INET4 || f==OS_AF_INET6)continue;
  packet.family=f;assert(!e1000_send_tcp_reply(&driver,&packet,&reply));
 }
 packet.family=OS_AF_INET4;
 assert(!e1000_send_tcp_reply(0,&packet,&reply));
 assert(!e1000_send_tcp_reply(&driver,0,&reply));
 assert(!e1000_send_tcp_reply(&driver,&packet,0));
 reply.valid=false;assert(!e1000_send_tcp_reply(&driver,&packet,&reply));
 assert(!emitted && !notified && !driver.tcp_loopback_data_ack_seen);
 printf("Unified TCP replies: %u IPv4/IPv6 flag/status/loopback cases; real wire checksums/addresses, exact error completion and invalid-input silence PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='net-tcp-reply-') as directory:
    binary = str(Path(directory)/'test')
    variants = {
        'production': program,
        'wrong family': program.replace('bool ipv6 = packet->family == OS_AF_INET6;',
                                         'bool ipv6 = false;'),
        'lost output failure': program.replace('reply->flags, status);',
                                               'reply->flags, K_OK);'),
        'reversed address': program.replace(
            'packet->destination_address, packet->source_address,',
            'packet->source_address, packet->destination_address,'),
    }
    for label, candidate in variants.items():
        if label != 'production':
            assert candidate != program
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I'+str(root/'include'),'-I'+str(root/'kernel/drivers/net'),
                        '-x','c','-',str(root/'kernel/net/core/core.c'),'-o',binary],
                       input=candidate,text=True,check=True)
        result = subprocess.run([binary],capture_output=True,text=True,timeout=30,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        if label == 'production':
            assert result.returncode == 0,result.stderr
            print(result.stdout,end='')
        else:
            assert result.returncode != 0, label+' escaped the regression gate'
            print('Negative control rejected: '+label)
