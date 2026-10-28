"""执行真实句柄获取和读写分支，防止读写文件每次 I/O 都重新解析路径。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
core = (root / 'kernel/fs/vfs/core.c').read_text()
start = core.index('    if (!backend->directory &&\n        !fat32_open')
acquire = core[start:core.index('    /*\n    * 优先使用规范 FAT vnode', start)]
source = (root / 'kernel/fs/vfs/backend.c').read_text()
rw = source[source.index('kstatus_t vfs_fat32_backend_read('):
            source.index('kstatus_t vfs_fat32_backend_fsync(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/fat32.h>
#define VFS_PATH_LIMIT 256U
typedef struct {bool directory,detached;OS_FAT32_FILE *fat_file;
                uint64_t size,content_revision;char path[256];} vfs_backend_node_t;
typedef struct {unsigned unused;} vnode_t;
typedef struct {OS_FAT32 *filesystem;} vfs_fat32_mount_t;
static OS_FAT32 fs;
static OS_FAT32_FILE native;
static vfs_fat32_mount_t mounted={&fs};
static vnode_t node;
static unsigned opens,closes,releases,drops,routes,fail_mode,memory_calls;
static unsigned char media[8192];
BOOLEAN fat32_open(OS_FAT32 *filesystem,const CHAR8 *path,OS_FAT32_FILE **out) {
    assert(filesystem==&fs && !strcmp(path,"db"));++opens;*out=0;
    if(fail_mode==2 || (fail_mode==1 && opens==1))return 0;
    assert(!native.Used);native.Used=1;native.FileSystem=&fs;native.Size=4096;
    *out=&native;return 1;
}
BOOLEAN fat32_close(OS_FAT32_FILE *file) {
    assert(file==&native && file->Used);++closes;file->Used=0;return 1;
}
static bool vfs_release_idle_fat_handle(OS_FAT32 *filesystem) {
    assert(filesystem==&fs);++releases;return fail_mode==1;
}
void object_put(void *vnode) {assert(vnode==&node);++drops;}
static kstatus_t acquire(vfs_backend_node_t *backend,bool keep_fat_file) {
    vfs_fat32_mount_t *mount=&mounted;const char *relative="db";vnode_t *vnode=&node;
''' + acquire + r'''
    return K_OK;
}
BOOLEAN fat32_read_file(OS_FAT32_FILE *file,UINT64 offset,VOID *out,UINT32 size,UINT32 *count) {
    assert(file==&native && file->Used && offset<=file->Size);
    if(size>file->Size-offset)size=(UINT32)(file->Size-offset);
    memcpy(out,media+offset,size);*count=size;return 1;
}
BOOLEAN fat32_write_file(OS_FAT32_FILE *file,UINT64 offset,const VOID *in,UINT32 size,UINT32 *count) {
    assert(file==&native && file->Used && offset+size<=sizeof(media));
    memcpy(media+offset,in,size);*count=size;if(offset+size>file->Size)file->Size=offset+size;
    return 1;
}
BOOLEAN fat32_read_path(OS_FAT32 *filesystem,const CHAR8 *path,UINT64 offset,
                        VOID *out,UINT32 size,UINT32 *count) {
    OS_FAT32_FILE *file=0;if(!fat32_open(filesystem,path,&file))return 0;
    BOOLEAN result=fat32_read_file(file,offset,out,size,count);
    assert(fat32_close(file));return result;
}
BOOLEAN fat32_write_path(OS_FAT32 *filesystem,const CHAR8 *path,UINT64 offset,
                         const VOID *in,UINT32 size,UINT32 *count) {
    OS_FAT32_FILE *file=0;if(!fat32_open(filesystem,path,&file))return 0;
    BOOLEAN result=fat32_write_file(file,offset,in,size,count);
    assert(fat32_close(file));return result;
}
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *path,char *relative) {
    assert(!strcmp(path,"/db"));++routes;strcpy(relative,"db");return &mounted;
}
static kstatus_t vfs_memory_backend_read(void *ctx,uint64_t offset,void *out,size_t size,uint64_t *bytes) {
    (void)ctx;(void)offset;(void)out;(void)size;(void)bytes;++memory_calls;return K_EIO;
}
static kstatus_t vfs_memory_backend_write(void *ctx,uint64_t offset,const void *in,size_t size,uint64_t *bytes) {
    (void)ctx;(void)offset;(void)in;(void)size;(void)bytes;++memory_calls;return K_EIO;
}
''' + rw + r'''
static void reset(vfs_backend_node_t *backend) {
    *backend=(vfs_backend_node_t){.size=4096,.path="/db"};memset(&native,0,sizeof(native));
    opens=closes=releases=drops=routes=fail_mode=memory_calls=0;
}
int main(void) {
    vfs_backend_node_t backend;reset(&backend);uint64_t bytes=0;
    assert(acquire(&backend,false)==K_OK && backend.fat_file==&native);
    for(unsigned i=0;i<2000;++i) {
        unsigned offset=i*13%4096;unsigned char in=(unsigned char)i,out=0;
        assert(vfs_fat32_backend_write(&backend,offset,&in,1,&bytes)==K_OK && bytes==1);
        assert(vfs_fat32_backend_read(&backend,offset,&out,1,&bytes)==K_OK && bytes==1 && out==in);
    }
    assert(opens==1 && !closes && !routes && backend.content_revision==2000);
    unsigned char in=23;
    assert(vfs_fat32_backend_write(&backend,5000,&in,1,&bytes)==K_OK && backend.size==5001);
    backend.detached=true;
    assert(vfs_fat32_backend_read(&backend,0,&in,1,&bytes)==K_EIO);
    assert(vfs_fat32_backend_write(&backend,0,&in,1,&bytes)==K_EIO);
    assert(memory_calls==2 && opens==1 && !routes);
    reset(&backend);fail_mode=1;
    assert(acquire(&backend,false)==K_OK && backend.fat_file==&native && opens==2 && releases==1 && !drops);
    reset(&backend);fail_mode=2;
    assert(acquire(&backend,false)==K_OK && !backend.fat_file && opens==1 && releases==1 && !drops);
    fail_mode=0;unsigned char out=0;
    assert(vfs_fat32_backend_read(&backend,0,&out,1,&bytes)==K_OK && opens==2 && closes==1 && routes==1);
    assert(vfs_fat32_backend_write(&backend,0,&in,1,&bytes)==K_OK && opens==3 && closes==2 && routes==2);
    reset(&backend);fail_mode=2;
    assert(acquire(&backend,true)==K_EIO && drops==1 && !backend.fat_file);
    reset(&backend);backend.directory=true;
    assert(acquire(&backend,false)==K_OK && !opens && !releases);
    puts("Writable FAT handle: 2000 read/write pairs use one open without mount scans; size, detached I/O, idle reclaim, exhaustion fallback and required-read failure passed");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-writable-handle-') as directory:
    path = Path(directory)
    for variant in ('current', 'read_only_retention'):
        code = program if variant == 'current' else program.replace(
            'if (!backend->directory &&', 'if (keep_fat_file && !backend->directory &&', 1)
        (path / 'test.c').write_text(code)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
        result = subprocess.run([str(path/'test')],capture_output=True,text=True)
        if variant == 'current':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0, '读写打开不保留句柄的旧路径未被检出'
            print('负对照通过：仅为只读文件保留句柄会触发回归断言')
