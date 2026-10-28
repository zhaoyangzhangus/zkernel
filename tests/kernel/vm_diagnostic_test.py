"""Run production fault diagnostics with serial, task and page-stat fixtures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/mm/fault.c').read_text()
for name in ('fault.c', 'object.c', 'vm_space.c'):
    assert 'LITEOS_' not in (ROOT / 'kernel/mm' / name).read_text(), name


def end_function(text, start):
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return end


diagnostics = source[source.index('static atomic_flag g_vm_fault_diag_lock'):source.index('enum vm_fault_counter')]
start = diagnostics.index('static void vm_fault_diag_write_char(')
end = end_function(diagnostics, start)
diagnostics = diagnostics[:start] + 'static void vm_fault_diag_write_char(char c) { capture(c); }\n' + diagnostics[end:]
start = source.index('static void vm_fault_write_diagnostic(')
diagnostics += source[start:end_function(source, start)]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/process.h>
#include <kernel/sched.h>
''' + f'#include "{ROOT / "kernel/mm/internal.h"}"\n' + r'''
#define printf test_console_printf
''' + f'#include "{ROOT / "kernel/console_printf.c"}"\n' + r'''
#undef printf
int kernel_serial_printf_serial_only(const char *, ...) __attribute__((format(printf, 1, 2)));
static char output[1024];
static size_t used;
static thread_t *current;
static kstatus_t stats_status = K_OK;
static unsigned stats_calls;
kstatus_t page_memory_get_stats(uint64_t *total, uint64_t *free) {
    assert(total && free && !used);
    ++stats_calls;
    if (stats_status != K_OK) return stats_status;
    *total = UINT64_MAX; *free = 0x123456789abcdef0ULL;
    return K_OK;
}
thread_t *sched_current_thread(void) { return current; }
static void capture(char c) { assert(used + 1 < sizeof(output)); output[used++] = c; }
static void reset(void) { memset(output,0,sizeof(output)); used=0; }
void kernel_serial_write(const char *text) { (void)text; assert(0); }
void kernel_serial_write_serial_only(const char *text) {
    assert(strlen(text) < sizeof(output)); strcpy(output,text);
}
''' + diagnostics + r'''
int main(void) {
    process_t process = {.pid=UINT64_MAX};
    thread_t thread = {.process=&process}; current=&thread;
    vm_fault_info_t fault = {.address=UINT64_MAX,.access=UINT32_MAX,.cpu_error=UINT32_MAX};
    vm_area_t area = {.start=0x123456789abcdef0,.end=UINT64_MAX,.prot=7,.flags=VM_AREA_COW};
    vm_fault_diag_emit(&thread,&fault,UINT32_MAX,K_EIO,&area,UINT64_MAX,3);
    assert(!strcmp(output,"OS_VM_USER_FAULT pid=0xffffffffffffffff address=0xffffffffffffffff access=0x00000000ffffffff error=0x00000000ffffffff status=0xfffffffffffffffb area=0x123456789abcdef0-0xffffffffffffffff prot=0x0000000000000007 object=0x0000000000000003 file_offset=0xffffffffffffffff memory_status=0x0000000000000000 total_pages=0xffffffffffffffff free_pages=0x123456789abcdef0\r\n"));
    reset(); vm_fault_diag_emit(&thread,&fault,0,INT64_MIN,NULL,0,0);
    assert(strstr(output,"status=0x8000000000000000 area=0x0000000000000000-0x0000000000000000 prot=0x0000000000000000"));
    stats_status = K_EIO;
    reset(); vm_fault_diag_emit(&thread,&fault,0,K_EIO,NULL,0,0);
    assert(strstr(output," memory_status=0xfffffffffffffffb total_pages=0x0000000000000000 free_pages=0x0000000000000000\r\n"));
    assert(stats_calls == 3);
    reset(); vm_write_repair_diag("COW",&fault,&area,UINT64_MAX,UINT64_MAX,UINT64_MAX,INT64_MIN);
    assert(!strcmp(output,"OS_VM_WRITE_REPAIR stage=COW pid=ffffffffffffffff va=ffffffffffffffff page=ffffffffffffffff index=ffffffffffffffff area_flags=80000000 cow=1 pte_flags=ffffffffffffffff cpu_error=ffffffff status=-9223372036854775808\r\n"));
    current=NULL; reset(); vm_write_repair_diag(NULL,NULL,NULL,0,0,0,K_OK);
    assert(!strcmp(output,"OS_VM_WRITE_REPAIR stage=unknown pid=0 va=0 page=0 index=0 area_flags=0 cow=0 pte_flags=0 cpu_error=0 status=0\r\n"));
    current=&thread; thread.process=NULL; reset();
    vm_write_repair_diag("PROTECT",NULL,NULL,0,0,0,K_EIO);
    assert(strstr(output,"stage=PROTECT pid=0") && strstr(output,"status=-5\r\n"));
    reset(); vm_fault_write_diagnostic("missing",&fault,&area,UINT64_MAX);
    assert(!strcmp(output,"OS_DIAG_VM_FAULT missing ADDRESS=ffffffffffffffff ACCESS=ffffffff AREA_START=123456789abcdef0 AREA_END=ffffffffffffffff PROT=7 FLAGS=80000000 PTE=ffffffffffffffff\r\n"));
    reset(); vm_fault_write_diagnostic("no-area",&fault,NULL,0);
    assert(strstr(output,"AREA_START=0 AREA_END=0 PROT=0 FLAGS=0 PTE=0\r\n"));
    puts("VM_DIAGNOSTIC_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='vm-diagnostics-') as temporary:
    fixture = Path(temporary) / 'test.c'; binary = Path(temporary) / 'test'
    fixture.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(ROOT / 'include'), str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
