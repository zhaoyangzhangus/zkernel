"""Run production RTL8126 PCI gating and lifecycle with fault-injected hardware."""
from pathlib import Path
import subprocess
import tempfile
import re

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/rtl8126.c').read_text()


def function(name):
    start = re.search(r'^(?:static )?bool ' + name + r'\(', source, re.M).start()
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define K_OK 0
#define RTL8126_ERROR_BASE 0x812600U
#define NET_BACKEND_NONE 0
#define NET_BACKEND_RTL8126 2
#define RTL8126_REG_CHIP_CMD 0x37U
#define RTL8126_REG_CFG9346 0x50U
#define RTL8126_REG_PHY_STATUS 0x6cU
#define RTL8126_PHY_LINK 2U
typedef struct {uint16_t command;} pci_device_t;
typedef struct {
 const pci_device_t *pci;
 bool initialized,rtl_dma_quarantined;
 int backend;
 uint8_t mac[6];
 bool link_up;
 struct {bool link_up;} device;
} e1000_state_t;
static uint32_t config;
static bool read_ok,write_ok,readback_ok,reset_ok,free_ok,latch_ok;
static unsigned enable_fault;
static bool fail_next_read;
static unsigned reads,writes,freed,unmapped,step,reset_step,gate_step,free_step;
static unsigned prepare_count,fail_stage,stage;
static bool rings_ready,check_start,outputs;
static int pci_read_config32(const pci_device_t *p,unsigned off,uint32_t *v){
 assert(p && off==4); ++reads;
 if(fail_next_read){fail_next_read=false;return -1;}
 if(!read_ok || (reads%2==0 && !readback_ok))return -1;
 *v=config;return 0;
}
static int pci_write_config32(const pci_device_t *p,unsigned off,uint32_t v){
 assert(p && off==4 && !(v&0xffff0000U));++writes;
 if(!write_ok)return -1;
 if(check_start && (v&4)) assert(rings_ready);
 if((v&4) && enable_fault==1)return -1;
 if(latch_ok && !((v&4) && enable_fault==3))config=(config&0xffff0000U)|v;
 if((v&4) && enable_fault==2)fail_next_read=true;
 if(!(v&4))gate_step=++step;
 return 0;
}
static void rtl8126_clear_interrupts(e1000_state_t *s){assert(s);}
static void rtl8126_write8(e1000_state_t *s,unsigned off,unsigned v){
 assert(s && (off==0x37 || off==0x50) && v==0);
}
static bool rtl8126_reset_controller(e1000_state_t *s){
 assert(s);reset_step=++step;return reset_ok;
}
static void e1000_runtime_set_ready(bool v){assert(!v);}
static void e1000_remove_socket_outputs(void){outputs=false;}
static void rtl8126_unbind_interrupt_state(e1000_state_t *s){assert(s);}
static bool rtl8126_free_dma(e1000_state_t *s){
 assert(s && reset_step && gate_step>reset_step && !(config&4));
 ++freed;free_step=++step;return free_ok;
}
static void rtl8126_unmap_mmio(e1000_state_t *s){assert(s);++unmapped;}
static void e1000_record_error(unsigned v){assert(v);}
static void e1000_state_prepare(e1000_state_t *s,const pci_device_t *p,int b){
 memset(s,0,sizeof(*s));s->pci=p;s->backend=b;++prepare_count;
}
static bool next_stage(void){return ++stage!=fail_stage;}
static bool rtl8126_map_mmio(e1000_state_t *s){assert(s && !(config&4));return next_stage();}
static bool rtl8126_identify(e1000_state_t *s){assert(s);return next_stage();}
static bool rtl8126_hardware_initialize(e1000_state_t *s){assert(s && !(config&4));return next_stage();}
static void rtl8126_read_mac(e1000_state_t *s){s->mac[0]=2;}
static bool rtl8126_mac_valid(uint8_t m[6]){assert(m[0]==2);return next_stage();}
static bool rtl8126_phy_config(e1000_state_t *s){assert(s);return next_stage();}
static bool rtl8126_alloc_dma(e1000_state_t *s){assert(s && !(config&4));return next_stage();}
static bool rtl8126_set_busmaster(e1000_state_t *s,bool enabled);
static bool rtl8126_start_controller(e1000_state_t *s){
 assert(s && !(config&4));rings_ready=next_stage();
 return rings_ready && rtl8126_set_busmaster(s,true);
}
static void e1000_configure_link_local(e1000_state_t *s){assert(s);}
static void e1000_device_transmit(void){}
static void net_device_init(void *d,const char *n,uint8_t *m,unsigned mtu,
                           void(*cb)(void),void *s){assert(d && n && m && mtu==1500 && cb && s);}
