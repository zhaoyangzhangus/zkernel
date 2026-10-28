"""Exercise the real post-firmware PHY reset barrier and failure ordering."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/rtl8126.c').read_text()
wait = source[source.index('static bool rtl8126_phy_wait_reset('):
              source.index('static bool rtl8126_phy_page(')]
config = source[source.index('static bool rtl8126_phy_config('):
                source.index('static bool rtl8126_disable_aspm(')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define RTL8126_PHY_OCP_BASE 0xa400U
typedef struct {bool rtl_firmware_required;uint32_t rtl_phy_ocp_base;} e1000_state_t;
static unsigned reads,elapsed,pages,autoneg,reset_reads;
static bool firmware,read_failure,all_ones,page_failure;
static bool rtl8126_load_firmware(e1000_state_t *s) {assert(s);return firmware;}
static bool rtl8126_gphy_ocp_read(const e1000_state_t *s,uint32_t r,uint16_t *v) {
 assert(s && r==RTL8126_PHY_OCP_BASE && !pages && !autoneg);++reads;
 *v=all_ones?UINT16_MAX:reads<=reset_reads?0x9000:0x1000;
 return !read_failure;
}
static void rtl8126_delay_ms(uint16_t n) {assert(n==50);elapsed+=n;}
static bool rtl8126_phy_page(e1000_state_t *s,uint16_t p,uint16_t r,uint16_t m,uint16_t v) {
 assert(s);(void)p;(void)r;(void)m;(void)v;++pages;return !page_failure;
}
static bool rtl8126_phy_start_autoneg(e1000_state_t *s) {assert(s);++autoneg;return true;}
''' + wait + config + r'''
static void fixture(void) {
 reads=elapsed=pages=autoneg=reset_reads=0;
 firmware=true;read_failure=all_ones=page_failure=false;
}
int main(void) {
 e1000_state_t s={0};fixture();assert(!rtl8126_phy_wait_reset(0) && !reads);
 for(unsigned n=0;n<=12;++n) {
  fixture();reset_reads=n;assert(rtl8126_phy_config(&s));
  assert(reads==n+1 && elapsed==n*50 && pages==6 && autoneg==1);
  assert(!s.rtl_firmware_required && s.rtl_phy_ocp_base==RTL8126_PHY_OCP_BASE);
 }
 fixture();reset_reads=13;assert(!rtl8126_phy_config(&s));
 assert(reads==13 && elapsed==600 && !pages && !autoneg);
 fixture();read_failure=true;assert(!rtl8126_phy_config(&s));
 assert(reads==1 && !elapsed && !pages && !autoneg);
 fixture();all_ones=true;assert(!rtl8126_phy_config(&s));
 assert(reads==1 && !elapsed && !pages && !autoneg);
 fixture();page_failure=true;assert(!rtl8126_phy_config(&s) && !autoneg);
 fixture();firmware=false;assert(rtl8126_phy_config(&s));
 assert(s.rtl_firmware_required && !reads && !elapsed && pages==6 && autoneg==1);
}
'''
variants = [
    ('production', program),
    ('ignored reset failure', program.replace(
        'if (firmware_loaded && !rtl8126_phy_wait_reset(state)) return false;',
        'if (firmware_loaded) (void)rtl8126_phy_wait_reset(state);')),
    ('no bounded wait', program.replace('attempt <= 12U', 'attempt < 12U')),
    ('inaccessible PHY accepted', program.replace('bmcr == UINT16_MAX', 'false')),
]
with tempfile.TemporaryDirectory(prefix='rtl-phy-reset-') as directory:
    binary = str(Path(directory) / 'test')
    for label, code in variants:
        if label != 'production':
            assert code != program
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, timeout=5)
        assert (result.returncode == 0) == (label == 'production'), (label, result.stderr)
print('RTL8126 post-firmware reset: completion ordering, 600ms bound, I/O failure and negative controls PASS')
