"""Run production down/timer transitions with lost link notifications."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/services/networkd/netd.c').read_text()
def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#define NETD_DHCP_RETRY_NS 2000000000ULL
#define OS_NET_STATUS_LINK_UP 2U
enum {NETD_STATE_LINK_DOWN,NETD_STATE_BOUND};
typedef struct {unsigned flags,ipv4_address,ipv4_gateway,ipv4_prefix_length;}os_net_status_t;
typedef struct {os_net_status_t status;unsigned socket,timer,state;}netd_context_t;
static os_net_status_t hardware;
static unsigned armed,discover,bound,cleared;
static bool timer_ok=true;
static void close_handle(unsigned *h){*h=0;}
static int netd_set_ipv4(unsigned a,unsigned p,unsigned g){assert(!a&&!p&&!g);++cleared;return 0;}
static bool arm_timer(netd_context_t *c,uint64_t ns){assert(c&&ns==NETD_DHCP_RETRY_NS);++armed;c->timer=9;return timer_ok;}
static int netd_get_status(os_net_status_t *s){*s=hardware;return 0;}
static bool start_dhcp(netd_context_t *c){assert(c);++discover;return true;}
static bool enter_bound(netd_context_t *c){assert(c);++bound;return true;}
''' + function('static bool enter_link_down(') + '\n' + function('static bool process_timer(') + r'''
int main(void){
 netd_context_t c={.socket=4,.timer=8,.status={.ipv4_address=1}};
 assert(enter_link_down(&c));
 assert(armed==1 && cleared==1 && !c.socket && c.timer && !c.status.ipv4_address);
 /* 连续错过十次通知后，周期性重试仍不得丢失。 */
 for(unsigned i=0;i<10;++i)assert(process_timer(&c));
 assert(armed==11 && !discover && !bound);
 hardware.flags=OS_NET_STATUS_LINK_UP;
 assert(process_timer(&c) && discover==1 && !bound);
 hardware.ipv4_address=0xc0a80102;hardware.ipv4_prefix_length=24;
 assert(process_timer(&c) && bound==1 && discover==1);
 timer_ok=false;hardware=(os_net_status_t){0};
 assert(!process_timer(&c));
 assert(!enter_link_down(0) && !process_timer(0));
}
'''
with tempfile.TemporaryDirectory(prefix='netd-cold-link-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Netd cold link: 10 lost notifications, timer rearm, late-up DHCP, bound and failure paths PASS')
