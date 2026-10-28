"""Production data dispatch must process ACKs carried by payload/FIN segments."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
runtime=(root/'kernel/drivers/net/runtime.c').read_text()
protocol=(root/'kernel/net/socket_protocol.c').read_text()
def block_at(text,start):
    opening=text.index('{',start)
    depth=1
    cursor=opening+1
    while depth:
        depth += (text[cursor]=='{')-(text[cursor]=='}')
        cursor+=1
    return text[opening+1:cursor-1]
blocks=[]
acks=[]
for family in ('4','6'):
    call=runtime.index('status = socket_inject_tcp_ipv'+family+'_reply(')
    start=runtime.rfind('} else {',0,call)
    blocks.append(block_at(runtime,start))
    handler=protocol.index('kstatus_t socket_inject_tcp_ack_ipv'+family+'_reply(')
    entry=block_at(protocol,handler)
    assert 'return socket_tcp_ack_target(target, sequence, acknowledgement, flags, window, reply);' in entry
    shared=protocol.index('static kstatus_t socket_tcp_ack_target(')
    start=protocol.index('if (private->tcp_state == SOCKET_TCP_ESTABLISHED &&\n'
                         '        private->tcp_send_pending &&\n'
                         '        acknowledgement == private->tcp_send_next_sequence)', shared)
    acks.append(protocol[start:protocol.index('    bool established',start)])
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define K_OK 0
#define K_EAGAIN (-11)
#define K_EIO (-5)
#define NET_TCP_FLAG_ACK 16
#define NET_TCP_FLAG_SYN 2
#define NET_TCP_FLAG_PSH 8
#define NET_TCP_FLAG_FIN 1
#define SOCKET_TCP_ESTABLISHED 2
typedef struct {
    bool valid;
    uint32_t acknowledgement;
} socket_tcp_reply_t;
typedef struct {
    unsigned tcp_state;
    bool tcp_send_pending;
    unsigned tcp_send_next_sequence,tcp_send_length,tcp_send_flags;
    unsigned tcp_send_unacknowledged,tcp_send_retransmits;
    uint64_t tcp_send_deadline_tsc;
    uint64_t tcp_local_retry_tsc;
    uint8_t tcp_send_payload[4096];
} private_t;
typedef struct {
    uint32_t source_address,destination_address,sequence,acknowledgement;
    uint16_t source_port,destination_port,window;
    uint8_t source_address6[16],destination_address6[16],flags,payload[16];
    size_t payload_length;
} packet_t;
typedef struct { unsigned unused; } e1000_state_t;
static private_t sender;
static unsigned notifications,ack_calls,rx_calls,replies;
static bool rx_full;
static char events[4];
static unsigned event_count;
static void socket_copy(void *d,const void *s,size_t n){memmove(d,s,n);}
static uint64_t x86_read_tsc(void){return 1000;}
static uint64_t socket_tcp_retransmit_deadline(uint64_t t){return t+1000;}
'''
for index, family in enumerate(('4','6')):
    addr='uint32_t' if family=='4' else 'const uint8_t*'
    program += f'''
static int socket_inject_tcp_ack_ipv{family}_window(
    {addr} src,uint16_t sp,{addr} dst,uint16_t dp,uint32_t sequence,
    uint32_t acknowledgement,uint8_t flags,uint16_t window) {{
    (void)src;(void)sp;(void)dst;(void)dp;(void)sequence;
    assert(flags&NET_TCP_FLAG_ACK); assert(window==8192);
    private_t* private=&sender; bool notify=false;
    events[event_count++]='A'; ++ack_calls;
{acks[index]}
    notifications+=notify;
    return K_OK;
}}
static int socket_inject_tcp_ipv{family}_reply(
    {addr} src,uint16_t sp,{addr} dst,uint16_t dp,uint32_t sequence,
    uint8_t flags,const void* buffer,size_t length,socket_tcp_reply_t* reply) {{
    (void)src;(void)sp;(void)dst;(void)dp;(void)buffer;
    assert(flags&NET_TCP_FLAG_ACK);
    ++rx_calls; events[event_count++]='R';
    reply->valid=true;
    reply->acknowledgement=sequence+(rx_full?0:length)+
        (!rx_full && (flags&NET_TCP_FLAG_FIN)!=0);
    return rx_full?K_EAGAIN:K_OK;
}}
static bool e1000_send_tcp_reply{'' if family=='4' else '6'}(
    e1000_state_t* state,packet_t* packet,socket_tcp_reply_t* reply) {{
    (void)state;(void)packet; assert(reply->valid); ++replies; return true;
}}
static int dispatch{family}(packet_t packet) {{
    e1000_state_t storage={{0}},*state=&storage; int status=K_OK;
{blocks[index]}
    return status;
}}
'''
program += r'''
static void reset(void) {
    sender=(private_t){.tcp_state=SOCKET_TCP_ESTABLISHED,
        .tcp_send_pending=true,.tcp_send_next_sequence=1000,
        .tcp_send_length=100,.tcp_send_flags=NET_TCP_FLAG_ACK|NET_TCP_FLAG_PSH,
        .tcp_send_unacknowledged=900,.tcp_send_retransmits=3,
        .tcp_send_deadline_tsc=1234};
    notifications=ack_calls=rx_calls=replies=event_count=0; rx_full=false;
}
int main(void) {
    int (*dispatch[])(packet_t)={dispatch4,dispatch6};
    for (unsigned family=0;family<2;++family) {
        for (unsigned variant=0;variant<4;++variant) {
            reset();
            packet_t packet={.flags=NET_TCP_FLAG_ACK,.sequence=2000,
                .acknowledgement=1000,.window=8192,.payload_length=4};
            if (variant==1) packet.flags|=NET_TCP_FLAG_PSH;
            if (variant==2) { packet.flags|=NET_TCP_FLAG_FIN; packet.payload_length=0; }
            if (variant==3) rx_full=true;
            assert(dispatch[family](packet)==(rx_full?K_EAGAIN:K_OK));
            assert(!sender.tcp_send_pending && !sender.tcp_send_length);
            assert(!sender.tcp_send_flags && !sender.tcp_send_retransmits);
            assert(!sender.tcp_send_deadline_tsc && sender.tcp_send_unacknowledged==1000);
            assert(ack_calls==1 && rx_calls==1 && replies==1 && notifications==1);
            assert(event_count==2 && events[0]=='A' && events[1]=='R');
            /* Duplicate acknowledgements must not notify twice. */
            event_count=0;
            assert(dispatch[family](packet)==(rx_full?K_EAGAIN:K_OK));
            assert(notifications==1);
        }
        reset();
        packet_t packet={.flags=NET_TCP_FLAG_ACK,.sequence=2000,
            .acknowledgement=950,.window=8192,.payload_length=4};
        assert(dispatch[family](packet)==K_OK);
        assert(sender.tcp_send_pending && sender.tcp_send_length==50 && sender.tcp_send_unacknowledged==950);
        assert(notifications==1 && ack_calls==1 && rx_calls==1 && replies==1);
        /* An ACK beyond SND.NXT cannot confirm bytes never transmitted. */
        reset();
        packet.acknowledgement=1001;
        assert(dispatch[family](packet)==K_OK);
        assert(sender.tcp_send_pending && sender.tcp_send_length==100);
        assert(sender.tcp_send_unacknowledged==900 && !notifications);
        /* A numerically large ACK can be valid partial progress across wrap. */
        reset();
        sender.tcp_send_next_sequence=32;
        sender.tcp_send_unacknowledged=UINT32_MAX-67;
        packet.acknowledgement=UINT32_MAX-15;
        assert(dispatch[family](packet)==K_OK);
        assert(sender.tcp_send_pending && sender.tcp_send_length==48);
        assert(sender.tcp_send_unacknowledged==UINT32_MAX-15 && notifications==1);
        event_count=0;
        packet.acknowledgement=32;
        assert(dispatch[family](packet)==K_OK);
        assert(!sender.tcp_send_pending && !sender.tcp_send_length);
        assert(sender.tcp_send_unacknowledged==32 && notifications==2);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-piggyback-ack-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-Wno-unused-function','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('IPv4/IPv6 piggyback ACK: data, PSH, FIN, RX-full, duplicate/partial/future ACK and sequence wrap passed')
