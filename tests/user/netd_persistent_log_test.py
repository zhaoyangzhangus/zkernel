"""Production netd startup log: bounded, partial writes and failure isolation."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/services/networkd/netd.c').read_text()
start = source.index('static void netd_persist_debug(const char *text, size_t length) {')
function = source[start:source.index('static int64_t netd_syscall_six(', start)]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
typedef uint64_t os_handle_t;
enum {OS_INVALID_HANDLE=0,OS_SYS_FILE_OPEN=1,OS_SYS_FILE_WRITE=2,OS_SYS_FILE_FSYNC=3,
 OS_FILE_OPEN_WRITE=1,OS_FILE_OPEN_CREATE=2,OS_FILE_OPEN_TRUNCATE=4};
static unsigned mode,opens,writes,syncs,total;
static char bytes[16384];
static int64_t netd_syscall_four(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d){
 if(n==OS_SYS_FILE_OPEN){
  ++opens;assert(!strcmp((char*)(uintptr_t)a,"/log/netd.log") && b==7 && c==0644);
  if(mode==1)return -5;*(os_handle_t*)(uintptr_t)d=9;return 0;
 }
 assert(n==OS_SYS_FILE_WRITE && a==9 && c && c<=240);++writes;
 if(mode==2)return -5;
 uint64_t *count=(uint64_t*)(uintptr_t)d;
 if(mode==3){*count=0;return 0;}
 if(mode==4){*count=c+1;return 0;}
 if(c>7)c=7;
 assert(total+c<=sizeof(bytes));memcpy(bytes+total,(void*)(uintptr_t)b,c);total+=c;
 *count=c;return 0;
}
static int64_t netd_syscall_one(uint64_t n,uint64_t a){
 assert(n==OS_SYS_FILE_FSYNC && a==9);++syncs;return -5;
}
''' + function + r'''
int main(int argc,char **argv){
 assert(argc==2);mode=atoi(argv[1]);
 char message[240];memset(message,'X',sizeof(message));
 netd_persist_debug(0,1);netd_persist_debug(message,0);netd_persist_debug(message,241);
 assert(!opens);
 for(unsigned i=0;i<1000;++i)netd_persist_debug(message,sizeof(message));
 assert(opens==1);
 if(mode==1)assert(!writes && !syncs);
 else if(mode)assert(writes==1000 && syncs==1000 && !total);
 else {
  assert(total==16320 && syncs==68);
  for(unsigned i=0;i<total;++i)assert(bytes[i]=='X');
 }
}
'''
with tempfile.TemporaryDirectory(prefix='netd-log-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    for mode in range(5):
        subprocess.run([str(path / 'test'), str(mode)], check=True)
print('Netd persistent log: 16KiB cap, open-once, short/failed/zero/invalid writes and fsync failures PASS')
