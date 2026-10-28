"""Execute the real RTL8126 stop/shared-FIFO handshake with fault injection."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/rtl8126.c').read_text()
start = source.index('static bool rtl8126_wait_fifo_empty(')
end = source.index('static uint8_t rtl8126_order_for_pages(', start)
defines = '\n'.join(re.findall(r'^#define RTL8126_\w+[^\n]*$',
                              source.replace('\\\n', ''), re.M))
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct { int unused; } e1000_state_t;
static struct { uint64_t tsc_hz; } x86_boot_cpu_features = {1000000000};
static unsigned ms,step,fault,waits,fifo_reads,error;
static void e1000_record_error(unsigned value) { if(!error)error=value; }
static uint64_t clock_ticks;
static uint8_t command,mcu;
static bool bad_misc,bad_cmd,bad_fifo,busy_fifo;
static uint64_t x86_timeout_ns_to_tsc(uint64_t ns) {return ns;}
static uint64_t x86_read_tsc(void) {return clock_ticks+=1000000;}
static void rtl8126_delay_ms(uint16_t n) {ms+=n;}
static bool next(void) {return ++step!=fault;}
''' + defines + r'''
static uint32_t rtl8126_read32(const e1000_state_t *s,unsigned r) {
 assert(s && r==RTL8126_REG_MISC);return bad_misc?UINT32_MAX:0;
}
static uint8_t rtl8126_read8(const e1000_state_t *s,unsigned r) {
 assert(s);
 if(r==RTL8126_REG_CHIP_CMD)return bad_cmd?UINT8_MAX:command;
 assert(r==RTL8126_REG_MCU);return mcu;
}
static uint16_t rtl8126_read16(const e1000_state_t *s,unsigned r) {
 assert(s && r==RTL8126_REG_FIFO_STATUS && waits==1 && (command&RTL8126_CHIP_STOP_REQ));
 ++fifo_reads;return bad_fifo?UINT16_MAX:busy_fifo?0:0x01ff;
}
static void rtl8126_write32(const e1000_state_t *s,unsigned r,uint32_t v) {
 assert(s && r==RTL8126_REG_MISC && v==RTL8126_MISC_RXDV_GATED_EN);
 assert(!step);step=1;
}
static void rtl8126_write8(const e1000_state_t *s,unsigned r,uint8_t v) {
 assert(s);
 if(r==RTL8126_REG_CHIP_CMD) {
  if(v&RTL8126_CHIP_TX_EN) {assert(ms==2 && (v&RTL8126_CHIP_STOP_REQ));}
  else {assert(fifo_reads && waits==1 && !(v&RTL8126_CHIP_RX_EN));}
  command=v;return;
 }
 assert(r==RTL8126_REG_MCU && ms>=4 && !(v&RTL8126_MCU_NOW_IS_OOB));mcu=v;
}
static bool rtl8126_wait_mcu_flags(const e1000_state_t *s,uint8_t mask,bool set) {
 assert(s && set);++waits;
 if(mask==RTL8126_MCU_RXTX_EMPTY) {
  assert(ms==3 && (command&RTL8126_CHIP_STOP_REQ));
 } else {assert(mask==RTL8126_MCU_LINK_LIST_READY && ms==4);}
 return next();
}
static bool rtl8126_mac_ocp_modify(e1000_state_t *s,unsigned r,uint16_t m,uint16_t v) {
 assert(s && ms==4 && r==0xe8de && m==(1U<<14) && !v);return next();
}
static bool rtl8126_mac_ocp_write(e1000_state_t *s,unsigned r,uint16_t v) {
 assert(s && ms==4 && waits==2);
 assert((r==0xc0aa && v==0x07d0) || (r==0xc0a6 && v==0x0150) || (r==0xc01e && v==0x5555));
 return next();
}
''' + source[start:end] + r'''
static void fixture(void) {
 ms=step=waits=fifo_reads=error=0;clock_ticks=0;fault=0;
 command=RTL8126_CHIP_RX_EN|RTL8126_CHIP_TX_EN;mcu=RTL8126_MCU_NOW_IS_OOB;
 bad_misc=bad_cmd=bad_fifo=busy_fifo=false;
}
int main(void) {
 e1000_state_t s={0};fixture();assert(!rtl8126_hardware_initialize(0) && !step);
 fixture();assert(rtl8126_hardware_initialize(&s) && step==8 && waits==3 && ms==4 && !error);
 for(unsigned fail=2;fail<=8;++fail) {
  fixture();fault=fail;assert(!rtl8126_hardware_initialize(&s) && step==fail);
  unsigned expected[]={0,0,0x21,0x26,0x27,0x28,0x28,0x28,0x29};
  assert(error==(RTL8126_ERROR_BASE|expected[fail]));
 }
 fixture();bad_misc=true;assert(!rtl8126_hardware_initialize(&s) && !step && error==(RTL8126_ERROR_BASE|0x24));
 fixture();bad_cmd=true;assert(!rtl8126_hardware_initialize(&s) && !waits && error==(RTL8126_ERROR_BASE|0x25));
 fixture();bad_fifo=true;assert(!rtl8126_hardware_initialize(&s) && fifo_reads==1 && step==2 && error==(RTL8126_ERROR_BASE|0x22));
 fixture();busy_fifo=true;assert(!rtl8126_hardware_initialize(&s) && fifo_reads<20 && step==2 && error==(RTL8126_ERROR_BASE|0x23));
}
'''
variants = [('production', program),
            ('missing StopReq', program.replace('command | RTL8126_CHIP_STOP_REQ', 'command')),
            ('missing quiet interval', program.replace('rtl8126_delay_ms(1U);', '', 1)),
            ('ignored FIFO failure', program.replace('if (!rtl8126_wait_fifo_empty(state)) return false;', '(void)rtl8126_wait_fifo_empty(state);'))]
with tempfile.TemporaryDirectory(prefix='rtl-init-handshake-') as directory:
    binary = str(Path(directory) / 'test')
    for label, code in variants:
        if label != 'production':
            assert code != program
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, timeout=5)
        assert (result.returncode == 0) == (label == 'production'), (label, result.stderr)
print('RTL8126 init handshake: StopReq/quiet interval, both FIFO checks, short-circuit failures and negative controls PASS')
