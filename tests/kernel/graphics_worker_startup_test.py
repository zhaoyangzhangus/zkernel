"""执行真实图形线程启动入口，检查创建权、失败回滚和重入。"""
from pathlib import Path
import subprocess
import tempfile
import resource
import signal

root = Path(__file__).resolve().parents[2]


def function(path, name):
    text = (root / path).read_text()
    start = text.index(name)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


claim = function('kernel/graphics/internal.h', 'static inline bool window_worker_claim_start(')
entries = [('server', 'window_server_start_worker'),
           ('assets', 'desktop_shell_start_asset_worker'),
           ('launcher', 'desktop_shell_start_launch_worker')]
prefix = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <arch/x86_64/context.h>
static struct {
 thread_t worker,desktop_asset_worker,desktop_launch_worker;
 atomic_bool worker_started,desktop_asset_worker_started,desktop_launch_worker_started;
} g_window_server;
static atomic_bool g_window_worker_starting,g_desktop_asset_worker_starting,g_desktop_launch_worker_starting;
static uint8_t g_window_server_worker_stack[32768] __attribute__((aligned(16)));
static uint8_t g_desktop_asset_worker_stack[32768] __attribute__((aligned(16)));
static uint8_t g_desktop_launch_worker_stack[32768] __attribute__((aligned(16)));
static thread_t bootstrap;
static bool have_current=true,fail_enqueue;
static unsigned cpu,constructors,enqueues,monitors,site;
static bool (*active)(void);
static bool checking_reentry;
bool window_server_start_worker(void);
bool desktop_shell_start_asset_worker(uint32_t c);
bool desktop_shell_start_launch_worker(uint32_t c);
static bool asset(void){return desktop_shell_start_asset_worker(1);}
static bool launcher(void){return desktop_shell_start_launch_worker(1);}
static void window_server_worker_main(void *p){(void)p;}
static void desktop_asset_worker_main(void *p){(void)p;}
static void desktop_launch_worker_main(void *p){(void)p;}
void x86_kernel_thread_start(void){assert(0);}
bool window_server_init(void){return true;}
thread_t *sched_current_thread(void){return have_current?&bootstrap:NULL;}
uint32_t x86_current_cpu_index(void){return cpu;}
uint32_t x86_smp_discovered_count(void){return 4;}
bool x86_smp_cpu_online(uint32_t c){return c<4;}
void window_lock(void){}
void window_unlock(void){}
void window_mark_dirty_locked(void){}
void sched_initialize_new_thread(thread_t *t,uint32_t c,bool pin){
 assert(pin&&c<MAX_CPUS);++constructors;
 /* 初始化过程中递归调用真实入口，不得再次修改同一线程。 */
 thread_t *target=site==0?&g_window_server.worker:site==1?
     &g_window_server.desktop_asset_worker:&g_window_server.desktop_launch_worker;
 if(t==target){
  assert(!checking_reentry);checking_reentry=true;
  assert(!active());checking_reentry=false;
 }
 t->owner_cpu=c;atomic_init(&t->state,THREAD_NEW);
}
bool sched_enqueue_bootstrap(thread_t *t){
 assert(atomic_load(&t->state)==THREAD_READY);++enqueues;return !fail_enqueue;
}
void sched_monitor_register(thread_t *t,const char *name){assert(t&&name);++monitors;}
''' + claim + '\n'
actual = '\n'.join(function('kernel/graphics/' + file + '.c', 'bool ' + name + '(')
                   for file, name in entries)
main = r'''
static atomic_bool contested,already_started;
static atomic_uint winners,arrivals;
static void *claim_once(void *unused){
 (void)unused;
 if(window_worker_claim_start(&contested,&already_started))atomic_fetch_add(&winners,1);
 atomic_fetch_add(&arrivals,1);return NULL;
}
int main(void){
 bool (*calls[])(void)={window_server_start_worker,asset,launcher};
 atomic_bool *started[]={&g_window_server.worker_started,&g_window_server.desktop_asset_worker_started,
                        &g_window_server.desktop_launch_worker_started};
 atomic_bool *starting[]={&g_window_worker_starting,&g_desktop_asset_worker_starting,
                         &g_desktop_launch_worker_starting};
 for(site=0;site<3;++site){
  memset(&g_window_server,0,sizeof(g_window_server));
  atomic_store(&g_window_worker_starting,false);
  atomic_store(&g_desktop_asset_worker_starting,false);
  atomic_store(&g_desktop_launch_worker_starting,false);
  constructors=enqueues=monitors=0;active=calls[site];cpu=0;
  have_current=false;assert(!active()&&!constructors);have_current=true;
  cpu=MAX_CPUS;assert(!active()&&!constructors);cpu=0;
  fail_enqueue=true;
  assert(!active()&&constructors==1&&enqueues==1&&!monitors);
  assert(!atomic_load(started[site])&&!atomic_load(starting[site]));
  fail_enqueue=false;assert(active());
  assert(atomic_load(started[site])&&!atomic_load(starting[site]));
  unsigned before=constructors;assert(active()&&constructors==before);
 }
 for(unsigned round=0;round<64;++round){
  atomic_store(&contested,false);atomic_store(&already_started,false);
  atomic_store(&winners,0);atomic_store(&arrivals,0);
  pthread_t threads[16];
  for(unsigned i=0;i<16;++i)assert(!pthread_create(&threads[i],NULL,claim_once,NULL));
  for(unsigned i=0;i<16;++i)assert(!pthread_join(threads[i],NULL));
  assert(winners==1&&arrivals==16);
  atomic_store(&contested,false);atomic_store(&already_started,true);
  assert(!window_worker_claim_start(&contested,&already_started)&&!atomic_load(&contested));
 }
 puts("Graphics startup: three real entrypoints, invalid context, reentry, enqueue rollback/retry, idempotence; 1024 contended claims PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='graphics-worker-startup-') as directory:
    binary = directory + '/test'
    command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
               '-DOS_KERNEL_BUILD', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=prefix + actual + main, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
    for name, old, new in (
        ('asset enqueue failure ignored',
         'if (!sched_enqueue_bootstrap(worker)) {',
         'if (sched_enqueue_bootstrap(worker) && false) {'),
        ('creation guard bypassed',
         'if (!atomic_compare_exchange_strong_explicit(starting, &expected, true,',
         'if (false && !atomic_compare_exchange_strong_explicit(starting, &expected, true,'),
    ):
        # 入队故障只替换资源线程，确保负例直接覆盖原先缺失的失败处理。
        candidate = (prefix + actual + main).replace(old, new)
        if name == 'asset enqueue failure ignored':
            broken = function('kernel/graphics/assets.c', 'bool desktop_shell_start_asset_worker(').replace(old, new)
            candidate = prefix + actual.replace(function('kernel/graphics/assets.c',
                'bool desktop_shell_start_asset_worker('), broken) + main
        assert candidate != prefix + actual + main
        subprocess.run(command, input=candidate, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, timeout=60,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert result.returncode == -signal.SIGABRT
        print('Negative control rejected:', name)
