"""Actual best-effort boot-volume cache diagnostic, including I/O failures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/init/runtime.c').read_text()
start = source.index('static void runtime_record_display_cache(')
end = source.index('\n}', start) + 2
program = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define K_OK 0
#define VFS_OPEN_WRITE 1
#define VFS_OPEN_CREATE 2
#define VFS_OPEN_TRUNCATE 4
typedef struct { int unused; } file_t;
static file_t file;
static char saved[64];
static unsigned mode, used, closes, syncs;
static int vfs_open_kernel(const char *path, unsigned flags, unsigned perms, file_t **out) {
    assert(!strcmp(path,"/log/display-cache.log") && flags==7 && perms==0644);
    if(mode==1) return -5;
    *out=mode==2?0:&file; return K_OK;
}
static int vfs_write_kernel(file_t *f, const void *text, size_t length, uint64_t *written) {
    assert(f==&file);
    if(mode==3) return -5;
    if(mode==4) { *written=0; return K_OK; }
    if(mode==5) { *written=length+1; return K_OK; }
    if(length>3) length=3;
    memcpy(saved+used,text,length);used+=length;*written=length;return K_OK;
}
static int vfs_fsync(file_t *f) { assert(f==&file); ++syncs; return -5; }
static void vfs_close(file_t *f) { assert(f==&file); ++closes; }
''' + source[start:end] + r'''
int main(void) {
    for(mode=0;mode<6;++mode) for(unsigned wc=0;wc<2;++wc) {
        used=closes=syncs=0;memset(saved,0,sizeof(saved));
        runtime_record_display_cache(wc);
        if(mode==1 || mode==2) { assert(!closes && !syncs && !used); continue; }
        assert(closes==1 && syncs==1);
        if(mode==0) assert(!strcmp(saved,wc?"OS_GOP_CACHE_WC_OK\n":"OS_GOP_CACHE_UC_RETAINED\n"));
        else assert(!used);
    }
    puts("Display cache diagnostic: WC/UC, short writes, open/write/sync failures and cleanup PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='display-cache-log-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=10)
