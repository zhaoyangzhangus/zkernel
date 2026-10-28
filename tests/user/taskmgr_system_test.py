"""真实任务管理器整机采样：首帧、CPU 热插拔、计数回退、失败和溢出。"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'user/desktop/taskmgr/main.c').read_text()
def function(name):
 import re
 m=re.search(r'^static [^\n]*\b'+name+r'\([^;{}]*\)\s*\{',s,re.M);assert m
 end=s.index('{',m.start())+1;depth=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[m.start():end]
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <uapi/all.h>
#define TASKMGR_STATUS_CAP 96U
static os_cpu_usage_t g_usage_previous,next_usage;
static os_memory_info_t g_memory;
static uint32_t g_total_cpu;
static bool g_usage_baseline,g_usage_valid,g_total_cpu_valid,g_memory_valid,failed,bad_memory;
static int64_t taskmgr_syscall_one(uint64_t n,uint64_t p){
 if(failed)return -5;
 if(n==OS_SYS_MEMORY_INFO){
  os_memory_info_t *m=(void*)(uintptr_t)p;m->page_size=bad_memory?UINT64_MAX:4096;
  m->total_pages=1048576;m->free_pages=262144;return 0;
 }
 assert(n==OS_SYS_CPU_GET_USAGE);*(os_cpu_usage_t*)(uintptr_t)p=next_usage;return 0;
}
'''+''.join(function(n) for n in ['text_length','append_text','append_u64','taskmgr_cpu_percent','taskmgr_system_sample','taskmgr_summary'])+r'''
int main(void){
 next_usage=(os_cpu_usage_t){.busy_ns=100,.idle_ns=300,.cpu_count=4};
 taskmgr_system_sample();assert(g_memory_valid&&g_usage_valid&&!g_total_cpu_valid);
 char text[160];taskmgr_summary(text,sizeof(text));assert(strstr(text,"采样中")&&strstr(text,"空闲内存：1024 MiB")&&strstr(text,"已使用内存：3072 MiB"));
 next_usage.busy_ns+=100;next_usage.idle_ns+=300;taskmgr_system_sample();
 assert(g_total_cpu_valid&&g_total_cpu==25000);taskmgr_summary(text,sizeof(text));assert(strstr(text,"25.000%"));
 next_usage.cpu_count=8;taskmgr_system_sample();assert(!g_total_cpu_valid&&g_usage_valid);
 next_usage.busy_ns=0;next_usage.idle_ns=0;taskmgr_system_sample();assert(!g_total_cpu_valid);
 next_usage.idle_ns=8000;taskmgr_system_sample();assert(g_total_cpu_valid&&!g_total_cpu);
 next_usage.busy_ns=8000;taskmgr_system_sample();assert(g_total_cpu_valid&&g_total_cpu==100000);
 taskmgr_system_sample();assert(!g_total_cpu_valid);
 failed=true;taskmgr_system_sample();assert(!g_usage_valid&&!g_memory_valid&&!g_total_cpu_valid);
 taskmgr_summary(text,sizeof(text));assert(!strstr(text,"100.000%")&&strstr(text,"不可用"));
 failed=false;bad_memory=true;taskmgr_system_sample();assert(!g_memory_valid&&!g_total_cpu_valid);
 uint64_t seed=919;
 for(unsigned i=0;i<100000;++i){
  seed=seed*6364136223846793005ULL+1;
  uint64_t total=seed?seed:1,busy=seed/3;
  uint32_t expected=(uint32_t)((__uint128_t)busy*100000/total);
  assert(taskmgr_cpu_percent(busy,total)==expected);
 }
 assert(taskmgr_cpu_percent(UINT64_MAX-1,UINT64_MAX)==99999);
 assert(!taskmgr_cpu_percent(0,UINT64_MAX));
 puts("Task manager system: total CPU/available memory, independent failures, baseline/reset/online changes and 100000 overflow-safe ratios PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='taskmgr-system-') as directory:
 p=Path(directory);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',
  '-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
