"""Production libc bulk memory: guard pages, overlap, bounds and throughput."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'user/libc/memory.c').read_text()
begin = source.index('static void *libc_copy_forward(')
end = source.index('\nint memcmp(', begin)
production = '#include <stddef.h>\n#include <stdint.h>\n'+source[begin:end]
harness = (root/'tests/kernel/string_bulk_test.c').read_text()
harness = harness.replace('int main(void) {', r'''
void *liteos_memmove(void *, const void *, size_t);
static void overlap_test(void) {
    unsigned char *page = guarded_page(), expected[4096];
    const size_t lengths[] = {0,1,7,8,15,31,63,64,65,127,255,1023,4032};
    assert(liteos_memmove(NULL,NULL,0)==NULL);
    for (unsigned n=0;n<sizeof(lengths)/sizeof(lengths[0]);++n) {
        size_t length=lengths[n];
        for (int distance=-31;distance<=31;++distance) {
            for (unsigned edge=0;edge<2;++edge) {
                size_t from=edge ? 4096-length-(distance>0?distance:0) :
                                   (size_t)(distance<0?-distance:0);
                size_t to=from+distance;
                for(size_t i=0;i<4096;++i)page[i]=expected[i]=(unsigned char)(i*37+11);
                /* 先保存预期字节快照，避免拿同样存在重叠问题的算法实现作为对照。 */
                for(size_t i=0;i<length;++i)expected[to+i]=(unsigned char)((from+i)*37+11);
                assert(liteos_memmove(page+to,page+from,length)==page+to);
                for(size_t i=0;i<4096;++i)assert(page[i]==expected[i]);
                /* 测试边界恰好分离或相接时可安全前向复制。 */
                assert(liteos_memmove(page+from,page+from,length)==page+from);
            }
        }
    }
    assert(munmap(page-4096,12288)==0);
}
int main(void) {
    overlap_test();
''').replace('Kernel bulk copy/fill:', 'Libc copy/move/fill:')
with tempfile.TemporaryDirectory(prefix='system-libc-bulk-') as directory:
    path = Path(directory)
    (path/'memory.c').write_text(production)
    (path/'test.c').write_text(harness)
    for sanitized in (True,False):
        obj,exe = path/'memory.o',path/'test'
        flags = ['-O2','-Wall','-Wextra','-Werror','-fno-builtin','-fno-tree-vectorize']
        if sanitized:
            flags += ['-fsanitize=address,undefined','-fno-pie','-no-pie']
        subprocess.run(['cc',*flags,'-Dmemcpy=liteos_memcpy',
                        '-Dmemmove=liteos_memmove','-Dmemset=liteos_memset',
                        '-c',str(path/'memory.c'),'-o',str(obj)],check=True)
        asm = subprocess.check_output(['objdump','-d',str(obj)],text=True)
        assert 'rep movsq' in asm and '\tstd' not in asm
        assert not any(r in asm for r in ('%ymm','%zmm'))
        subprocess.run(['cc',*flags,str(path/'test.c'),str(obj),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
