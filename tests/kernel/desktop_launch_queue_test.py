"""Actual launch queue: FIFO model, bounded publication and concurrent draining."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/launcher.c').read_text()
start = source.index('static bool desktop_launch_worker_ready(')
code = source[start:source.index('static void __attribute__((noreturn)) desktop_launch_worker_main(', start)]
metadata_start = source.index('const char *window_shell_program_path(')
metadata = source[metadata_start:source.index('bool window_shell_title_matches(', metadata_start)]
internal = (root / 'kernel/graphics/internal.h').read_text()
enum_start = internal.rindex('enum {', 0, internal.index('DESKTOP_APP_NONE'))
app_enum = internal[enum_start:internal.index('};', enum_start) + 2]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
static bool desktop_shell_assets_available(void){return true;}
static struct {const char *path,*title;} g_desktop_programs[]={
 {"",""},{"/usr/apps/fileman/fileman","FILEMAN"},{"/usr/apps/gshell/gshell","SHELL"},
 {"/usr/apps/notepad/notepad","NOTEPAD"},{"/usr/apps/settings/settings","NETWORK"},
 {"/usr/apps/taskmgr/taskmgr","TASK MANAGER"},{"/usr/apps/chrome/chrome-start","CHROME"},
 {"/usr/apps/imageview/imageview","IMAGEVIEW"}};
#define WINDOW_DESKTOP_LAUNCH_QUEUE_CAPACITY 8U
static struct {
 atomic_uint desktop_launch_queue_count;
 atomic_bool desktop_launch_worker_started, desktop_chrome_relaunch_pending;
 atomic_uint_fast64_t desktop_chrome_launch_pid;
 unsigned desktop_launch_queue[8], desktop_launch_queue_head, desktop_launch_queue_tail;
 unsigned desktop_launch_waitq;
} g_window_server;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned depth;
static atomic_uint notifications,finished;
static void window_lock(void){assert(!depth);assert(!pthread_mutex_lock(&lock));++depth;}
static void window_unlock(void){assert(depth==1);--depth;assert(!pthread_mutex_unlock(&lock));}
static unsigned wake_one(void *queue){
 assert(!depth && queue==&g_window_server.desktop_launch_waitq);
 atomic_fetch_add(&notifications,1);return 1;
}
'''
suffix = r'''
static void *producer(void *opaque){
 unsigned app=(unsigned)(uintptr_t)opaque;
 for(unsigned i=0;i<20000;++i)while(!desktop_launch_enqueue(app))sched_yield();
 atomic_fetch_add(&finished,1);return 0;
}
int main(void){
 const char *paths[]={0,"/usr/apps/fileman/fileman","/usr/apps/gshell/gshell","/usr/apps/notepad/notepad",
  "/usr/apps/settings/settings","/usr/apps/taskmgr/taskmgr","/usr/apps/chrome/chrome-start","/usr/apps/imageview/imageview"};
 const char *titles[]={0,"FILEMAN","SHELL","NOTEPAD","NETWORK","TASK MANAGER","CHROME","IMAGEVIEW"};
 for(unsigned app=0;app<100000;++app){
  const char *path=window_shell_program_path(app),*title=window_shell_app_title(app);
  if(app>=1&&app<=7){assert(path&&title&&!strcmp(path,paths[app])&&!strcmp(title,titles[app]));}
  else assert(!path&&!title);
 }
 assert(!window_shell_program_path(UINT32_MAX) && !window_shell_app_title(UINT32_MAX));
 unsigned out=99;
 assert(!desktop_launch_enqueue(1) && !desktop_launch_worker_ready(0));
 atomic_store(&g_window_server.desktop_launch_worker_started,true);
 assert(!desktop_launch_enqueue(0) && !desktop_launch_enqueue(8));
 assert(!desktop_launch_dequeue(0) && !desktop_launch_dequeue(&out) && out==99);
 unsigned model[8],head=0,count=0,expected_wakes=0,rng=7;
 for(unsigned i=0;i<200000;++i){
  rng=rng*1664525U+1013904223U;
  if(rng&256U){
   unsigned app=1+(rng>>16)%7;
   bool accepted=desktop_launch_enqueue(app);
   assert(accepted==(count<8));
   if(accepted){if(!count)++expected_wakes;model[(head+count)%8]=app;++count;}
  }else{
   out=99;bool taken=desktop_launch_dequeue(&out);
   assert(taken==(count!=0));
   if(taken){assert(out==model[head]);head=(head+1)%8;--count;}else assert(out==99);
  }
  assert(atomic_load(&g_window_server.desktop_launch_queue_count)==count);
  assert(desktop_launch_worker_ready(0)==(count!=0));
  assert(atomic_load(&notifications)==expected_wakes);
 }
 while(count){assert(desktop_launch_dequeue(&out)&&out==model[head]);head=(head+1)%8;--count;}
 atomic_store(&g_window_server.desktop_chrome_relaunch_pending,true);
 assert(desktop_launch_worker_ready(0));
 assert(desktop_launch_dequeue(&out)&&out==DESKTOP_APP_CHROME);
 assert(!desktop_launch_worker_ready(0));
 pthread_t threads[4];unsigned received[4]={0};
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,producer,(void*)(uintptr_t)(i+1)));
 for(;;){
  if(desktop_launch_dequeue(&out)){assert(out>=1&&out<=4);++received[out-1];}
  else if(atomic_load(&finished)==4 && !desktop_launch_worker_ready(0))break;
  else sched_yield();
 }
 for(unsigned i=0;i<4;++i){assert(!pthread_join(threads[i],0));assert(received[i]==20000);}
 assert(!depth && !atomic_load(&g_window_server.desktop_launch_queue_count));
 puts("Launch queue: 200000 exact FIFO/full/wrap/readiness/notification cases, relaunch and 80000 concurrent submissions PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='desktop-launch-queue-') as directory:
    binary = str(Path(directory) / 'test')
    def compile_code(actual):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-x', 'c', '-', '-o', binary],
                       input=prefix + app_enum + metadata + actual + suffix, text=True, check=True)
    compile_code(code)
    subprocess.run([binary], check=True, timeout=30)
    old = 'if (count == 0U) (void)wake_one('
    assert old in code
    for replacement in ('(void)wake_one(', 'if (false) (void)wake_one('):
        compile_code(code.replace(old, replacement))
        failed = subprocess.run([binary], capture_output=True, text=True, timeout=30)
        assert failed.returncode != 0 and 'Assertion' in failed.stderr
    print('Negative controls rejected: redundant append notifications and missing empty-queue wake')
