"""Test actual bounded real-test record builders and UEFI address formatting."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
realtest = (ROOT / 'kernel/realtest.c').read_text()
boot = (ROOT / 'boot/uefi/main.c').read_text()
assert 'LITEOS_' not in realtest and 'LITEOS_' not in boot
path = re.search(r'g_realtest_loader_log_path\[\] = \{(.*?)\};',boot,re.S)[1]
chars = re.findall(r"'((?:\\.|[^'])*)'",path)
decoded = ''.join(bytes(char,'ascii').decode('unicode_escape') for char in chars)
assert decoded == '\\log\\realtest.log', decoded
assert "'log\\realtest.log'" in (ROOT / 'tools/collect-realtest-log.ps1').read_text()
for source in (realtest,boot):
    # Persisted firmware identity stays compatible while the log path changes.
    assert '0x4C495445U, 0x4F53U, 0x5254U' in source
    assert "'L', 'i', 't', 'e', 'O', 'S', 'R', 'e', 'a', 'l', 'T', 'e', 's', 't'" in source
helpers = realtest[realtest.index('static void append_text('):realtest.index('bool realtest_enabled(')]
builders = realtest[realtest.index('void realtest_record_failure('):realtest.index('void realtest_finish_success(')]
loader = boot[boot.index('static VOID loader_log_hex('):boot.index('static VOID loader_runtime_mark(')]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef void VOID;
typedef char CHAR8;
typedef uint64_t UINT64;
typedef size_t UINTN;
static char output[1024], state[256];
static size_t used;
static unsigned marks;
static bool g_realtest_specific_failure_state;
void realtest_mark(const char *text) { assert(strlen(text)<sizeof(state)); strcpy(state,text); ++marks; }
void realtest_capture(const char *text) { assert(strlen(text)<sizeof(output)); strcpy(output,text); }
static void loader_log_write(const char *text) {
    assert(used+strlen(text)<sizeof(output)); strcpy(output+used,text); used+=strlen(text);
}
static void reset(void) { memset(output,0,sizeof(output)); memset(state,0,sizeof(state)); used=marks=0; }
''' + helpers + builders + loader + r'''
int main(void) {
    realtest_record_failure("kernel/test.c",UINT32_MAX);
    assert(!strcmp(output,"OS_REALTEST_FAIL loc=kernel/test.c:4294967295\r\n"));
    assert(!strcmp(state,"FAILURE kernel/test.c:4294967295") && marks==1);
    reset(); g_realtest_specific_failure_state=true;
    realtest_record_failure(NULL,0);
    assert(!strcmp(output,"OS_REALTEST_FAIL\r\n") && !marks);
    reset(); realtest_record_exception(UINT64_MAX,0,UINT64_C(0x8000000000000000),UINT64_MAX,true);
    assert(!strcmp(output,"OS_REALTEST_EXCEPTION vector=0xFFFFFFFFFFFFFFFF error=0x0000000000000000 rip=0x8000000000000000 cr2=0xFFFFFFFFFFFFFFFF\r\n"));
    reset(); realtest_record_exception(0,0,0,UINT64_MAX,false);
    assert(!strstr(output,"cr2=") && strstr(output,"rip=0x0000000000000000\r\n"));
    reset(); loader_log_address("OS_LOADER_KERNEL_PA ",UINT64_MAX);
    assert(!strcmp(output,"OS_LOADER_KERNEL_PA 0xFFFFFFFFFFFFFFFF\r\n"));
    reset(); loader_log_address("OS_LOADER_STACK ",0);
    assert(!strcmp(output,"OS_LOADER_STACK 0x0000000000000000\r\n"));
    for (size_t capacity=0;capacity<=32;++capacity) {
        unsigned char guarded[40]; memset(guarded,0xa5,sizeof(guarded));
        char *buffer=(char *)guarded+4; size_t length=0;
        append_text(buffer,&length,capacity,"abcdefghijk");
        append_hex(buffer,&length,capacity,UINT64_MAX);
        append_u32(buffer,&length,capacity,UINT32_MAX);
        assert(length < capacity || !capacity);
        for (size_t i=0;i<4;++i) assert(guarded[i]==0xa5);
        for (size_t i=4+capacity;i<sizeof(guarded);++i) assert(guarded[i]==0xa5);
        if (capacity) assert(buffer[length]=='\0');
    }
    char untouched[8]; memset(untouched,0x5a,sizeof(untouched));
    size_t length=SIZE_MAX;
    append_text(untouched,&length,8,"x"); append_hex(untouched,&length,8,0); append_u32(untouched,&length,8,0);
    assert(length==SIZE_MAX);
    for (size_t i=0;i<sizeof(untouched);++i) assert(untouched[i]==0x5a);
    length=0;
    append_text(NULL,&length,8,"x"); append_text(untouched,NULL,8,"x"); append_text(untouched,&length,8,NULL);
    append_hex(NULL,&length,8,0); append_hex(untouched,NULL,8,0);
    append_u32(NULL,&length,8,0); append_u32(untouched,NULL,8,0); assert(!length);
    puts("BOOT_REALTEST_DIAGNOSTIC_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='boot-realtest-diagnostics-') as temporary:
    fixture=Path(temporary)/'test.c'; binary=Path(temporary)/'test'
    fixture.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(fixture),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
