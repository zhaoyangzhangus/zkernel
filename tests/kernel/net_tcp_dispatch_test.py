"""Exercise production queue dispatch; transport calls are traceable boundaries."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/runtime.c').read_text()
start = source.index('bool e1000_dispatch_software_queues(')
end = source.index('\n}', start) + 2
dispatch = source[start:end]

program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/drivers/net/core_internal.h"
static e1000_rx_packet_t input;
static bool queued, claimed, valid_reply, output_ok;
static kstatus_t inject_status;
static unsigned trace[16], count, sent;
static void event(unsigned code) { assert(count < 16); trace[count++] = code; }
static kstatus_t inject(unsigned code, socket_tcp_reply_t *reply) {
 event(code); if (reply) reply->valid = valid_reply; return inject_status;
}
static const e1000_rx_packet_t *borrow(e1000_software_queue_t *q) {
 assert(q && !claimed); if (!queued) return 0; claimed = true; return &input;
}
static void release(e1000_software_queue_t *q, const e1000_rx_packet_t *packet) {
 assert(q && queued && claimed && packet == &input); queued = claimed = false;
}
static bool send_reply(e1000_state_t *s, const e1000_rx_packet_t *p,
                       const socket_tcp_reply_t *r) {
 assert(s && p && r->valid); ++sent; return output_ok;
}
#define e1000_queue_borrow borrow
#define e1000_queue_release release
#define e1000_send_tcp_reply send_reply
#define e1000_tcp_self_test_peer_reply(...) false
#define e1000_build_tcp_reset_reply(...) false
#define e1000_tcp_loopback_peer_ack(...) false
#define e1000_tcp_loopback_peer_ack6(...) false
#define net_device_send(...) (assert(frame_length == 1), K_OK)
/* Keep the actual SYN output branch and its locals compiled, while the wire
 * builders themselves are covered by the window-scale and reply tests. */
#define BUILD() ((void)frame, (void)scale, frame_length = 1, K_OK)
#define net_tcp_build_ipv4_scaled(...) BUILD()
#define net_tcp_build_ipv6_scaled(...) BUILD()
'''
for family, base in (('4', 0), ('6', 10)):
    for name, code, result in (
        ('socket_inject_tcp_syn_ipv'+family+'_scaled', 1, 'inject'),
        ('socket_tcp_mss_ipv'+family, 2, 'event'),
        ('socket_tcp_syn_scale_ipv'+family, 3, 'scale'),
        ('socket_tcp_scale_ipv'+family, 4, 'event'),
        ('socket_inject_tcp_ack_ipv'+family+'_reply', 5, 'inject'),
        ('socket_inject_tcp_ack_ipv'+family+'_window', 6, 'window'),
        ('socket_inject_tcp_ipv'+family+'_reply', 7, 'inject'),
        ('socket_inject_udp_ipv6' if family == '6' else 'socket_inject_udp', 8, 'window'),
    ):
        expression = (f'inject({base+code}, &reply)' if result == 'inject' else
                      f'(event({base+code}), -1)' if result == 'scale' else
                      f'(event({base+code}), inject_status)' if result == 'window' else
                      f'event({base+code})')
        program += f'#define {name}(...) {expression}\n'
program += dispatch + r'''
int main(void) {
 static e1000_state_t device;
 device.software_queue_count = 1;
 unsigned cases = 0;
 const kstatus_t statuses[] = {K_OK, K_EAGAIN, K_EIO};
 for (unsigned f=0; f<2; ++f) for (unsigned flags=0; flags<256; ++flags)
 for (unsigned length=0; length<3; ++length) for (unsigned st=0; st<3; ++st)
 for (unsigned valid=0; valid<2; ++valid) for (unsigned output=0; output<2; ++output) {
  memset(&input, 0, sizeof(input));
  input.family = f ? OS_AF_INET6 : OS_AF_INET4;
  input.protocol = SOCKET_PROTOCOL_TCP;
  input.flags = flags; input.payload_length = length;
  queued=true; count=sent=0; valid_reply=valid; output_ok=output;
  inject_status=statuses[st];
  bool received=e1000_dispatch_software_queues(&device);
  unsigned expected[8], n=0, base=f*10;
  bool rst=flags & NET_TCP_FLAG_RST, syn=flags & NET_TCP_FLAG_SYN;
  bool ack=flags & NET_TCP_FLAG_ACK;
  bool pure=ack && !length && !(flags & (NET_TCP_FLAG_FIN|NET_TCP_FLAG_PSH));
  bool emits=false;
  if (rst) { expected[n++]=base+5; emits=valid; }
  else if (syn && !ack) {
   expected[n++]=base+1; expected[n++]=base+2;
   if (!st && valid) expected[n++]=base+3;
  } else if (syn || pure) {
   if (syn) { expected[n++]=base+4; expected[n++]=base+2; }
   expected[n++]=base+5; emits=!st && valid;
  } else {
   if (ack) expected[n++]=base+6;
   expected[n++]=base+7; emits=valid;
  }
  assert(!queued && !claimed && count==n && !memcmp(trace, expected, n*sizeof(*trace)));
  assert(sent==(unsigned)emits);
  assert(received==(!st && (!emits || output)));
  ++cases;
 }
 assert(!e1000_dispatch_software_queues(NULL));
 puts("TCP dispatch: all 18432 family/flags/payload/status/reply/output cases PASS");
 assert(cases==18432);
}
'''

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix='net-tcp-dispatch-') as directory:
    path = Path(directory)
    def run(text):
        (path/'test.c').write_text(text)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-no-pie', '-DOS_DEBUG_SERIAL=0',
                        '-I', str(root), '-I', str(root/'include'),
                        str(path/'test.c'), '-o', str(path/'test')], check=True)
        return subprocess.run([str(path/'test')], capture_output=True, text=True,
                              timeout=30, preexec_fn=no_core)
    result = run(program)
    assert result.returncode == 0, result.stderr
    print(result.stdout, end='')
    for label, old, new in (
        ('SYN options on ordinary ACK',
         'if ((packet->flags & NET_TCP_FLAG_SYN) != 0U) {', 'if (true) {'),
        ('data ACK routed as empty ACK',
         'packet->payload_length == 0U &&', 'true &&'),
        ('RST precedence removed',
         'if ((packet->flags & NET_TCP_FLAG_RST) != 0U) {', 'if (false) {'),
    ):
        assert old in dispatch
        mutated = program.replace(dispatch, dispatch.replace(old, new))
        assert run(mutated).returncode != 0, label
        print('Negative control rejected:', label)
