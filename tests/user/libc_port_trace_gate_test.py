"""Known porting stages are quiet by default; actual errors stay visible."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/libc/syscall.c').read_text()
constants = source[source.index('static volatile uint32_t g_libc_service_trace_drop_chunks;'):
                   source.index('static bool libc_trace_budget(')]
start = source.index('static bool libc_drop_service_trace_chunk(')
function = source[start:source.index('static void libc_debug_path(', start)]
debug_start = source.index('static ssize_t debug_write(')
debug_write = source[debug_start:source.index('ssize_t write(', debug_start)]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
''' + constants + function + r'''
#define OS_SYS_DEBUG_WRITE 99U
static unsigned native_calls, native_errors;
static bool inject_error;
static int64_t __libc_native_syscall6(uint64_t number, uint64_t buffer,
                                    uint64_t size, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5) {
    assert(number==OS_SYS_DEBUG_WRITE && buffer && size && size<=240U);
    assert(!a2 && !a3 && !a4 && !a5);
    ++native_calls;
    return inject_error ? -5 : (int64_t)size;
}
static int libc_error(int64_t status) {
    assert(status==-5); ++native_errors; return -1;
}
''' + debug_write + r'''
int main(void) {
    const char *stages[]={"OS_NAVIGATE_BEGIN\n", "OS_TAB_MODEL_AFTER_HELPERS\n",
                         "OS_VIZ_BACK_TO_BACK_ADD_OBSERVER\n",
                         "OS_CC_UPDATE_AFTER_HUD\n",
                         "OS_EXTERNAL_BEGIN_FRAME_CLIENT_CREATE\n"};
    for (unsigned i=0;i<sizeof(stages)/sizeof(*stages);i++) {
#ifdef OS_RUNTIME_TRACE
        assert(!libc_drop_service_trace_chunk(stages[i],strlen(stages[i])));
#else
        assert(libc_drop_service_trace_chunk(stages[i],strlen(stages[i])));
#endif
    }
    const char *prefixes[]={"OS_NAVIGATE_",
                           "OS_TAB_MODEL_", "OS_VIZ_BACK_TO_BACK_",
                           "OS_CC_UPDATE_", "OS_EXTERNAL_BEGIN_FRAME_"};
    for (unsigned i=0;i<sizeof(stages)/sizeof(*stages);i++) {
        for (size_t n=0;n<=strlen(stages[i]);n++) {
#ifdef OS_RUNTIME_TRACE
            assert(!libc_drop_service_trace_chunk(stages[i],n));
#else
            assert(libc_drop_service_trace_chunk(stages[i],n)==(n>=strlen(prefixes[i])));
#endif
        }
    }
    const char *keep[]={"[125:126:ERROR] failure\n", "OS_LIBC_MAIN_INIT_FAIL\n",
                       "LITEOS_UNKNOWN_ERROR\n", "LITEOS_NAVIGAT", "LITEOS_NAVIGATE_BEGIN\n",
                       "LITEOS_PROFILE_SERVICE_",
                       "OS_UNKNOWN_ERROR\n", "OS_NAVIGAT", "OS_", "LITEOS_",
                       "prefix OS_NAVIGATE_BEGIN\n", "OS_PROFILE_SERVIC"};
    for (unsigned i=0;i<sizeof(keep)/sizeof(*keep);i++)
        assert(!libc_drop_service_trace_chunk(keep[i],strlen(keep[i])));
    assert(!libc_drop_service_trace_chunk(0,0));
#ifndef OS_RUNTIME_TRACE
    const char *services[]={"OS_PROFILE_SERVICE_"};
    for (unsigned n=0;n<sizeof(services)/sizeof(*services);n++) {
        for (size_t size=0;size<strlen(services[n]);size++)
            assert(!libc_drop_service_trace_chunk(services[n],size));
        assert(libc_drop_service_trace_chunk(services[n],strlen(services[n])));
        for (unsigned i=0;i<4;i++) assert(libc_drop_service_trace_chunk("chunk",5));
        assert(!libc_drop_service_trace_chunk("ordinary",8));
    }
#endif
    native_calls=0;
    for (unsigned i=0;i<sizeof(stages)/sizeof(*stages);i++)
        assert(debug_write(stages[i],strlen(stages[i]))==(ssize_t)strlen(stages[i]));
#ifdef OS_RUNTIME_TRACE
    assert(native_calls==sizeof(stages)/sizeof(*stages));
#else
    assert(native_calls==0);
#endif
    native_calls=0;
    assert(debug_write(NULL,0)==0 && native_calls==0);
    char bulk[1001]; memset(bulk,'x',sizeof(bulk));
    assert(debug_write(bulk,sizeof(bulk))==sizeof(bulk));
    assert(native_calls==5);
    native_calls=0; inject_error=true;
    assert(debug_write("[ERROR] visible",15)==-1);
    assert(native_calls==1 && native_errors==1);
}
'''
assert 'strlen(' not in function
with tempfile.TemporaryDirectory(prefix='system-port-trace-gate-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    for flags in ([], ['-DOS_RUNTIME_TRACE']):
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-Wno-unused-variable','-fsanitize=address,undefined',
                        '-fno-pie','-no-pie',*flags,str(path/'test.c'),
                        '-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True)
print('Neutral port traces: retired markers preserved, every truncation boundary, service drain, explicit tracing, '
      'actual debug writer zero kernel calls for quiet stages, chunking and error propagation passed; no per-write strlen')
