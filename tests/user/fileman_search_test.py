"""真实文件名匹配与独立逐位置对照，约束重复前缀的解码工作量。"""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'user/desktop/fileman/main.c').read_text()
start=source.index('static char fm4_fold(')
end=source.index('\n}',start)+2
fold=source[start:end].replace('{','{ ++fold_calls;',1)
start=source.index('typedef struct fm4_search {')
search=source[start:source.index('static void fm4_apply_filter(void) {',start)]
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static char g_query[128];
static unsigned fold_calls;
'''+fold+search+r'''
static unsigned char lower(unsigned char c) {return c>='A'&&c<='Z'?c+32:c;}
static bool reference(const char *name) {
    size_t n=strlen(name),q=strlen(g_query);
    for(size_t start=0;start<=n;++start) {
        size_t i=0;
        while(i<q && i<n-start && lower(name[start+i])==lower(g_query[i]))++i;
        if(i==q)return true;
    }
    return false;
}
static unsigned cases;
static void verify(const char *name) {
    fold_calls=0;
    fm4_search_t search=fm4_search_prepare();
    assert(fold_calls==strlen(g_query));
    unsigned prepared=fold_calls;
    bool actual=fm4_name_matches(name,&search);
    assert(actual==reference(name));
    assert(fold_calls-prepared<=strlen(name));++cases;
}
int main(void) {
    const char *samples[]={"","A","aAaAaAb","中文😀.PNG","document.txt","中A文B"};
    for(unsigned i=0;i<6;++i)for(unsigned j=0;j<6;++j){strcpy(g_query,samples[j]);verify(samples[i]);}
    char name[257];uint32_t random=1;
    for(unsigned sample=0;sample<50000;++sample) {
        random=random*1664525U+1013904223U;unsigned n=random%257;
        random=random*1664525U+1013904223U;unsigned q=random%128;
        for(unsigned i=0;i<n;++i){random=random*1664525U+1013904223U;name[i]=(char)(1+random%255);}
        name[n]=0;
        for(unsigned i=0;i<q;++i){random=random*1664525U+1013904223U;g_query[i]=(char)(1+random%255);}
        g_query[q]=0;
        if(q&&q<=n&&sample%2==0)memcpy(g_query,name+n-q,q);
        verify(name);
    }
    /* 先匹配很长的重复前缀再失败，旧逐位置算法会反复折叠同一段。 */
    for(unsigned n=1;n<sizeof(name);++n)for(unsigned q=1;q<sizeof(g_query);++q) {
        memset(name,'a',n);name[n]=0;
        memset(g_query,'A',q);g_query[q-1]='b';g_query[q]=0;
        verify(name);name[n-1]='B';verify(name);
    }
    printf("Fileman search: %u independent match oracles, ASCII/UTF-8/raw bytes, 127-byte queries and repeated prefixes; one fold per input byte PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='fileman-search-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
