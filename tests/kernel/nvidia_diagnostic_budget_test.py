"""Run actual callback diagnostics, including concurrent hot-path calls."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/gpu/nvidia/vendor_abi/nvabi_os.c').read_text()
def function(name):
    start=source.index(name+'(')
    start=source.rfind('\n',0,start)+1
    end=source.index('\n}',start)+2
    return source[start:end]
code=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
typedef uint64_t NvU64;
typedef uint32_t NvU32;
static int nvabi_string_compare(const char *a,const char *b) {int n=strcmp(a,b);return n<0 ? -1 : n>0;}
static atomic_uint_fast64_t g_nvabi_post_event_unsupported_count=ATOMIC_VAR_INIT(0);
static atomic_uint reports=ATOMIC_VAR_INIT(0);
static atomic_uint chunks=ATOMIC_VAR_INIT(0);
static _Thread_local char last_chunk[32];
static void kernel_serial_write(const char *text) {
    atomic_fetch_add(&chunks,1);
    if(!strcmp(text,"OS_NVIDIA_ABI_UNSUPPORTED "))atomic_fetch_add(&reports,1);
    if(*text>='0'&&*text<='9') {
        assert(strlen(text)<sizeof(last_chunk));strcpy(last_chunk,text);
    }
}
'''
code+=function('nvabi_append_char')+'\n'+function('nvabi_append_unsigned')+'\n'
code+=function('nvabi_log_unsupported')+r'''
static void *worker(void *unused) {
    (void)unused;
    for(unsigned i=0;i<131072;++i)nvabi_log_unsupported("nv_post_event","vendor-abi/nv-unsupported");
    return NULL;
}
int main(void) {
    /* 检查初始报告和汇总点的精确数量，同时保留其他诊断信息。 */
    for(unsigned i=0;i<33;++i)nvabi_log_unsupported("nv_post_event","test");
    assert(atomic_load(&reports)==10 && !strcmp(last_chunk,"32"));
    nvabi_log_unsupported("different_callback","test");
    nvabi_log_unsupported(NULL,NULL);
    assert(atomic_load(&reports)==12);
    char decimal[24];
    size_t n=nvabi_append_unsigned(decimal,sizeof(decimal),0,UINT64_MAX,10,false);
    decimal[n]=0;assert(!strcmp(decimal,"18446744073709551615"));
    atomic_store(&g_nvabi_post_event_unsupported_count,0);
    atomic_store(&reports,0);atomic_store(&chunks,0);
    pthread_t threads[8];
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,worker,NULL));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
    assert(atomic_load(&g_nvabi_post_event_unsupported_count)==1048576);
    assert(atomic_load(&reports)==25 && atomic_load(&chunks)==175);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nvidia-diagnostic-budget-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True,timeout=15)
print('NVIDIA callback diagnostic budget: initial reports, cumulative summaries, unrelated errors, uint64 formatting and 1048576 concurrent calls passed')
