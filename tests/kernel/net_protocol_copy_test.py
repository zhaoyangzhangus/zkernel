"""Actual driver protocol outputs: bulk-copy and original byte-loop wire parity."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
protocol = (root/'kernel/drivers/net/protocol.c').read_text()
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "core_internal.h"
static unsigned depth;
/* This parity test sends ACK/data only; handshake lookup must stay cold. */
int socket_tcp_syn_scale_ipv4(uint32_t a,uint16_t b,uint32_t c,uint16_t d,uint32_t e,uint32_t f){
 (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;assert(0);return -1;
}
int socket_tcp_syn_scale_ipv6(const uint8_t a[16],uint16_t b,const uint8_t c[16],uint16_t d,uint32_t e,uint32_t f){
 (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;assert(0);return -1;
}
void sched_preempt_disable(void){++depth;}
void sched_preempt_enable(void){assert(depth);--depth;}
kstatus_t e1000_transmit_status(e1000_state_t *s,const uint8_t *p,size_t n){
 (void)s;(void)p;(void)n;assert(0);return K_EIO;
}
bool e1000_transmit(e1000_state_t *s,const uint8_t *p,size_t n){
 (void)s;(void)p;(void)n;assert(0);return false;
}
void e1000_record_error(uint32_t error){(void)error;assert(0);}
'''
suffix = r'''
static e1000_state_t state;
static uint8_t payload[4200],last[2048];
static const uint8_t *input;
static size_t processed,last_length;
static unsigned sent;
static kstatus_t capture(void *context,const void *frame,size_t n){
 assert(context==&state && n<=sizeof(last) && !depth);
 memcpy(last,frame,n);last_length=n;++sent;
 assert(fwrite(&n,sizeof(n),1,stdout)==1 && fwrite(frame,1,n,stdout)==n);
 net_tcp_view_t tcp;net_udp_view_t udp;net_ipv6_udp_view_t udp6;
 if(net_tcp_parse_ipv4(last,n,&tcp)==K_OK || net_tcp_parse_ipv6(last,n,&tcp)==K_OK){
  assert(tcp.source_port==1234 && tcp.destination_port==4321);
  assert(tcp.sequence==100+processed && tcp.acknowledgement==200 && tcp.window==4096);
  assert(!memcmp(tcp.payload,input+processed,tcp.payload_length));
  processed+=tcp.payload_length;
 }else if(net_udp_parse_ipv4(last,n,&udp)==K_OK){
  assert(udp.source_port==1234 && udp.destination_port==4321);
  assert(!memcmp(udp.payload,input,udp.payload_length));processed+=udp.payload_length;
 }else if(net_udp_parse_ipv6(last,n,&udp6)==K_OK){
  assert(udp6.source_port==1234 && udp6.destination_port==4321);
  assert(!memcmp(udp6.payload,input,udp6.payload_length));processed+=udp6.payload_length;
 }
 return K_OK;
}
int main(void){
 const uint8_t own[6]={2,1,2,3,4,5},peer[6]={2,6,7,8,9,10};
 const uint8_t s6[16]={0x20,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
 const uint8_t d6[16]={0x20,1,0,0,0,0,0,0,0,0,0,0,0,0,0,2};
 const unsigned lengths[]={0,1,63,64,65,512,1440,1452,1460,1472,2048,4096};
 for(unsigned i=0;i<sizeof(payload);++i)payload[i]=(uint8_t)(i*17+3);
 net_device_init(&state.device,"test",own,1500,capture,&state);
 state.initialized=state.link_up=state.device.link_up=true;
 memcpy(state.mac,own,6);memcpy(state.ipv6_address,s6,16);
 state.ipv6_address_configured=true;state.ipv4_address=0xc0a80101;
 unsigned cases=0;
 for(unsigned v=0;v<4;++v)for(unsigned mode=0;mode<3;++mode)
 for(unsigned off=0;off<8;++off)for(unsigned j=0;j<sizeof(lengths)/sizeof(*lengths);++j){
  unsigned n=lengths[j],destination=mode?0xc0a80102:state.ipv4_address;
  const uint8_t *destination6=mode?d6:s6;
  net_arp_cache_init(&state.arp_cache);net_ipv6_neighbor_cache_init(&state.ndp_cache);
  if(mode==1){
   assert(net_arp_cache_update(&state.arp_cache,destination,peer,1)==K_OK);
   assert(net_ipv6_neighbor_cache_update(&state.ndp_cache,d6,peer,1)==K_OK);
  }
  input=payload+off;processed=sent=0;
  kstatus_t rc;
  if(v==0)rc=e1000_udp_ipv4_output(&state,state.ipv4_address,1234,destination,4321,input,n);
  else if(v==1)rc=e1000_tcp_ipv4_output(&state,state.ipv4_address,1234,destination,4321,100,200,NET_TCP_FLAG_ACK,4096,input,n);
  else if(v==2)rc=e1000_udp_ipv6_output(&state,s6,1234,destination6,4321,input,n);
  else rc=e1000_tcp_ipv6_output(&state,s6,1234,destination6,4321,100,200,NET_TCP_FLAG_ACK,4096,input,n);
  bool oversized=!(v&1) && n>(v==0?1472U:1452U);
  assert(rc==(oversized?K_EINVAL:mode==2?K_EAGAIN:K_OK));
  if(oversized)assert(!sent && !processed);
  else if(mode==2)assert(sent==1 && !processed);
  else{
   unsigned mss=v==1?1460U:1440U;
   assert(sent==((v&1)?(n?(n+mss-1)/mss:1):1) && processed==n);
   assert(!memcmp(last,mode==0?own:peer,6));
  }
  assert(!depth);
  assert(fwrite(&rc,sizeof(rc),1,stdout)==1 && fwrite(&sent,sizeof(sent),1,stdout)==1);
  ++cases;
 }
 for(unsigned unsolicited=0;unsolicited<2;++unsolicited){
  net_ndp_view_t request={0},view;
  memcpy(request.target_address,s6,16);
  if(!unsolicited){memcpy(request.source_address,d6,16);memcpy(request.source_mac,peer,6);}
  sent=0;assert(e1000_send_neighbor_advertisement(&state,&request) && sent==1);
  assert(net_ndp_parse_neighbor_advertisement(last,last_length,&view)==K_OK);
  assert(view.flags==(NET_NDP_FLAG_OVERRIDE|(unsolicited?0:NET_NDP_FLAG_SOLICITED)));
  assert(!memcmp(view.target_address,s6,16) && !memcmp(view.target_mac,own,6));
  if(unsolicited)assert(view.destination_address[0]==0xff && view.destination_address[15]==1 && last[0]==0x33 && last[1]==0x33);
  else assert(!memcmp(view.destination_address,d6,16) && !memcmp(view.destination_mac,peer,6));
 }
 fprintf(stderr,"Protocol outputs: %u IPv4/IPv6 UDP/TCP alignment/length/route cases plus both NDP advertisements PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='net-protocol-copy-') as directory:
    objects = []
    flags = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
             '-fno-builtin', '-mno-sse', '-mno-mmx', '-fsanitize=address,undefined',
             '-fno-pie', '-I'+str(root/'include'), '-I'+str(root/'kernel/drivers/net')]
    rename = ['-Dmemcpy=proto_memcpy', '-Dmemset=proto_memset',
              '-Dmemmove=proto_memmove', '-Dmemcmp=proto_memcmp']
    for name, source in (('core','kernel/net/core/core.c'),('string','kernel/string.c')):
        obj = directory+'/'+name+'.o'; objects.append(obj)
        subprocess.run([*flags, *rename, '-c', str(root/source), '-o', obj], check=True)
    outputs = []
    for original in (False, True):
        body = protocol
        if original:
            body = 'static void original_copy(void *d,const void *s,size_t n){\n' \
                   'unsigned char *o=d;const unsigned char *i=s;while(n--!=0)*o++=*i++;}\n' \
                   + body.replace('memcpy(', 'original_copy(')
            assert body.count('original_copy(')==9
        binary = directory+'/test'
        subprocess.run([*flags, *rename, '-no-pie', '-x', 'c', '-', '-x', 'none',
                        *objects, '-o', binary], input=prefix+body+suffix, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, timeout=30)
        assert result.returncode==0, result.stderr.decode(errors='replace')
        outputs.append(result.stdout)
        if not original:
            print(result.stderr.decode(),end='')
    assert outputs[0]==outputs[1], 'Complete wire/status transcripts differ'
    print(f'Original byte-loop oracle: all {len(outputs[0])} wire/status bytes match exactly')
    for name, old, new in (
        ('truncated local MAC',
         'memcpy(destination_mac, state->mac, sizeof(destination_mac));',
         'memcpy(destination_mac, state->mac, sizeof(destination_mac)-1);'
         'destination_mac[sizeof(destination_mac)-1]=0;'),
        ('wrong solicited neighbor MAC',
         'memcpy(destination_mac, request->source_mac, sizeof(destination_mac));',
         'memcpy(destination_mac, state->mac, sizeof(destination_mac));'),
    ):
        broken = protocol.replace(old, new, 1)
        assert broken != protocol, name
        subprocess.run([*flags, *rename, '-no-pie', '-x', 'c', '-', '-x', 'none',
                        *objects, '-o', binary], input=prefix+broken+suffix,
                       text=True, check=True)
        result = subprocess.run(
            [binary], capture_output=True, timeout=30,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert result.returncode == -signal.SIGABRT and b'!memcmp' in result.stderr, (
            name, result.returncode, result.stderr.decode(errors='replace'))
        print('Negative control rejected:', name)
