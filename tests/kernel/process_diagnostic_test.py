"""Run actual exit/cancel formatters and exec progress quota/debug state."""
from pathlib import Path
import subprocess
import tempfile
import resource

ROOT = Path(__file__).resolve().parents[2]
for name in ('exec.c', 'exit.c', 'thread.c'):
    assert 'LITEOS_' not in (ROOT / 'kernel/process' / name).read_text(), name


def diagnostics(name, start, end):
    source = (ROOT / 'kernel/process' / name).read_text()
    source = source[source.index(start):source.index(end)]
    assert 'inb ' not in source and 'outb ' not in source and 'atomic_flag' not in source
    return source


raw = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#define printf test_console_printf
''' + f'#include "{ROOT / "kernel/console_printf.c"}"\n' + r'''
#undef printf
static char output[1024];
static size_t used;
static unsigned records;
static thread_t *current;
thread_t *sched_current_thread(void) { return current; }
void kernel_serial_write(const char *text) { (void)text; assert(0); }
void kernel_serial_write_serial_only(const char *text) {
    used = strlen(text); assert(used < sizeof(output)); strcpy(output,text); ++records;
}
static void reset(void) { memset(output,0,sizeof(output)); used=records=0; }
'''
raw += diagnostics('exit.c', 'static void process_exit_diag(', 'bool process_publish_thread_exit')
raw += '\n#if OS_DEBUG_SERIAL\n' + diagnostics('thread.c', 'static void thread_cancel_diag(', '\n#endif') + '\n#endif\n'
raw += r'''
int main(void) {
    process_t process={.pid=UINT64_MAX};
    thread_t target={.tid=0x123456789abcdef0,.process=&process}; current=&target;
    process_exit_diag(&process,&target,INT64_MIN,UINT32_MAX);
#if OS_DEBUG_SERIAL
    assert(records==1 && !strcmp(output,"OS_PROCESS_EXIT pid=0xffffffffffffffff tid=0x123456789abcdef0 status=0x8000000000000000 signal=0xffffffff\r\n"));
    reset(); process_exit_diag(NULL,NULL,-1,0);
    assert(records==1 && !strcmp(output,"OS_PROCESS_EXIT pid=0x0 tid=0x0 status=0xffffffffffffffff signal=0x0\r\n"));
    reset(); thread_cancel_diag(&target,K_EIO);
    assert(!strcmp(output,"OS_THREAD_WAIT_CANCEL target_pid=ffffffffffffffff target_tid=123456789abcdef0 caller_pid=ffffffffffffffff caller_tid=123456789abcdef0 status=fffffffffffffffb\r\n"));
    current=NULL; reset(); thread_cancel_diag(NULL,0);
    assert(records==1 && !strcmp(output,"OS_THREAD_WAIT_CANCEL target_pid=0 target_tid=0 caller_pid=0 caller_tid=0 status=0\r\n"));
    target.process=NULL; current=&target; reset(); thread_cancel_diag(&target,INT64_MIN);
    assert(records==1 && strstr(output,"target_pid=0") && strstr(output,"caller_pid=0") && strstr(output,"status=8000000000000000\r\n"));
#else
    assert(!records && !used);
    reset(); process_exit_diag(NULL,NULL,-1,0);
    assert(!records && !used);
#endif
    assert(!process_is_chrome_image(NULL) && !process_is_chrome_image(&process));
    strcpy(process.linux_exe,"/usr/apps/chrome/chrome"); assert(process_is_chrome_image(&process));
    strcpy(process.linux_exe,"/usr/apps/chrome/chrome-extra"); assert(!process_is_chrome_image(&process));
    puts("PROCESS_EXIT_CANCEL_DIAGNOSTIC_TEST_OK");
}
'''
source = (ROOT / 'kernel/process/exec.c').read_text()
debug = source[source.index('static atomic_uint g_exec_debug_stage'):source.index('typedef struct elf_exec_arguments')]
progress = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#define printf test_console_printf
''' + f'#include "{ROOT / "kernel/console_printf.c"}"\n' + r'''
#undef printf
static _Thread_local thread_t *current;
static char output[256];
static unsigned records;
thread_t *sched_current_thread(void) { return current; }
void kernel_serial_write(const char *text) { (void)text; assert(0); }
void kernel_serial_write_serial_only(const char *text) {
    assert(strlen(text)<sizeof(output)); strcpy(output,text); ++records;
}
''' + debug + r'''
#if OS_DEBUG_SERIAL
static atomic_uint accepted;
static void *worker(void *unused) {
    (void)unused;
    for (unsigned i=0;i<1000;++i)
        if (exec_debug_serial_allow()) atomic_fetch_add(&accepted,1);
    return NULL;
}
#endif
int main(void) {
    process_exec_debug_reset();
    assert(!process_exec_debug_stage() && process_exec_debug_status() == (uint32_t)K_EBUSY);
    atomic_store(&g_exec_debug_thread,17);
    process_exec_debug_mark(1); assert(!process_exec_debug_stage() && !records);
    thread_t thread={.tid=18}; current=&thread;
    process_exec_debug_mark(2); assert(!process_exec_debug_stage() && !records);
    thread.tid=17; exec_debug_progress(0xabc);
    assert(process_exec_debug_stage() == 0xabc);
    exec_debug_result(K_EIO); assert(process_exec_debug_status() == (uint32_t)K_EIO);
#if OS_DEBUG_SERIAL
    assert(records == 1 && !strcmp(output,"OS_EXEC_PROGRESS tid=17 stage=abc\r\n"));
    process_exec_debug_reset();
    pthread_t workers[8];
    for (unsigned i=0;i<8;++i) assert(!pthread_create(&workers[i],NULL,worker,NULL));
    for (unsigned i=0;i<8;++i) assert(!pthread_join(workers[i],NULL));
    assert(atomic_load(&accepted) == 128 && g_exec_debug_serial_logs == 128);
    for (unsigned i=0;i<2000;++i) assert(!exec_debug_serial_allow());
    assert(g_exec_debug_serial_logs == 128);
    atomic_store(&g_exec_debug_thread,17);
    exec_debug_progress(3); assert(process_exec_debug_stage() == 3 && records == 1);
    __atomic_store_n(&g_exec_debug_serial_logs,UINT32_MAX,__ATOMIC_RELAXED);
    assert(!exec_debug_serial_allow() && g_exec_debug_serial_logs == UINT32_MAX);
    process_exec_debug_reset(); assert(!g_exec_debug_serial_logs && exec_debug_serial_allow());
#else
    assert(!records);
#endif
    process_exec_debug_reset(); exec_debug_progress(4); assert(!process_exec_debug_stage());
    assert(!atomic_load(&g_exec_debug_thread) && process_exec_debug_status() == (uint32_t)K_EBUSY);
    puts("EXEC_PROGRESS_QUOTA_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='process-diagnostics-') as temporary:
    fixture = Path(temporary) / 'test.c'; binary = Path(temporary) / 'test'
    bypass = raw.replace('#if OS_DEBUG_SERIAL\n    /* 常规退出',
                         '#if 1\n    /* 常规退出', 1)
    assert bypass != raw
    for code,flags in ((raw,['-DOS_DEBUG_SERIAL=0']),(raw,['-DOS_DEBUG_SERIAL=1']),
                       (progress,['-DOS_DEBUG_SERIAL=0']),
                       (progress,['-DOS_DEBUG_SERIAL=1']),
                       (bypass,['-DOS_DEBUG_SERIAL=0'])):
        fixture.write_text(code)
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-fno-builtin',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I',str(ROOT/'include'),*flags,str(fixture),'-o',str(binary)],check=True)
        if code == bypass:
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
            assert result.returncode and '!records && !used' in result.stderr, result.stderr
            print('Negative control rejected: exit diagnostics bypass the disabled serial switch')
        else:
            subprocess.run([str(binary)],check=True)
