"""Whole network manager with fake NIC/ports; no host networking operations."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/net/manager.c').read_text()
source=source.replace('event = (os_net_event_t){0};',
                      '++event_builds; event = (os_net_event_t){0};')
code=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned event_builds;
''' + source + r'''
static bool present,link,ipv6,interrupts,deny_ref,reenter,mode_busy;
static uint32_t ip,gateway;
static uint8_t prefix,mac[6],address6[16];
static unsigned tcp_polls,retries,kicks,samples,sends;
static uint64_t clock_now=12345;
static message_port_t ports[2];
static os_net_event_t last_event;
uint64_t x86_read_tsc(void) {return clock_now;}
void socket_tcp_poll(uint64_t now) {assert(now==clock_now);++tcp_polls;}
bool e1000_polling_required(void) {return !mode_busy && !interrupts;}
bool e1000_schedule_deferred_poll(void) {++kicks;return true;}
void e1000_retry_deferred_poll(void) {++retries;}
bool e1000_hardware_present(void) {++samples;return present;}
bool e1000_link_up(void) {return link;}
uint32_t e1000_ipv4_address(void) {return ip;}
uint8_t e1000_ipv4_prefix_length(void) {return prefix;}
uint32_t e1000_ipv4_gateway(void) {return gateway;}
bool e1000_ipv6_address(uint8_t out[16]) {memcpy(out,address6,16);return ipv6;}
kstatus_t e1000_get_mac_address(uint8_t out[6]) {memcpy(out,mac,6);return K_OK;}
void rtl8126_emit_diagnostic(void) {}
void object_get(void *p) {
    message_port_t *port=p;assert(port==&ports[0] || port==&ports[1]);
    assert(atomic_fetch_add(&port->object.refs.value,1)>0);
}
bool object_try_get(void *p) {if(deny_ref)return false;object_get(p);return true;}
void object_put(void *p) {
    message_port_t *port=p;assert(atomic_fetch_sub(&port->object.refs.value,1)>1);
}
kstatus_t message_port_send(message_port_t *port,const void *data,size_t size) {
    assert(!atomic_load(&g_net_manager_lock.state) && size==sizeof(last_event));
    assert(atomic_load(&port->object.refs.value)>=3);
    memcpy(&last_event,data,size);++sends;
    if(reenter) {reenter=false;assert(net_manager_subscribe(&ports[1])==K_OK);}
    return K_EAGAIN; /* Sending failure must still release the temporary ref. */
}
static void sample(void) {g_net_manager_poll_divider=63;net_manager_poll();}
static void refs(unsigned a,unsigned b) {
    assert(atomic_load(&ports[0].object.refs.value)==a);
    assert(atomic_load(&ports[1].object.refs.value)==b);
}
int main(void) {
    for(unsigned i=0;i<2;++i) {
        ports[i].object.type=KOBJECT_TYPE_MESSAGE_PORT;
        atomic_init(&ports[i].object.refs.value,1);
    }
    net_manager_poll();assert(!tcp_polls && !samples);
    assert(net_manager_init());assert(tcp_polls==1 && samples==1 && !event_builds);
    unsigned before=samples;
    for(unsigned i=0;i<63;++i)net_manager_poll();
    assert(samples==before);net_manager_poll();assert(samples==before+1);
    assert(retries==tcp_polls && kicks==tcp_polls);
    present=true;link=true;ip=0x01020304;gateway=0x01020301;prefix=24;ipv6=true;
    memset(mac,0x59,6);memset(address6,0x75,16);
    sample();assert(!event_builds && g_net_manager.link_transitions==1);
    net_manager_status_t status;assert(net_manager_get_status(&status));
    assert(status.hardware_present && status.link_up && status.ipv6_configured);
    assert(status.ipv4_address==ip && status.ipv4_gateway==gateway && status.ipv4_prefix_length==24);
    assert(!memcmp(status.mac,mac,6) && !memcmp(status.ipv6_address,address6,16));
    assert(net_manager_subscribe(&ports[0])==K_OK);refs(2,1);
    sample();assert(!event_builds && !sends);
    link=false;sample();assert(event_builds==1 && sends==1);refs(2,1);
    os_net_event_t expected={0};expected.hdr.size=sizeof(expected);
    expected.hdr.version=OS_SYSCALL_ABI_VERSION;expected.type=OS_NET_EVENT_LINK_CHANGED;
    expected.status_flags=OS_NET_STATUS_HARDWARE_PRESENT|OS_NET_STATUS_IPV6_CONFIGURED;
    expected.link_transitions=2;assert(!memcmp(&last_event,&expected,sizeof(expected)));
    deny_ref=true;link=true;sample();assert(event_builds==1 && sends==1);refs(2,1);
    deny_ref=false;reenter=true;link=false;sample();
    assert(event_builds==2 && sends==2);refs(1,2);
    present=false;sample();assert(event_builds==3 && sends==3);
    assert(!last_event.status_flags && last_event.link_transitions==5);
    assert(net_manager_get_status(&status) && !status.hardware_present && !status.link_up);
    for(unsigned i=0;i<16;++i)assert(!status.ipv6_address[i]);
    for(unsigned i=0;i<6;++i)assert(!status.mac[i]);
    unsigned previous_kicks=kicks;interrupts=true;
    net_manager_poll();assert(kicks==previous_kicks);
    previous_kicks=kicks;
    for(unsigned i=0;i<10000;++i)net_manager_poll();
    assert(kicks==previous_kicks && retries==tcp_polls && event_builds==3);
    present=true;link=true;sample();
    previous_kicks=kicks;
    for(unsigned i=0;i<63;++i)net_manager_poll();
    assert(kicks==previous_kicks); /* MSI fast path stays interrupt-driven. */
    net_manager_poll();assert(kicks==previous_kicks);
    for(unsigned i=0;i<10000;++i)net_manager_poll();
    assert(kicks==previous_kicks); /* Loop count cannot accelerate the clock. */
    clock_now+=2000;net_manager_poll();assert(kicks==previous_kicks);
    clock_now=UINT64_MAX;net_manager_poll();assert(kicks==previous_kicks);
    clock_now=10;net_manager_poll();assert(kicks==previous_kicks);
    interrupts=false;mode_busy=true;
    for(unsigned i=0;i<10000;++i)net_manager_poll();
    assert(kicks==previous_kicks); /* Busy is not a request for fallback RX. */
    mode_busy=false;
    interrupts=false;previous_kicks=kicks;
    for(unsigned i=0;i<128;++i)net_manager_poll();
    assert(kicks==previous_kicks+128); /* Never double-kick at sample ticks. */
    assert(net_manager_subscribe(NULL)==K_OK);refs(1,1);
    ports[0].object.type=KOBJECT_TYPE_SOCKET;
    assert(net_manager_subscribe(&ports[0])==K_EINVAL);refs(1,1);
    puts("Net manager poll: MSI has no periodic RX fallback; TCP/retry clocks, status cadence, complete snapshots, subscriber lifetime and balanced refs PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-net-poll-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    for label,old,new in [
        ('unconditional RX polling','if (e1000_polling_required())', 'if (true)')]:
        assert old in code
        broken=code.replace(old,new)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-I'+str(root/'include'),'-fsanitize=address,undefined',
                        '-fno-pie','-no-pie','-x','c','-','-o',binary],
                       input=broken,text=True,check=True)
        assert subprocess.run([binary],capture_output=True,timeout=30).returncode != 0
        print('Negative control rejected: '+label)
