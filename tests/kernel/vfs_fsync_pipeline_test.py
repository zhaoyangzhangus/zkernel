"""Production file/range sync share writeback, lock ordering and errors."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
pages=(root/'kernel/fs/vfs/page_cache.c').read_text()
io=(root/'kernel/fs/vfs/file_io.c').read_text()
functions=pages[pages.index('kstatus_t vfs_page_cache_sync('):pages.index('static kstatus_t vfs_file_page_get_batch(')]
functions+=io[io.index('kstatus_t vfs_fsync('):io.index('void vfs_close(')]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define K_OK 0
#define K_EINVAL -22
#define K_EIO -5
#define K_EOVERFLOW -75
#define KOBJECT_TYPE_FILE 3
typedef int kstatus_t;
typedef struct { unsigned unused; } io_mutex_t;
typedef struct { void *context; kstatus_t (*fsync)(void *); io_mutex_t write_mutex; } vfs_backend_node_t;
typedef struct { void *fs_private; } vnode_t;
typedef struct { struct { unsigned type; } object; vnode_t *vnode; } file_t;
static unsigned transaction_lock,vnode_lock,backend_lock,flushes,syncs;
static int lock_result,flush_result,sync_result;
static uint64_t expected_first,expected_count;
static vfs_backend_node_t backend;
static int io_mutex_lock(io_mutex_t *m) {
    assert(m==&backend.write_mutex && !transaction_lock && !vnode_lock && !backend_lock);
    if (lock_result) return lock_result;
    transaction_lock=1;return K_OK;
}
static void io_mutex_unlock_required(io_mutex_t *m) {
    assert(m==&backend.write_mutex && transaction_lock && !vnode_lock && !backend_lock);
    transaction_lock=0;
}
static void vfs_vnode_lock(vnode_t *v) { assert(v && transaction_lock && !vnode_lock && !backend_lock);vnode_lock=1; }
static void vfs_vnode_unlock(vnode_t *v) { assert(v && vnode_lock && !backend_lock);vnode_lock=0; }
static void vfs_backend_lock(vfs_backend_node_t *b) { assert(b==&backend && transaction_lock && !vnode_lock && !backend_lock);backend_lock=1; }
static void vfs_backend_unlock(vfs_backend_node_t *b) { assert(b==&backend && backend_lock);backend_lock=0; }
static kstatus_t vfs_page_cache_flush_locked(vnode_t *v,uint64_t first,uint64_t count) {
    assert(v && vnode_lock && !backend_lock && first==expected_first && count==expected_count);
    ++flushes;return flush_result;
}
static kstatus_t sync_backend(void *context) {
    assert(context==&backend && !vnode_lock && backend_lock && flushes>syncs);
    ++syncs;return sync_result;
}
''' + functions + r'''
int main(void) {
    backend.context=&backend;backend.fsync=sync_backend;
    vnode_t vnode={&backend};file_t file={{KOBJECT_TYPE_FILE},&vnode};
    expected_count=UINT64_MAX;
    assert(vfs_fsync(&file)==K_OK && flushes==1 && syncs==1);
    flush_result=-12;
    assert(vfs_fsync(&file)==-12 && flushes==2 && syncs==1);
    flush_result=0;sync_result=-5;
    assert(vfs_fsync(&file)==-5 && flushes==3 && syncs==2);
    backend.fsync=0;
    assert(vfs_fsync(&file)==K_OK && flushes==4 && syncs==2);
    vnode.fs_private=0;
    assert(vfs_fsync(&file)==K_EIO && flushes==4);
    assert(vfs_fsync(0)==K_EINVAL);file.object.type=0;
    assert(vfs_fsync(&file)==K_EINVAL);file.object.type=KOBJECT_TYPE_FILE;file.vnode=0;
    assert(vfs_fsync(&file)==K_EINVAL);
    vnode.fs_private=&backend;backend.fsync=sync_backend;sync_result=0;
    expected_first=17;expected_count=2;
    assert(vfs_page_cache_sync(&vnode,17,2)==K_OK && flushes==5 && syncs==3);
    assert(vfs_page_cache_sync(&vnode,17,0)==K_EINVAL && flushes==5);
    assert(vfs_page_cache_sync(0,17,2)==K_EINVAL);
    expected_first=0;expected_count=UINT64_MAX;
    assert(vfs_page_cache_writeback(&vnode)==K_OK && flushes==6 && syncs==3);
    flush_result=-12;
    assert(vfs_page_cache_writeback(&vnode)==-12 && flushes==7 && syncs==3);
    flush_result=0;lock_result=-5;
    assert(vfs_page_cache_writeback(&vnode)==-5 && flushes==7);
    assert(vfs_page_cache_sync(&vnode,0,1)==-5 && flushes==7);
    lock_result=0;vnode.fs_private=0;
    assert(vfs_page_cache_writeback(&vnode)==K_OK);
    assert(vfs_page_cache_sync(&vnode,0,1)==K_OK);
    assert(vfs_page_cache_sync(&vnode,1,UINT64_MAX)==K_EINVAL);
    assert(vfs_page_cache_sync(&vnode,UINT64_MAX,2)==K_EOVERFLOW);
    assert(vfs_page_cache_writeback(0)==K_EINVAL);
    assert(!transaction_lock && !vnode_lock && !backend_lock);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fsync-pipeline-') as directory:
    path=Path(directory);(path/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('VFS shared sync: full/range writeback, lock ordering, backend errors and invalid file semantics passed')
