"""Neutral syscall entries retain register semantics without retired exports."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'user/libc/syscall.c').read_text()
start=source.index('static int64_t libc_raw_syscall6(')
end=source.index('static size_t linux_syscall_argument_count(',start)
entry=source[start:end]
wrappers=entry[entry.index('int64_t __libc_linux_syscall6('):]
legacy=entry
legacy=legacy.replace('__libc_native_syscall6','liteos_syscall6').replace('__libc_linux_syscall6','liteos_linux_syscall6')
for path in (ROOT/'user').rglob('*'):
    if path.suffix not in ('.c','.h','.S'): continue
    if path in (ROOT/'user/libc/syscall.c',ROOT/'user/libc/include/libc/libc.h'): continue
    assert not re.search(r'\bliteos_(?:linux_)?syscall6\b',path.read_text()),path
assert 'liteos_syscall6' not in (ROOT/'tools/chromium/linux_events_test.c').read_text()
for path in (ROOT/'tests').rglob('*.c'):
    assert not re.search(r'\bliteos_(?:linux_)?syscall6\b',path.read_text()),path
for file in ('tools/verify-libc.sh','tools/verify-libc.ps1'):
    verifier=(ROOT/file).read_text()
    for name in ('__libc_native_syscall6','__libc_linux_syscall6'):
        assert name in verifier
    for name in ('liteos_syscall6','liteos_linux_syscall6'):
        assert name not in verifier

preamble='#include <stdint.h>\n#include <uapi/syscall.h>\n'
behavior=r'''
#include <assert.h>
#include <string.h>
static uint64_t received[7];
static int64_t returned;
static int64_t libc_raw_syscall6(uint64_t n,uint64_t a,uint64_t b,uint64_t c,
                                uint64_t d,uint64_t e,uint64_t f) {
    uint64_t values[]={n,a,b,c,d,e,f}; memcpy(received,values,sizeof(values));
    return returned;
}
''' + wrappers + r'''
int main(void) {
    typedef int64_t (*call_t)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
    call_t calls[]={__libc_linux_syscall6,__libc_native_syscall6};
    const uint64_t numbers[]={0,1,449,UINT32_MAX,UINT64_MAX,OS_SYSCALL_NATIVE_FLAG};
    const int64_t returns[]={0,17,-22,INT64_MIN,INT64_MAX};
    for (unsigned i=0;i<2;++i) for (unsigned n=0;n<sizeof(numbers)/sizeof(*numbers);++n)
        for (unsigned r=0;r<sizeof(returns)/sizeof(*returns);++r) {
            uint64_t expected[]={numbers[n],UINT64_MAX,0,UINT64_C(0x123456789abcdef0),4,5,6};
            if (i==1) expected[0]|=OS_SYSCALL_NATIVE_FLAG;
            returned=returns[r];
            assert(calls[i](numbers[n],expected[1],expected[2],expected[3],4,5,6)==returned);
            assert(!memcmp(received,expected,sizeof(expected)));
        }
}
'''
with tempfile.TemporaryDirectory(prefix='syscall-namespace-') as directory:
    base=Path(directory)
    flags=['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include')]
    for name,text in (('new',entry),('legacy',legacy)):
        c=base/(name+'.c'); c.write_text(preamble+text)
        obj=base/(name+'.o')
        subprocess.run(flags+['-c',str(c),'-o',str(obj)],check=True)
        subprocess.run(['objcopy','-O','binary','--only-section=.text',str(obj),str(base/(name+'.text'))],check=True)
    assert (base/'new.text').read_bytes()==(base/'legacy.text').read_bytes()
    symbols=subprocess.check_output(['nm','-g','--defined-only',str(base/'new.o')],text=True)
    table={r.split()[2]:r.split()[0] for r in symbols.splitlines()}
    assert '__libc_native_syscall6' in table and 'liteos_syscall6' not in table
    assert '__libc_linux_syscall6' in table and 'liteos_linux_syscall6' not in table
    c=base/'behavior.c'; c.write_text(preamble+behavior)
    subprocess.run(flags+[str(c),'-o',str(base/'behavior')],check=True)
    subprocess.run([str(base/'behavior')],check=True)
    for compiler in ('cc','c++'):
        c=base/('header.cc' if compiler=='c++' else 'header.c')
        c.write_text('#include <libc/libc.h>\n'+
                     'void check(void) {\n'+
                     'int64_t (*a)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)=__libc_native_syscall6;\n'+
                     'int64_t (*b)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)=__libc_linux_syscall6;\n'+
                     '(void)a;(void)b;\n}\n')
        subprocess.run([compiler,'-Wall','-Wextra','-Werror','-fsyntax-only',
                        '-I'+str(ROOT/'include'),'-I'+str(ROOT/'user/libc/include'),str(c)],check=True)
print('Syscall namespace: neutral consumers, C/C++ headers, retired exports absent, identical instructions and all six argument slots passed')
