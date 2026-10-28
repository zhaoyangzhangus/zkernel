"""Actual ARP dispatch must never synthesize a future TCP polling timestamp."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/net/runtime.c').read_text()
start = source.index('            bool arp_updated = net_arp_cache_update(')
end = source.index('    } else if (length != 0U &&', start)
body = source[start:end]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define OS_DEBUG_SERIAL 0
#define K_OK 0
typedef struct {unsigned arp_cache;} e1000_state_t;
typedef struct {uint32_t sender_address,target_address;uint8_t sender_mac[6];} arp_view_t;
static uint64_t now,last_poll;static unsigned polls,cache_updates,replies;
static bool update_ok,reply_ok;
static uint64_t x86_read_tsc(void){return now;}
static uint64_t telemetry_timestamp(void){return 33;}
static int net_arp_cache_update(unsigned *cache,uint32_t address,const uint8_t *mac,uint64_t ts){
 assert(*cache==7 && address==123 && mac[0]==42 && ts==33);++cache_updates;
 return update_ok?K_OK:-5;
}
static bool e1000_arp_reply(e1000_state_t *state,arp_view_t *view){
 assert(state->arp_cache==7 && view->sender_address==123);++replies;return reply_ok;
}
static void socket_tcp_poll(uint64_t tick){
 assert(tick==now);last_poll=tick;++polls;
}
static bool dispatch(e1000_state_t *state,arp_view_t arp_view){
 bool received=false;
''' + body + r'''
 return received;
}
int main(void){
 const uint64_t ticks[]={0,1,1000000000ULL,UINT64_MAX-1,UINT64_MAX};
 e1000_state_t state={7};arp_view_t view={.sender_address=123,.sender_mac={42}};
 for(unsigned t=0;t<5;++t)for(unsigned variant=0;variant<4;++variant){
  now=ticks[t];update_ok=variant&1;reply_ok=variant&2;
  for(unsigned packet=0;packet<256;++packet){
   assert(dispatch(&state,view)==(update_ok || !reply_ok));
   assert(last_poll==now);
  }
 }
 assert(polls==5120 && cache_updates==polls && replies==polls);
 puts("ARP dispatch: 5120 cache/reply/clock-boundary cases preserve real TCP time PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='tcp-arp-clock-') as directory:
    path = Path(directory)
    for negative in (False, True):
        candidate = program
        if negative:
            candidate = candidate.replace('socket_tcp_poll(x86_read_tsc());',
                'socket_tcp_poll(x86_read_tsc()+1000000001ULL);')
            assert candidate != program
        (path/'test.c').write_text(candidate)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        result = subprocess.run([str(path/'test')],capture_output=True,text=True)
        if negative:
            assert result.returncode != 0, 'Synthetic future time must fail'
            print('Negative control rejected: ARP advances unrelated TCP clocks')
        else:
            assert result.returncode == 0,result.stderr
            print(result.stdout,end='')
