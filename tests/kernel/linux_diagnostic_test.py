"""Run actual Chrome VM failure logging and bounded stderr capture."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/syscall/linux.c').read_text()
assert not re.search(r'(?<![A-Z0-9_])LITEOS_[A-Z0-9_]+', source)
assert r'\\r\\n' not in source
assert 'case LINUX_SYS_WINDOW_CREATE:' in source
assert 'case LINUX_SYS_DISPLAY_GET_INFO:' in source
abi = (ROOT / 'include/uapi/linux_abi.h').read_text()
assert re.search(r'#define LINUX_SYS_WINDOW_CREATE\s+450U',abi)
assert re.search(r'#define LINUX_SYS_DISPLAY_GET_INFO\s+451U',abi)
assert '"/lib/ld-linux-system.so.1"' in source
assert '"/lib/ld-linux-liteos.so.1"' not in source
start = source.index('static bool linux_process_is_chrome_image(')
stop = source.index('\n#if OS_DEBUG_SERIAL', start)
functions = source[start:stop]
begin = functions.index('static void linux_chrome_diag_char(')
opening = functions.index('{',begin)
depth, end = 1, opening+1
while depth:
    depth += (functions[end] == '{') - (functions[end] == '}'); end += 1
functions = functions[:begin] + 'static void linux_chrome_diag_char(char c) { capture(c); }\n' + functions[end:]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <kernel/process.h>
static char output[20000];
static size_t used;
static uint32_t g_linux_chrome_trace_bytes, g_linux_chrome_stderr_bytes;
#define LINUX_CHROME_STDERR_LIMIT (16U*1024U)
static void capture(char c) {
    size_t slot=__atomic_fetch_add(&used,1,__ATOMIC_RELAXED);
    assert(slot < sizeof(output)-1); output[slot]=c;
}
static void reset(void) { memset(output,0,sizeof(output)); used=0; }
static void reset_budget(void) { reset(); g_linux_chrome_trace_bytes=g_linux_chrome_stderr_bytes=0; }
''' + functions + r'''
static process_t *concurrent_process;
static void *writer(void *unused) {
    (void)unused;
    for (unsigned i=0;i<1000;++i)
        linux_chrome_stderr_write(concurrent_process,"error01234567890",16);
    return NULL;
}
int main(void) {
    process_t process={.pid=UINT64_MAX};
    assert(!linux_process_is_chrome_image(NULL) && !linux_process_is_chrome_image(&process));
    linux_chrome_vm_failure("MMAP",&process,0,0,0,0,0); assert(!used);
    strcpy(process.linux_exe,"/usr/apps/chrome/chrome"); assert(linux_process_is_chrome_image(&process));
    reset(); linux_chrome_vm_failure("MPROTECT",&process,UINT64_MAX,0,7,UINT64_MAX,K_EIO);
    assert(!strcmp(output,"OS_CHROME_MPROTECT_FAIL pid=0xffffffffffffffff addr=0xffffffffffffffff len=0x0000000000000000 prot=0x0000000000000007 flags=0xffffffffffffffff status=0xfffffffffffffffb\r\n"));
    reset(); linux_chrome_vm_failure(NULL,&process,0,0,0,0,0); assert(!used);
    char text[16384]; memset(text,'x',sizeof(text));
    reset_budget(); linux_chrome_stderr_write(&process,text,SIZE_MAX);
    assert(used==16384 && g_linux_chrome_stderr_bytes==16384 && !g_linux_chrome_trace_bytes);
    for (unsigned i=0;i<1000;++i) linux_chrome_stderr_write(&process,text,SIZE_MAX);
    assert(used==16384 && g_linux_chrome_stderr_bytes==16384);
    reset_budget(); memcpy(text,"OS_TRACE",8);
    linux_chrome_stderr_write(&process,text,SIZE_MAX);
    assert(used==4096 && g_linux_chrome_trace_bytes==4096 && !g_linux_chrome_stderr_bytes);
    linux_chrome_stderr_write(&process,"LITEOS_TRACE",12); assert(used==4096);
    linux_chrome_stderr_write(&process,"error",5);
    assert(used==4101 && g_linux_chrome_stderr_bytes==5);
    reset_budget(); linux_chrome_stderr_write(&process,"LITEOS_TRACE",12);
    assert(g_linux_chrome_trace_bytes==12 && !g_linux_chrome_stderr_bytes);
    reset_budget(); linux_chrome_stderr_write(&process,"OS",2);
    assert(g_linux_chrome_stderr_bytes==2 && !g_linux_chrome_trace_bytes);
    reset_budget(); g_linux_chrome_stderr_bytes=16380;
    linux_chrome_stderr_write(&process,"0123456789",10);
    assert(used==4 && !memcmp(output,"0123",4) && g_linux_chrome_stderr_bytes==16384);
    reset_budget(); g_linux_chrome_stderr_bytes=UINT32_MAX;
    linux_chrome_stderr_write(&process,"error",5); assert(!used && g_linux_chrome_stderr_bytes==UINT32_MAX);
    reset_budget(); strcpy(process.linux_exe,"/usr/apps/chrome/chrome-extra");
    linux_chrome_stderr_write(&process,text,10); assert(!used && !g_linux_chrome_trace_bytes);
    strcpy(process.linux_exe,"/usr/apps/chrome/chrome");
    linux_chrome_stderr_write(&process,NULL,1); linux_chrome_stderr_write(&process,text,0);
    linux_chrome_stderr_write(NULL,text,1); assert(!used && !g_linux_chrome_stderr_bytes);
    reset_budget(); concurrent_process=&process;
    pthread_t writers[8];
    for (unsigned i=0;i<8;++i) assert(!pthread_create(&writers[i],NULL,writer,NULL));
    for (unsigned i=0;i<8;++i) assert(!pthread_join(writers[i],NULL));
    assert(used==16384 && g_linux_chrome_stderr_bytes==16384 && !g_linux_chrome_trace_bytes);
    puts("LINUX_DIAGNOSTIC_CAPTURE_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='linux-diagnostics-') as temporary:
    fixture=Path(temporary)/'test.c'; binary=Path(temporary)/'test'
    fixture.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I',str(ROOT/'include'),str(fixture),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
