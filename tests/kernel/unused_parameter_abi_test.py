"""Probe the project's GCC: unused reserved argument across optimization scopes."""
from pathlib import Path
import re
import subprocess
import tempfile

compiler = 'x86_64-linux-gnu-gcc'
common = r'''
#include <stddef.h>
#include <stdint.h>
#define BODY { \
 (void)reserved; size_t words=size/8; \
 __asm__ volatile("cld; rep movsq" : "+D"(dst), "+S"(src), "+c"(words) : : "memory","cc"); \
}
'''
callee = common + r'''
__attribute__((noinline))
void movsq_external(void* dst,const void* src,uintptr_t reserved,size_t size) BODY
'''
caller = common + r'''
#include <assert.h>
#include <string.h>
void movsq_external(void*,const void*,uintptr_t,size_t);
static __attribute__((noinline))
void movsq_local(void* dst,const void* src,uintptr_t reserved,size_t size) BODY
static __attribute__((always_inline)) inline
void movsq_inline(void* dst,const void* src,uintptr_t reserved,size_t size) BODY
// Keep the probe entry points visible with their original three-argument ABI.
__attribute__((noipa))
void call_external(void* d,const void* s,size_t n) {
 movsq_external(d,s,0x11223344,n);
}
__attribute__((noipa))
void call_local(void* d,const void* s,size_t n) {
 movsq_local(d,s,0x11223344,n);
}
__attribute__((noipa))
void call_inline(void* d,const void* s,size_t n) {
 movsq_inline(d,s,0x11223344,n);
}
static volatile unsigned effects;
static uintptr_t reserved_effect(void) { ++effects;return 0x11223344; }
__attribute__((noipa))
void call_effect(void* d,const void* s,size_t n) {
 movsq_inline(d,s,reserved_effect(),n);
}
int main(int argc,char** argv) {
 (void)argv;
 uint64_t source[8],dest[8];
 for(unsigned i=0;i<8;++i)source[i]=i+17U;
 void (*calls[])(void*,const void*,size_t)={call_external,call_local,call_inline,call_effect};
 for(unsigned i=0;i<4;++i) {
  memset(dest,0,sizeof(dest));calls[i](dest,source,sizeof(source));
  assert(!memcmp(dest,source,sizeof(source)));
 }
 assert(effects==1);return argc<1;
}
'''

def function(assembly, name):
    match = re.search(r'^[0-9a-f]+ <'+name+r'>:\n(.*?)(?=\n\n|\Z)', assembly, re.M | re.S)
    if not match:
        raise RuntimeError('Missing probe symbol '+name)
    return match[1]

print(subprocess.check_output([compiler,'--version'],text=True).splitlines()[0])
with tempfile.TemporaryDirectory(prefix='unused-parameter-abi-') as directory:
    path=Path(directory)
    (path/'callee.c').write_text(callee)
    (path/'caller.c').write_text(caller)
    for lto in (False, True):
        flags=['-O2','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',
               '-mno-sse','-mno-mmx','-fno-tree-vectorize']
        if lto:flags+=['-flto']
        binary=path/('lto' if lto else 'separate')
        subprocess.run([compiler,*flags,str(path/'callee.c'),str(path/'caller.c'),
                        '-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
        assembly=subprocess.check_output(['objdump','-d',str(binary)],text=True)
        print('MODE', 'LTO' if lto else 'SEPARATE_TU')
        for name in ('call_external','call_local','call_inline','call_effect'):
            body=function(assembly,name)
            constant='0x11223344' in body
            print(name,'reserved_constant_passed='+str(constant))
            print(body.rstrip())
            if not lto and name=='call_external':assert constant
            if name in ('call_local','call_inline','call_effect'):assert not constant
        assert 'rep movsq' in function(assembly,'call_inline')
        assert 'effects' in function(assembly,'call_effect')
print('RUNTIME_PASS: all copies exact; unused argument side effect executed once')
