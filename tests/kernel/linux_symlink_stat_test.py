"""Execute stat/lstat routing and VFS link metadata translation."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
begin = source.index('static kstatus_t linux_vfs_stat_path(')
helpers = source[begin:source.index('static int64_t linux_stat_path(', begin)]
begin = source.index('    case LINUX_SYS_STAT:')
dispatch = source[begin:source.index('    case LINUX_SYS_FSTAT:', begin)]
for name, end in (('linux_stat_path', 'linux_statx'), ('linux_statx', 'linux_lseek')):
    body = source[source.index('static int64_t '+name+'('):source.index('static int64_t '+end+'(')]
    assert 'linux_vfs_stat_path(path, flags, &info)' in body
    assert 'linux_vfs_info_mode(&info)' in body
program = r'''
#include <assert.h>
#include <string.h>
#include <kernel/base.h>
#include <uapi/file.h>
#include <uapi/linux_abi.h>
#define LINUX_MAX_PATH 256
static bool followed;
static int resolution_error, metadata_error, stats;
static bool vfs_path_needs_resolution(const char *path) { (void)path;return true; }
static kstatus_t vfs_resolve_path_kernel(const char *path,bool follow,bool missing,char *out) {
 assert(!missing && !strcmp(path,"/tmp/link"));followed=follow;
 if(resolution_error)return resolution_error;
 strcpy(out,follow?"/tmp/target":"/tmp/link");return K_OK;
}
static kstatus_t vfs_stat_kernel(const char *path,os_file_info_t *info) {
 ++stats;if(metadata_error)return metadata_error;
 bool link=!strcmp(path,"/tmp/link");
 *info=(os_file_info_t){.type=link?OS_FILE_TYPE_SYMLINK:OS_FILE_TYPE_DIRECTORY,
                      .mode=link?0120777:0040700,.size=link?6:0};return K_OK;
}
''' + helpers + r'''
typedef int process_t;
typedef struct {uint64_t rdi,rsi;} arch_trap_frame_t;
static int64_t linux_stat_path(process_t *p,int64_t fd,uint64_t path,uint64_t flags,uint64_t out) {
 assert(p && fd==LINUX_AT_FDCWD && path==123 && out==456);return (int64_t)flags;
}
static int64_t dispatch(uint64_t number,process_t *process,arch_trap_frame_t *frame) {
 switch(number) {
''' + dispatch + r'''
 default:return K_ENOSYS;
 }
}
int main(void) {
 os_file_info_t info;
 assert(linux_vfs_stat_path("/tmp/link",0,&info)==K_OK && followed);
 assert(linux_vfs_info_mode(&info)==0040700);
 assert(linux_vfs_stat_path("/tmp/link",LINUX_AT_SYMLINK_NOFOLLOW,&info)==K_OK && !followed);
 assert(linux_vfs_info_mode(&info)==0120777 && info.size==6);
 resolution_error=K_ELOOP;int before=stats;
 assert(linux_vfs_stat_path("/tmp/link",0,&info)==K_ELOOP && stats==before);
 resolution_error=0;metadata_error=K_ENOENT;
 assert(linux_vfs_stat_path("/tmp/link",0,&info)==K_ENOENT);
 process_t process=1;arch_trap_frame_t frame={123,456};
 assert(dispatch(LINUX_SYS_STAT,&process,&frame)==0);
 assert(dispatch(LINUX_SYS_LSTAT,&process,&frame)==LINUX_AT_SYMLINK_NOFOLLOW);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-link-stat-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'include'), str(base/'test.c'), '-o', str(base/'test')], check=True)
    subprocess.run([str(base/'test')], check=True)
print('Linux stat/lstat/statx: actual follow/nofollow dispatch, symlink mode/size, '
      'directory permissions and resolution/metadata failures passed')
