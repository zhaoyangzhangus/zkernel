"""执行完整 open 路径：缓存命中省去 I/O，但不得绕过打开标志和引用管理。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
start = source.index('kstatus_t vfs_open_kernel(')
body = source[start:source.index('\n#endif', start)]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/vfs.h>
void io_mutex_init(io_mutex_t *mutex, bool priority) { assert(priority); mutex->scheduled.initialized=true; }
#define VFS_PATH_LIMIT 256U
typedef struct {bool directory,fat32_node; void *read,*write;} vfs_backend_node_t;
typedef struct {OS_FAT32 *filesystem;} vfs_fat32_mount_t;
static OS_FAT32 filesystem;
static vfs_fat32_mount_t mount={&filesystem};
static vfs_backend_node_t backend;
static vnode_t node;
static file_t result_file;
static const object_ops_t g_file_object_ops={0};
static unsigned locked,stats,factories,allocations,truncates,creates;
static bool hit=true,disk_exists=true,allocation_fail=false;
static bool uncached_factory;
static vfs_backend_node_t *uncached_backend;
static kstatus_t truncate_status=K_OK;
static void vfs_initialize(void) {}
static void vfs_lock(void) {assert(!locked);locked=1;}
static void vfs_unlock(void) {assert(locked);locked=0;}
void object_get(void *p) {
    assert(locked && p==&node); atomic_fetch_add(&node.object.refs.value,1);
}
void object_put(void *p) {
    assert(!locked && p==&node && atomic_load(&node.object.refs.value)>0);
    if (atomic_load(&node.object.refs.value)==1) {
        assert(uncached_backend && node.fs_private==uncached_backend);
        free(uncached_backend);uncached_backend=0;
    }
    atomic_fetch_sub(&node.object.refs.value,1);
}
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *path,char *relative) {
    assert(path);strcpy(relative,"file");return &mount;
}
static vnode_t *vfs_find_fat_node_locked(const char *path) {
    assert(locked && path);return hit?&node:0;
}
static vnode_t *vfs_find_node_locked(const char *path) {
    assert(locked && path);return 0;
}
BOOLEAN fat32_stat_path(OS_FAT32 *fs,const CHAR8 *path,os_file_info_t *out) {
    assert(!locked && fs==&filesystem && path);++stats;
    *out=(os_file_info_t){.type=backend.directory?OS_FILE_TYPE_DIRECTORY:OS_FILE_TYPE_REGULAR,
                         .size=99};return disk_exists;
}
static bool vfs_mount_parent_exists(const char *path,size_t length) {
    assert(!locked && path && length);return false;
}
static size_t vfs_string_length(const char *path) {return strlen(path);}
static bool vfs_path_range_equals(const char *a,size_t na,const char *b,size_t nb) {
    return na==nb && !memcmp(a,b,na);
}
static kstatus_t vfs_create_fat32_vnode(const char *path,bool keep,
                                       const os_file_info_t *known,vnode_t **out) {
    assert(!locked && path);(void)keep;++factories;
    if(known) assert(known->size==99);
    if(!uncached_factory)atomic_fetch_add(&node.object.refs.value,1);
    *out=&node;return K_OK;
}
static kstatus_t vfs_create_memory_node(const char *path,uint32_t mode,bool directory) {
    assert(!locked && path);(void)mode;(void)directory;++creates;disk_exists=true;return K_OK;
}
static kstatus_t vfs_truncate_node(vnode_t *v,uint64_t size) {
    assert(!locked && v==&node && !size);++truncates;
    if(truncate_status==K_OK) node.size=0;
    return truncate_status;
}
static void *kzalloc(size_t size,unsigned flags) {
    assert(!locked && size==sizeof(file_t) && !flags);++allocations;
    memset(&result_file,0,sizeof(result_file));return allocation_fail?0:&result_file;
}
''' + body + r'''
static void reset(void) {
    assert(!locked && !uncached_backend);memset(&node,0,sizeof(node));refcount_init(&node.object.refs,1);
    backend=(vfs_backend_node_t){false,true,(void*)1,(void*)1};
    node.fs_private=&backend;node.size=4096;
    stats=factories=allocations=truncates=creates=0;
    hit=disk_exists=true;allocation_fail=uncached_factory=false;truncate_status=K_OK;
}
static void release(file_t *file) {
    assert(file==&result_file && file->vnode==&node);
    assert(file->position_lock.scheduled.initialized);
    assert(file->object.type==KOBJECT_TYPE_FILE && atomic_load(&file->object.refs.value)==1);
    object_put(file->vnode);assert(atomic_load(&node.object.refs.value)==1);
}
int main(void) {
    reset();file_t *file=0;
    for(unsigned i=0;i<2000;++i) {
        uint32_t flags=i%2?VFS_OPEN_READ:VFS_OPEN_WRITE|VFS_OPEN_APPEND;
        assert(vfs_open_kernel("/file",flags,0,&file)==K_OK);
        assert(!stats && !factories && file->flags==flags && file->rights==flags);
        assert(file->position==(i%2?0:4096));release(file);
    }
    reset();
    assert(vfs_open_kernel("/file",VFS_OPEN_WRITE|VFS_OPEN_CREATE|VFS_OPEN_EXCLUSIVE,0600,&file)==K_EEXIST);
    assert(!stats && !factories && !allocations && atomic_load(&node.object.refs.value)==1);
    assert(vfs_open_kernel("/file",VFS_OPEN_WRITE|VFS_OPEN_TRUNCATE,0,&file)==K_OK);
    assert(!stats && !factories && truncates==1 && !node.size);release(file);
    reset();truncate_status=K_EBUSY;
    assert(vfs_open_kernel("/file",VFS_OPEN_WRITE|VFS_OPEN_TRUNCATE,0,&file)==K_EBUSY);
    assert(!allocations && atomic_load(&node.object.refs.value)==1);
    reset();allocation_fail=true;
    assert(vfs_open_kernel("/file",VFS_OPEN_READ,0,&file)==K_ENOMEM);
    assert(!stats && atomic_load(&node.object.refs.value)==1);
    reset();backend.write=0;
    assert(vfs_open_kernel("/file",VFS_OPEN_WRITE,0,&file)==K_EACCES);
    assert(!allocations && !stats && atomic_load(&node.object.refs.value)==1);
    reset();backend.read=0;
    assert(vfs_open_kernel("/file",VFS_OPEN_READ,0,&file)==K_EACCES);
    assert(atomic_load(&node.object.refs.value)==1);
    /* 无空闲缓存槽时，工厂返回唯一引用的临时 vnode；失败必须先确定错误再释放。 */
    reset();hit=false;uncached_factory=true;backend.directory=true;
    uncached_backend=malloc(sizeof(*uncached_backend));assert(uncached_backend);
    *uncached_backend=backend;node.fs_private=uncached_backend;
    assert(vfs_open_kernel("/file",VFS_OPEN_READ,0,&file)==K_EISDIR);
    assert(!uncached_backend && !atomic_load(&node.object.refs.value));
    reset();hit=false;uncached_factory=true;
    uncached_backend=malloc(sizeof(*uncached_backend));assert(uncached_backend);
    *uncached_backend=backend;node.fs_private=uncached_backend;
    assert(vfs_open_kernel("/file",VFS_OPEN_READ|VFS_OPEN_DIRECTORY,0,&file)==K_ENOTDIR);
    assert(!uncached_backend && !atomic_load(&node.object.refs.value));
    reset();
    assert(vfs_open_kernel("/file",VFS_OPEN_READ|VFS_OPEN_DIRECTORY,0,&file)==K_ENOTDIR);
    assert(atomic_load(&node.object.refs.value)==1);
    reset();backend.directory=true;backend.write=0;
    assert(vfs_open_kernel("/file",VFS_OPEN_READ|VFS_OPEN_DIRECTORY,0,&file)==K_OK);
    assert(stats==1 && factories==1);release(file);
    reset();hit=false;
    assert(vfs_open_kernel("/file",VFS_OPEN_READ,0,&file)==K_OK);
    assert(stats==1 && factories==1);release(file);
    reset();hit=false;disk_exists=false;
    assert(vfs_open_kernel("/file",VFS_OPEN_READ,0,&file)==K_ENOENT && !factories);
    assert(vfs_open_kernel("/file",VFS_OPEN_WRITE|VFS_OPEN_CREATE|VFS_OPEN_EXCLUSIVE,0600,&file)==K_OK);
    assert(creates==1 && factories==1);release(file);
    reset();
    assert(vfs_open_kernel("/file",VFS_OPEN_READ|VFS_OPEN_TRUNCATE,0,&file)==K_EINVAL);
    assert(vfs_open_kernel("/file",VFS_OPEN_WRITE|VFS_OPEN_APPEND|VFS_OPEN_DIRECTORY,0,&file)==K_EINVAL);
    assert(vfs_open_kernel("relative",VFS_OPEN_READ,0,&file)==K_EINVAL);
    assert(!stats && !factories && !allocations && !locked);
    puts("Cached open: 2000 opens without FAT queries/factory calls; exclusive, append, truncate, permissions, directories, create, OOM and balanced references passed");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-open-flags-') as directory:
    path = Path(directory)
    for variant in ('current', 'cache_disabled', 'old_release'):
        if variant == 'cache_disabled':
            changed = body.replace('if (mounted_path) {', 'if (mounted_path && false) {', 1)
        elif variant == 'old_release':
            fixed = '''kstatus_t failure = backend != 0 && backend->directory ? K_EISDIR :
            (backend != 0 && (flags & VFS_OPEN_DIRECTORY) != 0U ? K_ENOTDIR : K_EACCES);
        object_put(vnode);
        return failure;'''
            assert body.count(fixed) == 1
            changed = body.replace(fixed, '''object_put(vnode);
        return backend != 0 && backend->directory ? K_EISDIR : K_EACCES;''')
        else:
            changed = body
        code = program.replace(body, changed)
        (path / 'test.c').write_text(code)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I'+str(root/'include'),str(path/'test.c'),
                        '-o',str(path/'test')],check=True)
        result = subprocess.run([str(path/'test')],capture_output=True,text=True)
        if variant == 'current':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        elif variant == 'cache_disabled':
            assert result.returncode != 0, '重复查询磁盘的旧路径未被检出'
            print('负对照通过：禁用提前缓存查询会触发零 I/O 断言')
        else:
            assert result.returncode != 0 and 'heap-use-after-free' in result.stderr
            print('负对照通过：释放后继续读取后端会被内存检查检出')
