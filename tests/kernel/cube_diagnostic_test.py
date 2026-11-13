"""Actual cube logging handles short writes and emits each integer in one write."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'tools/nvidia/opengl_cube_demo.c').read_text()
gate = (ROOT/'tools/nvidia/cube_demo_gate.ps1').read_text()
assert 'LITEOS' not in source and 'LiteOS' not in source
assert 'LITEOS_CUBE_' not in gate
produced = set(re.findall(r'OS_CUBE_[A-Z0-9_]+',source))
consumed = set(re.findall(r'OS_CUBE_[A-Z0-9_]+',gate))
assert consumed <= produced, consumed-produced
functions = source[source.index('static void cube_emit('):source.index('static int cube_query_display(')]
functions = functions.replace('write(', 'capture_write(').replace('__errno_location()', 'capture_errno()')
code = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static char output[256];
static unsigned calls, mode;
static size_t used;
static int error_value;
static int *capture_errno(void) {return &error_value;}
static ssize_t capture_write(int fd, const void *text, size_t length) {
    assert(fd==STDOUT_FILENO && text && length); ++calls;
    if (mode==1 && calls==1) {error_value=EINTR; return -1;}
    if (mode==3) return 0;
    if (mode==4) {error_value=EIO; return -1;}
    if (mode==5) {error_value=EAGAIN; return -1;}
    if (mode==6) return (ssize_t)length+1;
    if (mode==2 && length>3) length=3;
    assert(used+length<sizeof(output));
    memcpy(output+used,text,length);used+=length; output[used]=0;
    return (ssize_t)length;
}
static void reset(unsigned m) {memset(output,0,sizeof(output));used=calls=0;mode=m;error_value=0;}
''' + functions + r'''
int main(void) {
    const uint64_t values[]={0,1,9,10,99,100,UINT32_MAX,UINT64_MAX};
    for (unsigned i=0;i<sizeof(values)/sizeof(*values);++i) {
        char expected[32]; snprintf(expected,sizeof(expected),"%llu",(unsigned long long)values[i]);
        for (unsigned m=0;m<3;++m) {
            reset(m);cube_emit_u64(values[i]);assert(!strcmp(output,expected));
            unsigned want=m==2?(strlen(expected)+2)/3:1+(m==1);
            assert(calls==want);
        }
    }
    reset(0);cube_emit(NULL);cube_emit("");assert(!calls && !used);
    reset(2);cube_emit("OS_CUBE_PASS\n");assert(!strcmp(output,"OS_CUBE_PASS\n"));
    for (unsigned m=3;m<=6;++m) {
        reset(m);cube_emit("OS_CUBE_FAIL\n");assert(calls==1 && !used);
    }
    /* 覆盖每个十进制位数边界，而不只是最大值。 */
    for (uint64_t n=10;;n*=10) {
        for (unsigned offset=0;offset<3;++offset) {
            uint64_t value=n-1+offset;char expected[32];
            snprintf(expected,sizeof(expected),"%llu",(unsigned long long)value);
            reset(0);cube_emit_u64(value);assert(!strcmp(output,expected)&&calls==1);
        }
        if (n>UINT64_MAX/10) break;
    }
}
'''
with tempfile.TemporaryDirectory(prefix='cube-diagnostics-') as directory:
    binary = Path(directory)/'test'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',str(binary)],input=code,text=True,check=True)
    subprocess.run([str(binary)],check=True)
print('Cube diagnostics: neutral producer/consumer names, exact uint64 output, one write, short writes/EINTR/error termination passed')
