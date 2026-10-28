"""Production bounded diagnostic writer, with short/failing storage writes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/rtl8126.c').read_text()
start = source.index('static void rtl8126_persist_diagnostic(')
end = source.index('void rtl8126_emit_diagnostic(void)', start)
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define K_OK 0
#define VFS_OPEN_WRITE 1
#define VFS_OPEN_CREATE 2
#define VFS_OPEN_TRUNCATE 4
#define VFS_OPEN_APPEND 8
typedef int file_t;
static file_t handle;
static char output[1024];
static size_t length;
static unsigned flags, writes, syncs, closes, mode;
static int vfs_open_kernel(const char *path,unsigned f,unsigned perm,file_t **out) {
    assert(!strcmp(path,"/log/rtl8126.log") && perm==0644U);flags=f;
    if(mode==1)return -1;
    *out=&handle;return K_OK;
}
static int vfs_write_kernel(file_t *f,const void *data,size_t size,uint64_t *out) {
    assert(f==&handle);++writes;
    if(mode==2)return -1;
    if(mode==3){*out=0;return K_OK;}
    if(mode==4){*out=size+1;return K_OK;}
    if(size>7)size=7;
    assert(length+size<sizeof(output));memcpy(output+length,data,size);
    length+=size;*out=size;return K_OK;
}
static int vfs_fsync(file_t *f) {assert(f==&handle);++syncs;return -1;}
static void vfs_close(file_t *f) {assert(f==&handle);++closes;}
''' + source[start:end] + r'''
int main(void) {
    const uint32_t values[27]={0,1,0xffffffff,0x80000000,4,5,6,7,8};
    rtl8126_persist_diagnostic(0,values);
    assert(flags==(VFS_OPEN_WRITE|VFS_OPEN_CREATE|VFS_OPEN_TRUNCATE));
    assert(!strcmp(output,"OS_RTL8126_DIAG IRQ=00000000 IRQSTATUS=00000001 RXDESC=ffffffff RXFRAME=80000000 TX=00000004 RXNEXT=00000005 TXLAST=00000006 INT=00000007 INTV2=00000008 CMD=00000000 TXCFG=00000000 RXCFG=00000000 PCICMD=00000000 MISC=00000000 TXBASELO=00000000 TXBASEHI=00000000 RXBASELO=00000000 RXBASEHI=00000000 TXOWN=00000000 TXFIRST=00000000 CMDSTART=00000000 CMDPREIRQ=00000000 CMDPOSTIRQ=00000000 CMDPRE=00000000 CMDPOST=00000000 ENABLES=00000000 ENABLEFAIL=00000000\n"));
    assert(syncs==1 && closes==1);
    length=0;rtl8126_persist_diagnostic(1,values);
    assert(flags==(VFS_OPEN_WRITE|VFS_OPEN_CREATE|VFS_OPEN_APPEND));
    for(mode=1;mode<=4;++mode) {
        unsigned before=writes,old_close=closes;
        rtl8126_persist_diagnostic(2,values);
        assert(writes==before+(mode==1?0:1));
        assert(closes==old_close+(mode==1?0:1));
    }
}
'''
with tempfile.TemporaryDirectory(prefix='rtl-diag-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True)
print('RTL8126 diagnostic formatting, truncate/append, short/error/invalid writes and close PASS')
