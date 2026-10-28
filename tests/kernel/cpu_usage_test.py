"""真实调度计时及整机汇总：忙闲切换、退出后保留、离线 CPU、回拨和饱和。"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/sched/clock.c').read_text()
a=source.index('static uint64_t scheduler_fair_vruntime_delta');b=source.index('void sched_tick(')
query=source[source.index('kstatus_t sched_usage_query('):]
code='#include "'+str(root/'kernel/sched/internal.h')+'"\n'+r'''
#include <uapi/cpu_usage.h>
#include <uapi/syscall.h>
#include <assert.h>
#include <stdio.h>
scheduler_cpu_t g_cpus[MAX_CPUS];uint32_t g_cpu_count=4;
static unsigned online=15;
bool x86_smp_cpu_online(uint32_t index){return index<4&&(online&(1U<<index));}
void scheduler_update_min_vruntime(run_queue_t *queue){(void)queue;}
'''+source[a:b]+query+r'''
int main(void){
 thread_t busy={.sched_class=SCHED_CLASS_FAIR},idle={0};busy.sched.weight=1024;
 for(unsigned i=0;i<4;++i){
  run_queue_t *q=&g_cpus[i].queue;q->idle=&idle;q->current=&busy;
  scheduler_account_current(q,100);
  scheduler_account_current(q,1100);
  q->current=&idle;scheduler_account_current(q,4100);
  scheduler_account_current(q,3000);
  assert(q->usage_busy_ns==1000&&q->usage_idle_ns==3000);
 }
 os_cpu_usage_t info;assert(sched_usage_query(&info)==K_OK);
 assert(info.busy_ns==4000&&info.idle_ns==12000&&info.cpu_count==4);
 assert(info.hdr.size==sizeof(info)&&info.hdr.version==OS_SYSCALL_ABI_VERSION&&!info.reserved);
 /* 累计数不持有线程引用，线程退出不会使整机占用回退。 */
 for(unsigned i=0;i<4;++i)g_cpus[i].queue.current=0;
 assert(sched_usage_query(&info)==K_OK&&info.busy_ns==4000);
 online=5;assert(sched_usage_query(&info)==K_OK&&info.cpu_count==2&&info.busy_ns==2000&&info.idle_ns==6000);
 online=0;assert(sched_usage_query(&info)==K_EAGAIN&&!info.cpu_count);
 assert(sched_usage_query(0)==K_EINVAL);
 online=15;g_cpus[0].queue.usage_busy_ns=UINT64_MAX;
 assert(sched_usage_query(&info)==K_OK&&info.busy_ns==UINT64_MAX);
 puts("CPU usage: per-CPU busy/idle, lifetime independence, online filtering, clock floor, overflow and invalid output PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='cpu-usage-') as directory:
 p=Path(directory);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',
  '-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
