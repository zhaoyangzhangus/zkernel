"""验证真实数字追加函数的容量边界、完整输出与目标扫描次数。"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'user/desktop/taskmgr/main.c').read_text()

def function(name):
    match = re.search(r'^static [^\n]*\b'+name+r'\([^;{}]*\)\s*\{', source, re.M)
    assert match, name
    start = match.start()
    end = source.index('{', start)+1
    depth = 1
    while depth:
        depth += (source[end] == '{')-(source[end] == '}')
        end += 1
    return source[start:end]

program = r'''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#define TASKMGR_STATUS_CAP 96U
static unsigned scans,cases;
''' + function('text_length').replace('uint32_t length = 0U;',
    '++scans; uint32_t length = 0U;') + '\n' + function('append_text') + '\n' + function('append_u64') + r'''
static void check(uint64_t value,unsigned capacity,unsigned prefix) {
    unsigned char actual[128],expected[128];
    memset(actual,0xa5,sizeof(actual));
    for(unsigned i=0;i<prefix;++i)actual[8+i]=(unsigned char)('A'+i%26);
    actual[8+prefix]=0;
    memcpy(expected,actual,sizeof(actual));
    char number[32];
    int n=snprintf(number,sizeof(number),"%" PRIu64,value);
    assert(n>0 && n<=20);
    if(capacity) {
        unsigned available=capacity-1-prefix;
        unsigned copied=(unsigned)n<available?(unsigned)n:available;
        memcpy(expected+8+prefix,number,copied);
        expected[8+prefix+copied]=0;
    }
    scans=0;
    append_u64((char*)actual+8,capacity,value);
    assert(!memcmp(actual,expected,sizeof(actual)));
    assert(scans==1);
    ++cases;
}
int main(void) {
    uint64_t values[256];unsigned count=0;
    values[count++]=0;values[count++]=UINT64_MAX;
    for(unsigned bit=0;bit<64;++bit) {
        uint64_t power=UINT64_C(1)<<bit;
        values[count++]=power;values[count++]=power-1;
        if(power<UINT64_MAX)values[count++]=power+1;
    }
    for(uint64_t power=10;power<=UINT64_MAX/10;power*=10) {
        values[count++]=power-1;values[count++]=power;values[count++]=power+1;
    }
    assert(count<=256);
    for(unsigned cap=1;cap<=96;++cap)
        for(unsigned prefix=0;prefix<cap;++prefix)
            for(unsigned i=0;i<count;++i)check(values[i],cap,prefix);
    uint64_t rng=19;
    for(unsigned i=0;i<50000;++i) {
        rng=rng*UINT64_C(6364136223846793005)+1;
        unsigned cap=1+(unsigned)(rng>>32)%96;
        check(rng,cap,(unsigned)rng%cap);
    }
    check(UINT64_MAX,0,0);
    scans=0;append_u64(NULL,0,UINT64_MAX);assert(scans==1);
    printf("Task numbers: %u snprintf oracles, capacities 0..96, all uint64 digit boundaries, guards; one destination scan PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='taskmgr-numbers-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-pie','-no-pie',
        str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
