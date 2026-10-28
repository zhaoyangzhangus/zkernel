"""Actual worker predicate: dirty retries sleep, input bypasses backoff."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
compositor = (root/'kernel/graphics/compositor.c').read_text()
server = (root/'kernel/graphics/server.c').read_text()
def function(source, name):
    start=source.index(name)
    opening=source.index('{',start)
    end,depth=opening+1,1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
static uint64_t now;
static atomic_uint_fast64_t g_compositor_native_retry_deadline;
static struct {bool kernel_ready;} g_window_server;
static atomic_bool g_window_dirty_hint;
static unsigned input;
static unsigned input_core_pending(void){return input;}
static uint64_t x86_read_tsc(void){return now;}
''' + function(compositor,'bool compositor_native_retry_ready(') + '\n' + \
    function(compositor,'uint64_t compositor_native_retry_wait_ns(') + '\n' + \
    function(server,'static bool window_server_worker_ready(') + r'''
int main(void){
 assert(!window_server_worker_ready(0));
 g_window_server.kernel_ready=true;
 assert(!window_server_worker_ready(0));
 atomic_store(&g_window_dirty_hint,true);
 assert(window_server_worker_ready(0));
 atomic_store(&g_compositor_native_retry_deadline,16000000);
 assert(!window_server_worker_ready(0));
 assert(compositor_native_retry_wait_ns()==16000000);
 input=1;assert(window_server_worker_ready(0));input=0;
 now=15999999;assert(!window_server_worker_ready(0));
 now++;assert(window_server_worker_ready(0));
 assert(compositor_native_retry_wait_ns()==UINT64_MAX);
 atomic_store(&g_window_dirty_hint,false);assert(!window_server_worker_ready(0));
}
'''
with tempfile.TemporaryDirectory(prefix='native-retry-') as temporary:
    binary=str(Path(temporary)/'test')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True)
print('Native retry: dirty blocked before deadline, timed wait armed, input bypass, deadline/idle PASS')
