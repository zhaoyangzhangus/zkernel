"""Power syscall tests use fake firmware: never reboot the host."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/debug.c').read_text()
start = source.index('int64_t syscall_system_power(')
end = source.index('#ifndef OS_DEBUG_SERIAL', start)
power = (root / 'kernel/power.c').read_text()
power = power[power.index('kstatus_t power_system_request('):power.index('static struct {')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <setjmp.h>
typedef int kstatus_t;
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define K_EBUSY -16
#define K_EIO -5
#define K_ENOSYS -38
static bool process=true, reenter;
static int sync_result, off_result, net_result;
static unsigned syncs, offs, reboots, stops;
static bool synced, stopped;
static jmp_buf jump;
static void *current_process(void) {return process ? &process : 0;}
int64_t syscall_system_power(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static int vfs_sync_all(void) {
    ++syncs;
    synced=false;stopped=false;
    if(reenter)assert(syscall_system_power(0,0,0,0,0,0)==K_EBUSY);
    synced=sync_result==K_OK;return sync_result;
}
static int e1000_shutdown(void) {
    assert(synced);++stops;
    if(reenter)assert(syscall_system_power(1,0,0,0,0,0)==K_EBUSY);
    stopped=net_result==K_OK;return net_result;
}
static void x86_system_reboot(void) {assert(stopped);++reboots;longjmp(jump,1);}
static int x86_acpi_poweroff(void) {assert(stopped);++offs;return off_result;}
''' + power + source[start:end] + r'''
int main(void) {
    assert(syscall_system_power(2,0,0,0,0,0)==K_EINVAL && !syncs);
    process=false;assert(syscall_system_power(0,0,0,0,0,0)==K_EPERM && !syncs);
    process=true;sync_result=K_EIO;
    assert(syscall_system_power(0,0,0,0,0,0)==K_EIO && !reboots && !offs);
    assert(syscall_system_power(1,0,0,0,0,0)==K_EIO && !offs);
    assert(!stops);
    sync_result=K_OK;net_result=K_EBUSY;
    assert(syscall_system_power(1,0,0,0,0,0)==K_EBUSY && !offs && stops==1);
    net_result=K_EIO;
    assert(syscall_system_power(0,0,0,0,0,0)==K_EIO && !reboots && stops==2);
    net_result=K_OK;
    sync_result=K_OK;off_result=K_ENOSYS;reenter=true;
    assert(syscall_system_power(1,0,0,0,0,0)==K_ENOSYS && offs==1);
    off_result=K_OK;
    assert(syscall_system_power(1,0,0,0,0,0)==K_EIO && offs==2);
    if(!setjmp(jump)) {syscall_system_power(0,0,0,0,0,0);assert(0);}
    assert(reboots==1 && syncs==7 && stops==5);
}
'''
with tempfile.TemporaryDirectory(prefix='system-power-test-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
print('Power control: validation, sync then NIC stop before firmware, failures/retry, reentry and firmware failure PASS')
