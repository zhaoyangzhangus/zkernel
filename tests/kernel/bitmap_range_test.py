"""Actual bitmap header versus scalar range writes, with guard-word checks."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
program = r'''
#include <kernel/bitmap.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define WORDS 4096U
static uint64_t actual[WORDS+2], expected[WORDS+2];
static uint64_t seed=871321;
static uint64_t random_word(void) {
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed;
}
static void check(size_t start, size_t count, bool value) {
    for (unsigned i=0; i<WORDS+2; ++i) actual[i]=random_word();
    memcpy(expected,actual,sizeof(actual));
    for (size_t i=0; i<count; ++i) {
        uint64_t mask=1ULL<<((start+i)%64);
        if (value) expected[1+(start+i)/64] |= mask;
        else expected[1+(start+i)/64] &= ~mask;
    }
    bitmap_set_range(actual+1,start,count,value);
    assert(!memcmp(actual,expected,sizeof(actual)));
}
int main(void) {
    /* 遍历跨越四个字边界的所有偏移和长度组合。 */
    for (unsigned start=0; start<128; ++start)
        for (unsigned count=0; count<=257; ++count)
            for (unsigned v=0; v<2; ++v) check(start,count,v);
    const size_t capacity=WORDS*64;
    check(0,capacity,true); check(0,capacity,false);
    check(1,capacity-2,true); check(1,capacity-2,false);
    check(capacity-1,1,true); check(capacity-1,1,false);
    check(capacity,0,true); bitmap_set_range(0,SIZE_MAX,0,false);
    for (unsigned i=0; i<2000; ++i) {
        size_t start=random_word()%capacity;
        size_t count=random_word()%(capacity-start+1);
        check(start,count,i&1);
    }
    puts("Bitmap ranges: exhaustive offsets/word edges, 262144-bit capacity, guards, zero-length/null and 2000 random cases passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-bitmap-range-') as directory:
    path=Path(directory); (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=30)
