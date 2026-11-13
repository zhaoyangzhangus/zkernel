"""执行三条真实关闭入口，验证显示、输入、表面等待者均在解锁后收到取消通知。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/window.c').read_text()
def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define WINDOW_SERVER_MAX_WINDOWS 64
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
typedef int kstatus_t;
typedef struct {uint64_t pid;} process_t;
typedef struct {process_t *owner; unsigned refs; bool removed;
 unsigned event_waitq, surface_waitq, present_waitq;
} window_server_window_t;
static struct {struct {unsigned count;window_server_window_t *children[64];} scene;
 unsigned desktop_launch_waitq;
} g_window_server;
static bool locked;
static unsigned signals,notifications;
static bool window_server_init(void){return true;}
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static void object_get(window_server_window_t *w){assert(locked&&w->refs);++w->refs;}
static void object_put(window_server_window_t *w){assert(w->refs>1);--w->refs;}
static void remove_window_locked(window_server_window_t *w){
 assert(locked);w->removed=true;
 for(unsigned i=0;i<g_window_server.scene.count;++i)
  if(g_window_server.scene.children[i]==w){
   for(unsigned j=i+1;j<g_window_server.scene.count;++j)
    g_window_server.scene.children[j-1]=g_window_server.scene.children[j];
   --g_window_server.scene.count;break;
  }
}
static bool window_server_owner_locked(process_t *p,window_server_window_t *w){
 assert(locked);return w->owner==p&&!w->removed;
}
static bool desktop_chrome_process_released_locked(uint64_t pid){assert(locked&&pid);return false;}
static void window_server_notify_worker(void){assert(!locked);++notifications;}
static void object_notify_signaled(window_server_window_t *w){assert(!locked&&w->removed&&w->refs>1);++signals;}
static int wake_all(unsigned *q){assert(!locked);++*q;return 0;}
static int wake_one(unsigned *q){assert(!locked);++*q;return 0;}
''' + '\n'.join(function(signature) for signature in (
    'void window_server_handle_closed(', 'void window_server_close_process(',
    'kstatus_t window_server_close_owned(')) + r'''
static void check(window_server_window_t *w){
 assert(w->removed&&w->refs==1);
 assert(w->event_waitq==1&&w->surface_waitq==1&&w->present_waitq==1);
}
int main(void){
 process_t p={1},other={2};
 window_server_window_t w={.owner=&p,.refs=1};
 assert(window_server_close_owned(&other,&w)==K_EPERM);
 assert(!w.removed&&!w.event_waitq&&!w.surface_waitq&&!w.present_waitq&&w.refs==1);
 assert(window_server_close_owned(&p,&w)==K_OK);check(&w);
 w=(window_server_window_t){.owner=&p,.refs=1};
 window_server_handle_closed(&w);check(&w);
 window_server_window_t a={.owner=&p,.refs=1},b={.owner=&p,.refs=1},c={.owner=&other,.refs=1};
 g_window_server.scene.count=3;
 g_window_server.scene.children[0]=&a;g_window_server.scene.children[1]=&c;g_window_server.scene.children[2]=&b;
 window_server_close_process(&p);check(&a);check(&b);
 assert(g_window_server.scene.count==1&&g_window_server.scene.children[0]==&c);
 assert(!c.removed&&!c.present_waitq&&!c.event_waitq&&!c.surface_waitq&&c.refs==1);
 assert(signals==4&&notifications==3&&!locked);
}
'''
with tempfile.TemporaryDirectory(prefix='window-close-waiters-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True)
print('关闭等待者：句柄关闭、显式关闭、进程退出均解锁后唤醒三类等待，所有权与引用保持正确')
