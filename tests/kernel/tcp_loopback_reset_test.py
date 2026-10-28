"""Run production reset dispatch guards for synthetic and live TCP peers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/runtime.c').read_text()


def function(name):
    start = source.index('static bool ' + name + '(')
    return source[start:source.index('\n}', start) + 2]


guards = []
cursor = 0
for family in range(2):
    start = source.index('                if (status == K_ENOENT', cursor)
    end = source.index('\n                }', start) + len('\n                }')
    guards.append(source[start:end])
    cursor = end

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define K_OK 0
#define K_EIO (-5)
#define K_ENOENT (-2)
#define SOCKET_PROTOCOL_TCP 6
#define NET_TCP_FLAG_FIN 1
#define NET_TCP_FLAG_SYN 2
#define NET_TCP_FLAG_RST 4
#define NET_TCP_FLAG_ACK 16
typedef struct {
    bool phy_loopback, tcp_loopback_peer_enabled, tcp_loopback_synack_seen;
} e1000_state_t;
typedef struct {
    unsigned protocol, flags, payload_length;
    uint32_t sequence, acknowledgement;
    uint8_t payload[4096];
} e1000_rx_packet_t;
typedef struct {
    bool valid;
    unsigned flags, window;
    uint32_t sequence, acknowledgement;
} socket_tcp_reply_t;
static unsigned resets;
static socket_tcp_reply_t last;
static bool send_reset(e1000_state_t *state, const e1000_rx_packet_t *packet,
                       const socket_tcp_reply_t *reply) {
    (void)state; (void)packet;
    assert(reply->valid); ++resets; last = *reply; return true;
}
#define e1000_send_tcp_reply send_reset
''' + function('e1000_build_tcp_reset_reply') + '\n' + function('e1000_tcp_self_test_peer_reply')

for family, guard in enumerate(guards):
    program += f'''
static void dispatch{family}(e1000_state_t *state, e1000_rx_packet_t packet,
                             int status) {{
{guard}
    assert(status != K_EIO);
}}
'''

program += r'''
int main(void) {
    void (*dispatch[])(e1000_state_t *, e1000_rx_packet_t, int) = {
        dispatch0, dispatch1
    };
    unsigned cases = 0;
    for (unsigned family = 0; family < 2; ++family)
    for (unsigned mode = 0; mode < 8; ++mode)
    for (unsigned flags = 0; flags < 256; ++flags)
    for (unsigned length = 0; length < 2; ++length) {
        e1000_state_t state = { mode & 1, mode & 2, mode & 4 };
        e1000_rx_packet_t packet = { .protocol = SOCKET_PROTOCOL_TCP,
            .flags = flags, .payload_length = length, .sequence = 300,
            .acknowledgement = 301 };
        bool synthetic_synack = mode == 7 && length == 0 && flags == 18;
        bool expected = !synthetic_synack && !(flags & NET_TCP_FLAG_RST);
        resets = 0;
        dispatch[family](&state, packet, K_ENOENT);
        assert(resets == (unsigned)expected);
        if (expected) {
            if (flags & NET_TCP_FLAG_ACK) {
                assert(last.flags == NET_TCP_FLAG_RST && last.sequence == 301);
            } else {
                assert(last.flags == (NET_TCP_FLAG_RST | NET_TCP_FLAG_ACK));
                assert(last.acknowledgement == 300 + length +
                       !!(flags & NET_TCP_FLAG_SYN) + !!(flags & NET_TCP_FLAG_FIN));
            }
        }
        resets = 0;
        dispatch[family](&state, packet, K_OK);
        assert(resets == 0);
        ++cases;
    }
    assert(cases == 8192);
}
'''

with tempfile.TemporaryDirectory(prefix='tcp-loopback-reset-') as directory:
    path = Path(directory)
    variants = {
        'current': program,
        'old-reset': program.replace(
            '!e1000_tcp_self_test_peer_reply(state, &packet)', 'true'),
        'suppress-live': program.replace(
            '!e1000_tcp_self_test_peer_reply(state, &packet)', 'false'),
    }
    for name, text in variants.items():
        src = path / (name + '.c')
        exe = path / name
        src.write_text(text)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-function',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(src), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True)
        assert (result.returncode == 0) == (name == 'current'), name
print('TCP reset: 8192 IPv4/IPv6 loopback/live cases; old-reset and suppress-live negative controls PASS')
