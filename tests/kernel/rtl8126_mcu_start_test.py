"""Compile the hardware-start body against a fake MAC OCP register bank."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/rtl8126.c').read_text()
start = source.index('static bool rtl8126_hardware_start(')
end = source.index('static bool rtl8126_map_mmio(', start)
defines = '\n'.join(re.findall(r'^#define RTL8126_\w+[^\n\\]*$', source, re.M))
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
typedef struct {unsigned rtl_xid;} e1000_state_t;
static unsigned milliseconds, pulse_start, pulse_done, reads, busy_reads;
static bool read_fail, config_fail;
/* ASPM has its own production-path regression; isolate MCU startup here. */
static bool rtl8126_disable_aspm(e1000_state_t *s) {(void)s;return true;}
static uint8_t rtl8126_read8(e1000_state_t *s,unsigned r) {(void)s;(void)r;return 0xff;}
static uint16_t rtl8126_read16(e1000_state_t *s,unsigned r) {(void)s;(void)r;return 0xffff;}
static uint32_t rtl8126_read32(e1000_state_t *s,unsigned r) {(void)s;(void)r;return 0xffffffff;}
static void rtl8126_write8(e1000_state_t *s,unsigned r,uint8_t v) {(void)s;(void)r;(void)v;}
static void rtl8126_write16(e1000_state_t *s,unsigned r,uint16_t v) {(void)s;(void)r;(void)v;}
static void rtl8126_write32(e1000_state_t *s,unsigned r,uint32_t v) {(void)s;(void)r;(void)v;}
static bool rtl8126_csi_modify(e1000_state_t *s,unsigned r,uint32_t m,uint32_t v) {
    (void)s;(void)r;(void)m;(void)v;return !config_fail;
}
static bool rtl8126_mac_ocp_modify(e1000_state_t *s,unsigned r,uint16_t m,uint16_t v) {
    (void)s;(void)m;
    if(r==0xeb54 && v==1)pulse_start=milliseconds+1;
    if(r==0xeb54 && v==0) {assert(milliseconds+1>pulse_start);++pulse_done;}
    return true;
}
static bool rtl8126_mac_ocp_write(e1000_state_t *s,unsigned r,uint16_t v) {
    (void)s;(void)r;(void)v;return true;
}
static bool rtl8126_mac_ocp_read(e1000_state_t *s,unsigned r,uint16_t *v) {
    (void)s;assert(r==0xe00e);++reads;
    if(read_fail)return false;
    *v=reads<=busy_reads ? 1U<<13 : 0;return true;
}
static void rtl8126_delay_ms(uint16_t ms) {milliseconds+=ms;}
''' + defines + '\n' + source[start:end] + r'''
int main(void) {
    e1000_state_t s={RTL8126_XID_2};
    assert(!rtl8126_hardware_start(0));
    s.rtl_xid=0;assert(!rtl8126_hardware_start(&s));
    s.rtl_xid=RTL8126_XID_2;busy_reads=3;
    assert(rtl8126_hardware_start(&s) && reads==4 && milliseconds==4 && pulse_done==1);
    reads=0;milliseconds=0;busy_reads=100;
    assert(!rtl8126_hardware_start(&s) && reads==10 && milliseconds==11);
    reads=0;read_fail=true;
    assert(!rtl8126_hardware_start(&s) && reads==1);
    read_fail=false;reads=0;busy_reads=0;config_fail=true;
    assert(!rtl8126_hardware_start(&s));
    config_fail=false;s.rtl_xid=RTL8126_XID_3;
    assert(rtl8126_hardware_start(&s));
}
'''
with tempfile.TemporaryDirectory(prefix='rtl-mcu-start-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
print('RTL8126 MCU start: timed reset pulse, bounded readiness wait, timeout/read/config errors, both revisions PASS')
