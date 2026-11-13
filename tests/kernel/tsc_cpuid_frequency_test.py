"""执行真实 CPU 特性探测：精确 TSC 频率优先级、缺失接口及 PIT 后备。"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/arch/x86_64/cpu/cpu.c').read_text()
a=s.index('static uint64_t cpu_tsc_hypervisor_hz(')
helper=s[a:s.index('static uint8_t cpu_pit_read(',a)]
a=s.index('void x86_cpu_detect_features(')
body=s[a:s.index('uint64_t x86_read_tsc(',a)]
program=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <arch/x86_64/cpu.h>
x86_cpu_features_t x86_boot_cpu_features;
static uint32_t basic,limit,khz,base,crystal,ratio;
static bool virtualized;
static char vendor[12];
static unsigned pit_calls,freq_calls,root_calls;
static void memory_zero(void *p,size_t n){memset(p,0,n);}
static uint64_t cpu_tsc_pit_sample(void){return (5000U-pit_calls++)*1000000ULL;}
static void cpuid(uint32_t leaf,uint32_t sub,uint32_t *a,uint32_t *b,uint32_t *c,uint32_t *d){
 assert(!sub);*a=*b=*c=*d=0;
 switch(leaf){
 case 0:*a=basic;break;
 case 1:*c=virtualized?1U<<31:0;break;
 case 7:break;
 case 0x80000000U:*a=0x80000000U;break;
 case 0x15:*a=1;*b=ratio;*c=crystal;break;
 case 0x16:*a=base;break;
 case 0x40000000U:++root_calls;*a=limit;memcpy(b,vendor,4);memcpy(c,vendor+4,4);memcpy(d,vendor+8,4);break;
 case 0x40000010U:++freq_calls;*a=khz;break;
 default:assert(0);
 }
}
''' + helper + body + r'''
static void reset(const char *name){
 basic=0x16;limit=0x40000010U;khz=5250000;base=4000;crystal=ratio=0;
 virtualized=true;memcpy(vendor,name,12);pit_calls=freq_calls=root_calls=0;
}
int main(void){
 const char *vendors[]={"KVMKVMKVM\0\0\0","VMwareVMware","TCGTCGTCGTCG"};
 for(unsigned i=0;i<3;++i){reset(vendors[i]);x86_cpu_detect_features();
  assert(x86_boot_cpu_features.tsc_hz==5250000000ULL&&freq_calls==1&&!pit_calls);}
 reset(vendors[0]);crystal=25000000;ratio=120;x86_cpu_detect_features();
 assert(x86_boot_cpu_features.tsc_hz==3000000000ULL&&!freq_calls&&!root_calls);
 reset(vendors[0]);virtualized=false;x86_cpu_detect_features();
 assert(x86_boot_cpu_features.tsc_hz==4000000000ULL&&!root_calls);
 for(unsigned i=0;i<5;++i){reset(vendors[0]);
  if(i==0)limit=0x40000001U;
  if(i==1)limit=0x40000100U;
  if(i==2)memcpy(vendor,"UnknownVMM!!",12);
  if(i==3)khz=0;
  if(i==4)khz=10000001;
  x86_cpu_detect_features();assert(x86_boot_cpu_features.tsc_hz==4000000000ULL&&!pit_calls);
  assert(freq_calls==(i>=3?1U:0U));
 }
 reset(vendors[0]);basic=0xd;x86_cpu_detect_features();
 assert(x86_boot_cpu_features.tsc_hz==5250000000ULL&&!pit_calls);
 reset(vendors[0]);basic=0;x86_cpu_detect_features();
 assert(x86_boot_cpu_features.tsc_hz==4998000000ULL&&pit_calls==3&&!root_calls);
 puts("TSC CPUID: real feature detection, three vendors, Hz units, frequency priority, invalid/absent leaves and PIT fallback PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='tsc-cpuid-') as d:
 p=Path(d);(p/'test.c').write_text(program)
 subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
