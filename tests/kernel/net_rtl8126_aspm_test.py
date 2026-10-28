"""Exercise the production RTL8126 chip-side power-management handoff."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/rtl8126.c').read_text()
start = source.index('static bool rtl8126_disable_aspm(')
end = source.index('\nstatic bool ', start + 1)
helper = source[start:end]
hardware_start = source[source.index('static bool rtl8126_hardware_start('):]
assert hardware_start.index('rtl8126_disable_aspm(state)') < hardware_start.index('rtl8126_csi_modify(')
definitions = '\n'.join(re.findall(
    r'^#define RTL8126_REG_(?:INT_CFG|CONFIG5)\s+[^\n]+', source, re.M))
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct { unsigned unused; } e1000_state_t;
static uint8_t registers[256];
static unsigned writes, ocp_calls;
static bool ocp_ok;
static uint8_t rtl8126_read8(e1000_state_t *s, unsigned off) {
    assert(s && off < sizeof(registers)); return registers[off];
}
static void rtl8126_write8(e1000_state_t *s, unsigned off, uint8_t value) {
    assert(s && ocp_calls == 1 && ocp_ok);
    assert(off == (writes == 0 ? 0x34 : 0x56));
    registers[off] = value; ++writes;
}
static bool rtl8126_mac_ocp_modify(e1000_state_t *s, unsigned off,
                                  unsigned clear, unsigned set) {
    assert(s && off == 0xe092 && clear == 0xff && set == 0 && !writes);
    ++ocp_calls; return ocp_ok;
}
'''
checks = r'''
int main(void) {
    e1000_state_t s = {0};
    for (unsigned a = 0; a < 255; ++a) {
        for (unsigned b = 0; b < 255; ++b) {
            registers[0x34] = a; registers[0x56] = b;
            writes = ocp_calls = 0; ocp_ok = true;
            assert(rtl8126_disable_aspm(&s));
            assert(writes == 2 && ocp_calls == 1);
            assert(registers[0x34] == (a & ~8U));
            assert(registers[0x56] == (b & ~1U));
        }
    }
    for (unsigned failure = 0; failure < 3; ++failure) {
        registers[0x34] = failure == 0 ? 255 : 8;
        registers[0x56] = failure == 1 ? 255 : 3;
        writes = ocp_calls = 0; ocp_ok = false;
        assert(!rtl8126_disable_aspm(&s) && !writes);
        assert(ocp_calls == (failure == 2));
    }
}
'''
with tempfile.TemporaryDirectory(prefix='rtl8126-aspm-') as directory:
    path = Path(directory)
    program = definitions + '\n' + fixture + helper + checks
    def check(text):
        (path / 'test.c').write_text(text)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True).returncode
    assert check(program) == 0
    for old, new in [('~(1U << 3)', '~(1U << 7)'),
                     ('config5 & (uint8_t)~1U', 'config5'),
                     ('0x00FFU, 0U', '0x000FU, 0U')]:
        assert old in helper
        assert check(program.replace(old, new)) != 0
print('RTL8126 ASPM: 65025 register combinations, failures and negative controls PASS')
