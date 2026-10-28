"""执行真实终端分词器，覆盖引号参数起点及原地写入边界。"""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'user/desktop/gshell/main.c').read_text()
source = source[source.index('static bool is_shell_space('):source.index('static bool make_program_path(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define SHELL_MAX_ARGUMENTS 16U
''' + source + r'''
static void check(const char *input, const char *const *expected, unsigned length, bool bg) {
    /* 恰好容纳输入及一个终止符，检测末尾多写一字节。 */
    char *line=malloc(strlen(input)+1);assert(line);strcpy(line,input);
    char *arguments[16];uint32_t count=999;bool background=!bg;
    assert(shell_tokenize(line,arguments,&count,&background));
    assert(count==length && background==bg);
    for(unsigned i=0;i<count;++i)assert(!strcmp(arguments[i],expected[i]));
    free(line);
}
int main(void) {
    const char *quoted[]={"touch","/Downloads/fs-probe.t t"};
    check("touch \"/Downloads/fs-probe.t t\"",quoted,2,false);
    check("touch '/Downloads/fs-probe.t t'",quoted,2,false);
    const char *escaped[]={"echo","two words","x y",""};
    check("echo two\\ words x\" \"y ''",escaped,4,false);
    const char *empty[]={"","",""};check("'' \"\" ''",empty,3,false);
    const char *joined[]={"abcd","&"};check("a'b'\"c\"d '&' &",joined,2,true);
    const char *plain[]={"cat","a","b"};check("cat a b",plain,3,false);
    check(" cat  a\tb  ",plain,3,false);
    const char *single[]={"abcdef"};check("abcdef",single,1,false);
    check("",NULL,0,false);check(" \t ",NULL,0,false);
    const char *bad[]={"cat '","cat \"","cat \\","cat & more"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){
        char line[64];strcpy(line,bad[i]);char *args[16];uint32_t count;bool bg;
        assert(!shell_tokenize(line,args,&count,&bg));
    }
    char maximum[33];for(unsigned i=0;i<16;++i){maximum[2*i]='x';maximum[2*i+1]=' ';}
    maximum[31]=0;const char *words[16];for(unsigned i=0;i<16;++i)words[i]="x";
    check(maximum,words,16,false);
    char overflow[]="x x x x x x x x x x x x x x x x x";
    char *args[16];uint32_t count;bool bg;
    assert(!shell_tokenize(overflow,args,&count,&bg));
    puts("Shell tokenize: quoted/escaped/empty/concatenated arguments, background jobs, argument limit and exact-size buffers passed");
}
'''
with tempfile.TemporaryDirectory(prefix='shell-tokenize-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True)
