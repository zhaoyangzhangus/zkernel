"""Exercise the actual UDP trace branch through the production formatter."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
for path in (ROOT / 'kernel/drivers/net').glob('*.c'):
    assert 'LITEOS_' not in path.read_text(), path
source = (ROOT / 'kernel/drivers/net/runtime.c').read_text()
start = source.index('if (g_debug_udp_dispatch_logs < 128U)')
opening = source.index('{', start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
branch = source[start:end]
inject_source = (ROOT / 'kernel/net/socket_protocol.c').read_text()
assert 'LITEOS_UDP_INJECT' not in inject_source
assert r'\\r\\n' not in inject_source
assert r'\\r\\n' not in (ROOT / 'kernel/syscall/network.c').read_text()
inject_branches = []
position = 0
for _ in range(2):
    start = inject_source.index('if (g_debug_udp_inject_logs < 128U)', position)
    opening = inject_source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (inject_source[end] == '{') - (inject_source[end] == '}')
        end += 1
    inject_branches.append(inject_source[start:end]); position = end
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define printf test_console_printf
''' + f'#include "{ROOT / "kernel/console_printf.c"}"\n' + r'''
#undef printf
static char output[256];
static unsigned serial_calls;
void kernel_serial_write(const char *text) { (void)text; assert(0); }
void kernel_serial_write_serial_only(const char *text) {
    assert(strlen(text) < sizeof(output)); strcpy(output,text); ++serial_calls;
}
static uint32_t g_debug_udp_dispatch_logs;
typedef struct { uint32_t source_address, destination_address; uint16_t source_port, destination_port; } diagnostic_packet_t;
static diagnostic_packet_t packet_storage;
static kstatus_t status;
static uint32_t g_debug_udp_inject_logs, destination_address, count;
static uint16_t destination_port;
static void dispatch(void) {
    diagnostic_packet_t *packet = &packet_storage;
''' + branch + r'''
}
static void inject_missing(void) {
''' + inject_branches[0] + r'''
}
static void inject_received(void) {
''' + inject_branches[1] + r'''
}
int main(void) {
    packet_storage.source_address = 0x0a000202; packet_storage.source_port = 12345;
    packet_storage.destination_address = 0x0a00020f; packet_storage.destination_port = 53;
    status = K_EIO; dispatch();
    assert(!strcmp(output,"OS_UDP_DISPATCH src=a000202:12345 dst=a00020f:53 status=-5\r\n"));
    status = K_OK;
    for (unsigned i = 1; i < 128; ++i) dispatch();
    assert(serial_calls == 128 && g_debug_udp_dispatch_logs == 128);
    assert(!strcmp(output,"OS_UDP_DISPATCH src=a000202:12345 dst=a00020f:53 status=0\r\n"));
    for (unsigned i = 0; i < 1000; ++i) dispatch();
    assert(serial_calls == 128 && g_debug_udp_dispatch_logs == 128);
    g_debug_udp_dispatch_logs = 0;
    packet_storage.source_address = packet_storage.destination_address = UINT32_MAX;
    packet_storage.source_port = packet_storage.destination_port = UINT16_MAX;
    status = K_ENOENT; dispatch();
    assert(!strcmp(output,"OS_UDP_DISPATCH src=ffffffff:65535 dst=ffffffff:65535 status=-2\r\n"));
    destination_address = 0x0a00020f; destination_port = 53; count = 7;
    inject_missing();
    assert(!strcmp(output,"OS_UDP_INJECT dst=a00020f:53 status=-2\r\n"));
    inject_received();
    assert(!strcmp(output,"OS_UDP_INJECT dst=a00020f:53 status=0 count=8\r\n"));
    assert(g_debug_udp_inject_logs == 2 && serial_calls == 131);
    g_debug_udp_inject_logs = 128;
    inject_missing(); inject_received();
    assert(g_debug_udp_inject_logs == 128 && serial_calls == 131);
    puts("NETWORK_DIAGNOSTIC_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='network-diagnostics-') as tmp:
    fixture = Path(tmp) / 'test.c'; binary = Path(tmp) / 'test'
    fixture.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(ROOT / 'include'), str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
