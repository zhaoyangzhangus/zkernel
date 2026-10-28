"""Actual network teardown and shutdown entry, without touching host PCI hardware."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/core.c').read_text()


def function(name):
    start = re.search(r'^(?:static )?(?:bool|kstatus_t) ' + name + r'\(', source, re.M).start()
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#define K_OK 0
#define K_EBUSY -16
#define K_EIO -5
#define NET_BACKEND_RTL8126 2
#define E1000_REG_CTRL 0U
#define E1000_REG_IMC 0xd8U
#define E1000_REG_TCTL 0x400U
#define E1000_REG_RCTL 0x100U
#define E1000_CTRL_RST (1U<<26)
#define E1000_MII_BMCR 0U
typedef int kstatus_t;
typedef struct {uint16_t command;} pci_device_t;
typedef struct {
 const pci_device_t *pci;bool initialized,phy_loopback;
 unsigned backend;int recovery;uint16_t phy_control;
} e1000_state_t;
static e1000_state_t g_e1000;
static bool g_e1000_runtime_ready,lock_busy,locked;
static atomic_bool g_e1000_poll_queued,g_e1000_poll_retry;
static bool read_ok,write_ok,latch_ok,free_ok,rtl_ok;
static unsigned reads,writes,mode,polls,freed,unmaps,rtl_calls;
static uint32_t config;
static bool outputs;
static bool e1000_lifecycle_try_lock(void){if(lock_busy)return false;assert(!locked);locked=true;return true;}
static void e1000_lifecycle_unlock(void){assert(locked);locked=false;}
static void e1000_write(e1000_state_t *s,unsigned r,uint32_t v){
 assert(locked && s);
 assert((r==E1000_REG_IMC && v==UINT32_MAX) ||
        (r==E1000_REG_TCTL && !v) || (r==E1000_REG_RCTL && !v) ||
        (r==E1000_REG_CTRL && v==E1000_CTRL_RST));
}
static uint32_t e1000_read(e1000_state_t *s,unsigned r){
 assert(s && r==E1000_REG_CTRL);++polls;
 if(mode==2)return UINT32_MAX;
 return mode==1 || polls<3 ? E1000_CTRL_RST : 0;
}
static int pci_read_config32(const pci_device_t *p,unsigned o,uint32_t *v){
 assert(p && o==4);++reads;if(!read_ok)return -1;*v=config;return 0;
}
static int pci_write_config32(const pci_device_t *p,unsigned o,uint32_t v){
 assert(p && o==4 && !(v&0xffff0000U));++writes;
 assert(v==((config&0xffffU)&~4U));
 if(!write_ok)return -1;
 if(latch_ok)config=(config&0xffff0000U)|v;
 return 0;
}
static bool rtl8126_destroy_state(e1000_state_t *s){
 assert(locked && s);++rtl_calls;if(rtl_ok)s->initialized=false;return rtl_ok;
}
static void e1000_remove_socket_outputs(void){outputs=false;}
static void e1000_recovery_unbind(void *p){assert(p && locked && !g_e1000_runtime_ready);}
static bool e1000_phy_write(e1000_state_t *s,unsigned r,unsigned v){assert(s && !r && !v);return true;}
static bool e1000_release_dma(e1000_state_t *s){
 assert(s && !(config&4) && !g_e1000_runtime_ready && mode==0);++freed;return free_ok;
}
static void e1000_unmap_mmio(e1000_state_t *s){assert(s);++unmaps;}
''' + function('e1000_stop_controller') + '\n' + function('e1000_destroy') + '\n' + function('e1000_shutdown') + r'''
static pci_device_t device;
static void fixture(void){
 memset(&g_e1000,0,sizeof(g_e1000));g_e1000.pci=&device;g_e1000.initialized=true;
 g_e1000_runtime_ready=true;g_e1000.backend=1;
 atomic_store(&g_e1000_poll_queued,true);atomic_store(&g_e1000_poll_retry,true);
 lock_busy=locked=false;read_ok=write_ok=latch_ok=free_ok=rtl_ok=true;
 reads=writes=mode=polls=freed=unmaps=rtl_calls=0;config=0xa5a50407U;outputs=true;
}
int main(void){
 fixture();lock_busy=true;
 assert(e1000_shutdown()==K_EBUSY && !polls && g_e1000_runtime_ready);
 fixture();g_e1000.initialized=false;
 assert(e1000_shutdown()==K_OK && !polls && !locked && !g_e1000_runtime_ready);
 for(unsigned v=0;v<65536;++v){
  fixture();config=0xa5a50000U|v;
  assert(e1000_shutdown()==K_OK && !locked && !g_e1000.initialized);
  assert(config==(0xa5a50000U|(v&~4U)) && device.command==(v&~4U));
  assert(freed==1 && unmaps==1 && polls==3 && !outputs);
  assert(!atomic_load(&g_e1000_poll_queued) && !atomic_load(&g_e1000_poll_retry));
  assert(e1000_shutdown()==K_OK && freed==1 && unmaps==1);
 }
 for(unsigned fault=0;fault<6;++fault){
  fixture();
  if(fault==0)mode=1;
  if(fault==1)mode=2;
  if(fault==2)read_ok=false;
  if(fault==3)write_ok=false;
  if(fault==4)latch_ok=false;
  if(fault==5)free_ok=false;
  assert(e1000_shutdown()==K_EIO && !locked && !unmaps && !g_e1000_runtime_ready);
  assert(g_e1000.initialized && !outputs && freed==(fault==5));
  if(fault==0)assert(polls==100000 && !(config&4));
  if(fault==1)assert(polls==1 && !(config&4));
  mode=0;polls=0;read_ok=write_ok=latch_ok=free_ok=true;
  assert(e1000_shutdown()==K_OK && unmaps==1);
 }
 fixture();config=UINT32_MAX;
 assert(e1000_shutdown()==K_EIO && !writes && !freed);
 for(unsigned init=0;init<2;++init){
  fixture();g_e1000.initialized=init;g_e1000.backend=NET_BACKEND_RTL8126;rtl_ok=false;
  assert(e1000_shutdown()==K_EIO && rtl_calls==1 && !polls && !locked);
  rtl_ok=true;assert(e1000_shutdown()==K_OK && rtl_calls==2);
 }
}
'''

with tempfile.TemporaryDirectory(prefix='net-shutdown-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
    for old, new in [
        ('if (!e1000_stop_controller(state)) return false;',
         '(void)e1000_stop_controller(state);'),
        ('if (!e1000_release_dma(state)) return false;',
         '(void)e1000_release_dma(state);'),
        ('if (!e1000_lifecycle_try_lock()) return K_EBUSY;',
         '(void)e1000_lifecycle_try_lock();'),
    ]:
        assert old in program
        (path / 'test.c').write_text(program.replace(old, new))
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True)
        assert result.returncode != 0, 'Negative control passed: ' + old
print('Network shutdown: lifecycle contention, idempotence, RTL quarantine dispatch, bounded Intel reset, PCI gate/W1C, retained resources and retries PASS')
