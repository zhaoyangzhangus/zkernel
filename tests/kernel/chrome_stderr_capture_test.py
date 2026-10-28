#!/usr/bin/env python3
"""Exercise the native Chrome diagnostic syscall with global serial disabled."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
linux = (root / "kernel/syscall/linux.c").read_text()
debug = (root / "kernel/syscall/debug.c").read_text()
identity = linux[linux.index("static bool linux_process_is_chrome_image("):
                 linux.index("static void linux_chrome_diag_char(")]
capture = linux[linux.index("void linux_chrome_stderr_write("):
                linux.index("\n#if OS_DEBUG_SERIAL", linux.index(
                    "void linux_chrome_stderr_write("))]
debug = ("#define DEBUG_WRITE_MAX_BYTES 240U\n" +
         debug[debug.index("#ifndef OS_DEBUG_SERIAL"):])
mock = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef int kstatus_t;
typedef struct { char linux_exe[256]; } process_t;
#define __user
#define K_OK 0
#define K_EINVAL -22
#define OS_DEBUG_SERIAL 0
#define LINUX_CHROME_STDERR_LIMIT (16U * 1024U)
static process_t process;
static unsigned emitted, forbidden_serial;
static uint32_t g_linux_chrome_stderr_bytes;
static uint32_t g_linux_chrome_trace_bytes;
static process_t *current_process(void) { return &process; }
static void linux_chrome_diag_char(char c) { (void)c; ++emitted; }
static kstatus_t copy_from_user(void *d, const void *s, size_t n) {
    memcpy(d,s,n); return K_OK;
}
'''
cases = r'''
int main(void) {
    char text[240]; memset(text,'x',sizeof(text));
    strcpy(process.linux_exe,"/sbin/other");
    assert(syscall_debug_write((uintptr_t)text,240,0,0,0,0)==240 && !emitted);
    strcpy(process.linux_exe,"/usr/apps/chrome/chrome");
    const char *trace="LITEOS_FRAME_DIAGNOSTIC\n";
    for(unsigned i=0;i<1000;++i)
        linux_chrome_stderr_write(&process,trace,strlen(trace));
    assert(emitted==4096 && g_linux_chrome_stderr_bytes==0);
    emitted=0;
    assert(syscall_debug_write((uintptr_t)text,240,0,0,0,0)==240 && emitted==240);
    assert(syscall_debug_write((uintptr_t)text,241,0,0,0,0)==K_EINVAL);
    assert(syscall_debug_write(0,0,0,0,0,0)==0);
    for (unsigned i=0;i<100;++i)
        assert(syscall_debug_write((uintptr_t)text,240,0,0,0,0)==240);
    assert(emitted==LINUX_CHROME_STDERR_LIMIT && !forbidden_serial);
}
'''
with tempfile.TemporaryDirectory(prefix="system-chrome-stderr-test-") as directory:
    binary = str(pathlib.Path(directory) / "test")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-", "-o", binary],
                   input=mock + identity + capture + debug + cases,
                   text=True, check=True)
    subprocess.run([binary], check=True)
print("chrome-stderr: native capture works with global serial off; filtering and 16 KiB cap OK")
