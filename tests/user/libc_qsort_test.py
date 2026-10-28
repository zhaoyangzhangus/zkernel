"""真实自有排序与宿主排序逐字节比较，覆盖未对齐条目及字宽交换尾部。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/libc/stdlib.c').read_text()
sort = source[source.index('static void swap_bytes('):source.index('void *bsearch(')]
sort = sort.replace('void qsort(', 'static void own_qsort(')
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
'''
main = r'''
static size_t element_size;
static unsigned cases;
static int compare(const void *a,const void *b) { return memcmp(a,b,element_size); }
static void verify(size_t size,size_t count,unsigned alignment,unsigned pattern) {
    size_t bytes=count*size,total=bytes+32;
    unsigned char *actual=malloc(total),*expected=malloc(total);
    assert(actual && expected);
    memset(actual,0xa5,total);
    unsigned char *data=actual+8+alignment;
    uint32_t random=0x12345678;
    for(size_t i=0;i<count;++i)for(size_t j=0;j<size;++j) {
        random=random*1664525U+1013904223U;
        data[i*size+j]=pattern==0 ? (unsigned char)(random>>24) :
            pattern==1 ? (unsigned char)i : pattern==2 ? (unsigned char)(count-i) : 17;
    }
    memcpy(expected,actual,total);
    element_size=size;
    qsort(expected+8+alignment,count,size,compare);
    own_qsort(data,count,size,compare);
    assert(!memcmp(actual,expected,total));
    free(actual);free(expected);++cases;
}
int main(void) {
    /* 相邻元素可以跨越任意字节边界，头尾哨兵必须原样保留。 */
    for(size_t size=1;size<=65;++size)
        for(size_t count=0;count<=33;++count)
            for(unsigned alignment=0;alignment<8;++alignment)
                for(unsigned pattern=0;pattern<4;++pattern)
                    verify(size,count,alignment,pattern);
    const size_t widths[]={255,256,257,304,511,512,513,1024,1031};
    for(unsigned w=0;w<sizeof(widths)/sizeof(widths[0]);++w)
        for(unsigned alignment=0;alignment<8;++alignment)
            for(unsigned pattern=0;pattern<4;++pattern)
                verify(widths[w],4096,alignment,pattern);
    unsigned char byte=11;
    own_qsort(0,2,1,compare);own_qsort(&byte,2,0,compare);
    own_qsort(&byte,2,1,0);assert(byte==11);
    printf("Own qsort: %u byte-exact host oracles, unaligned records, word tails, duplicates and canaries PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='libc-qsort-') as directory:
    path = Path(directory)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    def run(code, negative=False):
        (path/'test.c').write_text(prefix+code+main)
        subprocess.run(['cc','-std=c11','-O2','-fno-builtin','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        result = subprocess.run([str(path/'test')],capture_output=negative,text=True,timeout=60)
        if negative:
            assert result.returncode and '!memcmp(actual,expected,total)' in result.stderr, result
        else:
            assert result.returncode == 0, result
    run(sort)
    negative=sort.replace('while (size-- != 0U)', 'while (size-- > 1U)',1)
    assert negative!=sort
    run(negative, True)
print('负面对照通过：漏交换尾部字节会被完整内容和哨兵比较拒绝')
