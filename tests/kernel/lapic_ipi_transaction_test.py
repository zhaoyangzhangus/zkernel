"""执行真实 IPI 事务，覆盖空闲快路径、忙等待、超时和中断状态恢复。"""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'kernel/arch/x86_64/irq/apic.c').read_text()
start = s.index('static BOOLEAN lapic_send_ipi(')
body = s[start:s.index('\nBOOLEAN lapic_send_init(',start)]
body = body.replace('__asm__ volatile ("pushfq; popq %0; cli"\n                          : "=r"(interrupt_flags) : : "memory");',
                    'interrupt_flags = save_irq();')
body = body.replace('__asm__ volatile ("sti" : : : "memory");','enable_irq();')
assert '__asm__' not in body
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t UINT32;typedef uint64_t UINT64;typedef bool BOOLEAN;
#define LAPIC_ICR_LOW_REGISTER 0x300
#define LAPIC_ICR_HIGH_REGISTER 0x310
#define LAPIC_ICR_DELIVERY_PENDING (1U<<12)
static bool g_initialized,irq;
static unsigned reads,waits,writes,busy;
static unsigned expected_waits,fail_wait;
static uint32_t target,command;
static uint64_t save_irq(void){uint64_t f=irq?512:0;irq=false;return f;}
static void enable_irq(void){irq=true;}
static UINT32 lapic_read(UINT32 offset){
 assert(!irq&&offset==LAPIC_ICR_LOW_REGISTER);++reads;
 if(busy){--busy;return LAPIC_ICR_DELIVERY_PENDING;}return 0;
}
static BOOLEAN lapic_wait_icr_idle(void){
 assert(irq==!!expected_waits);++waits;
 if(fail_wait==waits)return false;
 return true;
}
static void lapic_write(UINT32 offset,UINT32 value){
 assert(!irq);
 if(writes++==0){assert(offset==LAPIC_ICR_HIGH_REGISTER);target=value;}
 else {assert(writes==2&&offset==LAPIC_ICR_LOW_REGISTER);command=value;}
}
''' + body + r'''
int main(void){
 for(unsigned enabled=0;enabled<2;++enabled)
 for(unsigned busy_count=0;busy_count<4;++busy_count)
 for(unsigned failure=0;failure<=busy_count+1;++failure){
  g_initialized=true;irq=enabled;reads=waits=writes=0;busy=busy_count;
  target=command=0;expected_waits=enabled;fail_wait=failure;
  assert(lapic_send_ipi(17,0x51)==!failure);
  assert(irq==!!enabled);
  if(failure&&failure<=busy_count){
   assert(reads==failure&&waits==failure&&!writes);
  }else{
   assert(reads==busy_count+1&&waits==busy_count+1&&writes==2);
   assert(target==(17U<<24)&&command==0x51);
  }
 }
 // 未初始化或目标超范围时，不触碰中断状态和设备寄存器。
 reads=waits=writes=0;irq=true;g_initialized=false;
 assert(!lapic_send_ipi(1,0x51));g_initialized=true;
 assert(!lapic_send_ipi(256,0x51));
 assert(irq&&!reads&&!waits&&!writes);
}
'''
with tempfile.TemporaryDirectory(prefix='lapic-ipi-') as d:
    binary=str(Path(d)/'test')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=program,text=True,check=True)
    subprocess.run([binary],check=True)
print('IPI 事务：空闲单次检查、忙重试、等待超时、目标/命令原子写入及原中断状态恢复通过')