static uint8_t rtl8126_read8(e1000_state_t *s,unsigned off){assert(s && off==0x6c);return 2;}
static void e1000_install_socket_outputs(e1000_state_t *s){assert(s->initialized);outputs=true;}
bool rtl8126_destroy_state(e1000_state_t *s);
''' + function('rtl8126_set_busmaster') + '\n' + function('rtl8126_quiesce_dma') + '\n' + function('rtl8126_initialize_state') + '\n' + function('rtl8126_destroy_state') + r'''
static void fixture(e1000_state_t *s,pci_device_t *p){
 memset(s,0,sizeof(*s));s->pci=p;s->backend=2;
 config=0xa5a50407U;
 read_ok=write_ok=readback_ok=reset_ok=free_ok=latch_ok=true;
 reads=writes=freed=unmapped=step=reset_step=gate_step=free_step=0;
 prepare_count=fail_stage=stage=0;rings_ready=false;check_start=false;outputs=false;
 enable_fault=0;fail_next_read=false;
}
int main(void){
 pci_device_t p={0};e1000_state_t s;
 fixture(&s,&p);
 assert(!rtl8126_set_busmaster(0,false));s.pci=0;
 assert(!rtl8126_set_busmaster(&s,false) && !reads);s.pci=&p;
 for(unsigned v=0;v<65536;++v){
  config=0xa5a50000U|v;assert(rtl8126_set_busmaster(&s,false));
  assert(config==(0xa5a50000U|((v|2U)&~4U)));
  assert(p.command==(uint16_t)config);
  assert(rtl8126_set_busmaster(&s,true));
  assert(config==(0xa5a50000U|v|6U));
 }
 fixture(&s,&p);read_ok=false;
 assert(!rtl8126_set_busmaster(&s,false) && !writes);
 fixture(&s,&p);config=UINT32_MAX;
 assert(!rtl8126_set_busmaster(&s,false) && !writes);
 fixture(&s,&p);write_ok=false;assert(!rtl8126_set_busmaster(&s,false));
 fixture(&s,&p);readback_ok=false;assert(!rtl8126_set_busmaster(&s,false));
 fixture(&s,&p);latch_ok=false;assert(!rtl8126_set_busmaster(&s,false));
 for(unsigned fault=0;fault<4;++fault){
  fixture(&s,&p);s.initialized=true;outputs=true;
  if(fault==1)reset_ok=false;
  if(fault==2)write_ok=false;
  if(fault==3)free_ok=false;
  assert(rtl8126_destroy_state(&s)==(fault==0));
  assert(!outputs && !s.initialized);
  if(!fault){assert(freed==1 && unmapped==1 && free_step>gate_step);}
  else {
   assert(s.rtl_dma_quarantined && !unmapped && s.backend==2);
   assert(freed==(fault==3));
   unsigned before=prepare_count;
   assert(!rtl8126_initialize_state(&s,&p) && prepare_count==before);
   reset_ok=write_ok=free_ok=true;
   assert(rtl8126_destroy_state(&s) && !s.rtl_dma_quarantined && unmapped==1);
  }
 }
 for(unsigned failure=0;failure<=7;++failure){
  fixture(&s,&p);fail_stage=failure;check_start=true;
  assert(rtl8126_initialize_state(&s,&p)==(failure==0));
  if(!failure){assert(s.initialized && outputs && rings_ready && (config&4));
   assert(rtl8126_destroy_state(&s));}
  else assert(!outputs && !s.initialized && !(config&4));
 }
 for(unsigned failure=1;failure<=3;++failure){
  fixture(&s,&p);check_start=true;enable_fault=failure;
  assert(!rtl8126_initialize_state(&s,&p));
  assert(!outputs && !s.initialized && !(config&4));
  assert(freed==1 && unmapped==1 && free_step>gate_step);
 }
 fixture(&s,&p);reset_ok=false;
 assert(!rtl8126_initialize_state(&s,&p));
 assert(s.rtl_dma_quarantined && !freed && !unmapped && !(config&4));
 assert(rtl8126_destroy_state(0));
}
'''

with tempfile.TemporaryDirectory(prefix='rtl8126-lifecycle-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
    for old, new in [
        ('if (!rtl8126_quiesce_dma(state)) return false;',
         '(void)rtl8126_quiesce_dma(state);'),
        ('if (!rtl8126_free_dma(state)) return false;',
         '(void)rtl8126_free_dma(state);'),
        ('if (enabled) command |= 4U;', 'command |= 4U; (void)enabled;'),
    ]:
        assert old in program
        (path / 'test.c').write_text(program.replace(old, new))
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True)
        assert result.returncode != 0, 'Negative control unexpectedly passed: ' + old
print('RTL8126 DMA lifecycle: PCI bits/W1C, deferred busmaster, reset/gate/free failures and quarantine PASS')

reset_program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define RTL8126_REG_CHIP_CMD 0x37U
#define RTL8126_CHIP_RESET 0x10U
typedef struct {int unused;} e1000_state_t;
static bool valid;
static unsigned polls,clears,writes,mode;
static bool rtl8126_reg_valid(e1000_state_t *s,unsigned o,unsigned n){
 assert(s && o==0x37 && n==1);return valid;
}
static void rtl8126_clear_interrupts(e1000_state_t *s){assert(s);++clears;}
static void rtl8126_write8(e1000_state_t *s,unsigned o,unsigned v){
 assert(s && o==0x37 && v==0x10);++writes;
}
static uint8_t rtl8126_read8(e1000_state_t *s,unsigned o){
 assert(s && o==0x37);++polls;
 return mode==2 ? 0xff : (mode==1 || polls<4 ? 0x10 : 0);
}
''' + function('rtl8126_reset_controller') + r'''
int main(void){
 e1000_state_t s={0};
 assert(!rtl8126_reset_controller(&s) && !writes && !clears);
 valid=true;
 assert(rtl8126_reset_controller(&s) && polls==4 && writes==1 && clears==1);
 for(mode=1;mode<=2;++mode){polls=0;
  assert(!rtl8126_reset_controller(&s) && polls==1000000);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='rtl8126-reset-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(reset_program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('RTL8126 reset: invalid register, completion, stuck reset and disconnected MMIO bounded polls PASS')
