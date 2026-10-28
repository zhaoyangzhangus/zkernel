"""The production initial RTO must tolerate WAN latency and saturate safely."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/net/socket.c').read_text()
start = source.index('uint64_t socket_tcp_retransmit_deadline(')
end = source.index('void socket_tcp_output_get(', start)
header = (root / 'include/kernel/socket.h').read_text()
define = re.search(r'^#define SOCKET_TCP_RETRANSMIT_TIMEOUT_NS.*$', header, re.M).group()
program = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
static uint64_t scale = 1;
static uint64_t x86_timeout_ns_to_tsc(uint64_t ns) { return ns * scale; }
''' + define + '\n' + source[start:end] + r'''
int main(void) {
    assert(SOCKET_TCP_RETRANSMIT_TIMEOUT_NS >= 1000000000ULL);
    for (scale = 1; scale <= 10; ++scale) {
        uint64_t interval = 1000000000ULL * scale;
        assert(socket_tcp_retransmit_deadline(0) == interval);
        assert(socket_tcp_retransmit_deadline(123) == 123 + interval);
        assert(socket_tcp_retransmit_deadline(UINT64_MAX - interval) == UINT64_MAX);
        assert(socket_tcp_retransmit_deadline(UINT64_MAX - 1) == UINT64_MAX);
    }
    puts("TCP initial RTO: WAN-safe interval, TSC conversion and saturation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='tcp-rto-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
