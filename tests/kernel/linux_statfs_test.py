"""Execute the real RAM-space queries and Linux statfs ABI frontends."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
linux = (root / 'kernel/syscall/linux.c').read_text()
vfs = (root / 'kernel/fs/vfs/core.c').read_text()

def between(source, first, last):
    start = source.index(first)
    return source[start:source.index(last, start)]

functions = between(vfs, 'static bool vfs_backend_memory_space(',
                    'kstatus_t vfs_directory_path_kernel(')
functions += between(vfs, 'kstatus_t vfs_space_kernel(',
                     'kstatus_t vfs_remove_typed_kernel(')
functions += between(linux, 'static int64_t linux_statfs_copy(',
                     'static kstatus_t linux_vfs_stat_path(')
dispatch = between(linux, '    case LINUX_SYS_STATFS:',
                   '    case LINUX_SYS_NEWFSTATAT:')
program = r'''
#include <assert.h>
#include <string.h>
#include <sys/vfs.h>
#include <kernel/vfs.h>
#include <uapi/linux_abi.h>
#define LINUX_MAX_PATH 256
#define VFS_PATH_LIMIT 256
typedef int process_t;
typedef struct {bool fat32_node; void *read,*write,*truncate,*fsync; OS_FAT32 *fat_filesystem;} vfs_backend_node_t;
typedef struct {OS_FAT32 *filesystem;} vfs_fat32_mount_t;
static OS_FAT32 disk;
static vfs_fat32_mount_t mount = {&disk};
static bool disk_failure;
BOOLEAN fat32_space(OS_FAT32 *fs, UINT64 *blocks, UINT64 *free_blocks, UINT64 *block_size) {
 assert(fs==&disk);if(disk_failure)return 0;
 *blocks=1000;*free_blocks=300;*block_size=4096;return 1;
}
typedef struct {file_t *file;} linux_fd_entry_t;
static vfs_backend_node_t backend;
static vnode_t node;
static file_t file;
static bool mounted, implicit;
static int locks, resolution_error, stat_error, memory_error, path_error;
static int lookup_error, puts, queries, copies;
static uint64_t total=1048576, available=524288;
static void vfs_initialize(void) {}
static void vfs_lock(void) {assert(!locks);++locks;}
static void vfs_unlock(void) {assert(locks==1);--locks;}
static kstatus_t page_memory_get_stats(uint64_t *t,uint64_t *a) {
 assert(!locks);++queries;*t=total;*a=available;return memory_error;
}
kstatus_t vfs_resolve_path_kernel(const char *p,bool follow,bool missing,char out[256]) {
 assert(follow && !missing);if(resolution_error)return resolution_error;
 strcpy(out,p);return K_OK;
}
kstatus_t vfs_stat_kernel(const char *p,os_file_info_t *i) {
 (void)p;(void)i;return stat_error;
}
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *p,char *relative) {
 (void)p;(void)relative;return mounted?&mount:0;
}
static vnode_t *vfs_find_node_locked(const char *p) {
 (void)p;assert(locks);return implicit?0:&node;
}
static bool vfs_volatile_path(const char *p) {
 return !strcmp(p,"/tmp") || !strncmp(p,"/tmp/",5);
}
static kstatus_t linux_path_from_user(process_t *p,int64_t fd,uint64_t ptr,char *out) {
 assert(p && fd==LINUX_AT_FDCWD);if(path_error)return path_error;
 strcpy(out,(const char*)(uintptr_t)ptr);return K_OK;
}
static kstatus_t linux_fd_lookup(process_t *p,int64_t fd,linux_fd_entry_t *entry) {
 assert(p && fd==7);if(lookup_error)return lookup_error;
 entry->file=&file;return K_OK;
}
static void linux_fd_entry_put(linux_fd_entry_t *e) {(void)e;++puts;}
static void linux_zero(void *p,size_t n) {memset(p,0,n);}
static kstatus_t copy_to_user(void *out,const void *in,size_t n) {
 assert(n==120);++copies;if(!out)return K_EACCES;memcpy(out,in,n);return K_OK;
}
static int64_t linux_copy_status(kstatus_t s) {return s==K_EACCES?K_EFAULT:s;}
''' + functions + r'''
typedef struct {uint64_t rdi,rsi;} frame_t;
static int64_t dispatch(unsigned number,process_t *process,frame_t *frame) {
 switch(number) {
''' + dispatch + r'''
 default:return K_ENOSYS;
 }
}
int main(void) {
 _Static_assert(sizeof(struct statfs)==sizeof(linux_statfs_t),"host ABI size");
 _Static_assert(offsetof(struct statfs,f_namelen)==offsetof(linux_statfs_t,name_max),"host ABI offset");
 _Static_assert(offsetof(struct statfs,f_flags)==offsetof(linux_statfs_t,flags),"host ABI flags");
 process_t process=1;vfs_space_info_t space;
 node.fs_private=&backend;file.vnode=&node;file.object.type=KOBJECT_TYPE_FILE;
 struct {linux_statfs_t stat;unsigned char guard[16];} out;
 memset(&out,0xa5,sizeof(out));
 frame_t frame={(uintptr_t)"/tmp/chrome-data/Default",(uintptr_t)&out.stat};
 assert(LINUX_SYS_STATFS==137 && LINUX_SYS_FSTATFS==138);
 assert(dispatch(LINUX_SYS_STATFS,&process,&frame)==K_OK);
 assert(out.stat.type==0x01021994 && out.stat.block_size==PAGE_SIZE);
 assert(out.stat.fragment_size==PAGE_SIZE && out.stat.blocks==total);
 assert(out.stat.blocks_free==available && out.stat.blocks_available==available);
 assert(out.stat.name_max==255 && out.stat.files==0 && out.stat.files_free==0);
 assert(out.stat.flags==0 && out.stat.filesystem_id[0]==1 && out.stat.filesystem_id[1]==0);
 for(unsigned i=0;i<4;++i)assert(out.stat.spare[i]==0);
 for(unsigned i=0;i<16;++i)assert(out.guard[i]==0xa5);
 frame.rdi=7;available=123;
 assert(dispatch(LINUX_SYS_FSTATFS,&process,&frame)==K_OK && puts==1);
 assert(out.stat.blocks_free==123);
 assert(linux_fstatfs(&process,7,0)==K_EFAULT && puts==2);
 lookup_error=K_EBADF;assert(linux_fstatfs(&process,7,frame.rsi)==K_EBADF && puts==2);
 lookup_error=0;backend.fat32_node=true;
 backend.fat_filesystem=&disk;
 assert(linux_fstatfs(&process,7,frame.rsi)==K_OK && puts==3);
 assert(out.stat.type==0x4d44 && out.stat.blocks==1000 && out.stat.blocks_free==300);
 assert(out.stat.block_size==4096);
 disk_failure=true;assert(linux_fstatfs(&process,7,frame.rsi)==K_EIO && puts==4);
 disk_failure=false;
 backend.fat32_node=false;backend.read=&file;
 assert(vfs_file_space(&file,&space)==K_ENOSYS);backend.read=0;
 assert(vfs_file_space(0,&space)==K_EBADF);
 assert(vfs_file_space(&file,0)==K_EINVAL);
 assert(vfs_space_kernel("/tmp",0)==K_EINVAL);
 int before=queries;
 mounted=true;assert(vfs_space_kernel("/disk",&space)==K_OK && queries==before);
 assert(space.type==0x4d44 && space.blocks_free==300 && space.blocks==1000);
 disk_failure=true;assert(vfs_space_kernel("/disk",&space)==K_EIO);disk_failure=false;
 mounted=false;implicit=true;
 assert(vfs_space_kernel("/tmp",&space)==K_OK);
 assert(vfs_space_kernel("/tmp-other",&space)==K_ENOSYS);implicit=false;
 resolution_error=K_ELOOP;assert(vfs_space_kernel("/tmp/link",&space)==K_ELOOP);
 resolution_error=0;stat_error=K_ENOENT;
 assert(vfs_space_kernel("/tmp/missing",&space)==K_ENOENT);stat_error=0;
 path_error=K_EFAULT;assert(linux_statfs(&process,0,frame.rsi)==K_EFAULT);path_error=0;
 memory_error=K_EIO;assert(vfs_file_space(&file,&space)==K_EIO);memory_error=0;
 available=total+1;assert(vfs_file_space(&file,&space)==K_EOVERFLOW);
 total=UINT64_MAX/PAGE_SIZE+1;available=0;
 assert(vfs_file_space(&file,&space)==K_EOVERFLOW);
 assert(!locks && copies==4);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-statfs-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), str(base / 'test.c'),
                    '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True)
print('statfs: real RAM accounting, native Linux ABI, dispatch, fd references, '
      'FAT path/fd queries, unsupported backends, error propagation and lock boundaries passed')
