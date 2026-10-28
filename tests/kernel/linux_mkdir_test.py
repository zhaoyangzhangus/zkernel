"""Execute production Linux directory creation and syscall argument routing."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
path = source[source.index('static kstatus_t linux_system_config_path('):
              source.index('static uint32_t linux_open_flags(')]
mkdir = source[source.index('static int64_t linux_mkdir('):
               source.index('static int64_t linux_symlink(')]
begin = source.index('    case LINUX_SYS_MKDIR:')
end = source.index('    case LINUX_SYS_', source.index('case LINUX_SYS_MKDIRAT:', begin) + 24)
dispatch = source[begin:end]
program = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <string.h>
#include <kernel/base.h>
#include <uapi/linux_abi.h>
#define LINUX_MAX_PATH 256
#define LINUX_EFAULT K_EFAULT
typedef struct { char cwd[256]; } process_t;
static kstatus_t process_cwd_copy(process_t *p,char *out) {
    if(!p || !out)return K_EINVAL;
    memcpy(out,p->cwd,sizeof(p->cwd));return K_OK;
}
typedef struct { uint64_t rax, rdi, rsi, rdx; } arch_trap_frame_t;
static char last_path[256];
static uint32_t last_mode;
static int calls, result;
#define LINUX_FD_FILE 1
typedef struct {int kind;void *file;} linux_fd_entry_t;
static unsigned fd_gets,fd_puts;
static kstatus_t directory_error;
static kstatus_t linux_fd_lookup(process_t *p,int64_t fd,linux_fd_entry_t *e) {
    assert(p);
    if(fd!=42 && fd!=43)return K_EBADF;
    ++fd_gets;e->kind=fd==42?LINUX_FD_FILE:9;e->file=(void*)1;return K_OK;
}
static void linux_fd_entry_put(linux_fd_entry_t *e){assert(e->file);++fd_puts;}
static kstatus_t vfs_directory_path_kernel(void *file,char *out,size_t size){
    assert(file && size==256);
    if(directory_error)return directory_error;
    strcpy(out,"/tmp/renamed-directory");return K_OK;
}
static void linux_copy(void *out, const void *in, size_t length) {
    memmove(out, in, length);
}
static kstatus_t linux_copy_string(uint64_t pointer, char *out, size_t size) {
    if (pointer <= 1) return K_EFAULT;
    const char *input = (const char *)(uintptr_t)pointer;
    size_t length = strnlen(input, size);
    if (length == size) return K_ENAMETOOLONG;
    memcpy(out, input, length + 1);
    return K_OK;
}
static kstatus_t vfs_mkdir_kernel(const char *path, uint32_t mode) {
    ++calls;
    strcpy(last_path, path);
    last_mode = mode;
    return result;
}
static bool vfs_path_needs_resolution(const char *path) { (void)path; return false; }
static kstatus_t vfs_resolve_path_kernel(const char *path, bool follow,
                                         bool missing, char out[256]) {
    (void)path;(void)follow;(void)missing;(void)out;return K_ENOSYS;
}
''' + path + mkdir + r'''
static int64_t dispatch(process_t *process, arch_trap_frame_t *frame) {
    switch (frame->rax) {
''' + dispatch + r'''
    default: return K_ENOSYS;
    }
}
int main(void) {
    process_t process = {"/tmp"};
    char config[256];
    assert(linux_path_from_user(&process,LINUX_AT_FDCWD,(uintptr_t)"/etc/resolv.conf",config)==K_OK);
    assert(!strcmp(config,"/EFI/OS/resolv.conf"));
    assert(linux_path_from_user(&process,LINUX_AT_FDCWD,(uintptr_t)"/etc",config)==K_OK);
    assert(!strcmp(config,"/EFI/OS"));
    assert(linux_path_from_user(&process,LINUX_AT_FDCWD,(uintptr_t)"/etcetera",config)==K_OK);
    assert(!strcmp(config,"/etcetera"));
    char oversized[256];memset(oversized,'a',255);oversized[255]=0;memcpy(oversized,"/etc/",5);
    assert(linux_path_from_user(&process,LINUX_AT_FDCWD,(uintptr_t)oversized,config)==K_ENAMETOOLONG);
    assert(LINUX_SYS_MKDIR == 83 && LINUX_SYS_MKDIRAT == 258);
    assert(linux_mkdir(&process, LINUX_AT_FDCWD, (uintptr_t)"profile", 0700) == K_OK);
    assert(calls == 1 && !strcmp(last_path, "/tmp/profile") && last_mode == 0040700);
    assert(linux_mkdir(&process, 99, (uintptr_t)"/tmp/profile///", 0) == K_OK);
    assert(calls == 2 && !strcmp(last_path, "/tmp/profile") && last_mode == 0040000);
    assert(linux_mkdir(&process, LINUX_AT_FDCWD, (uintptr_t)"/tmp/a", UINT64_MAX) == K_OK);
    assert(last_mode == 0040777);
    int before = calls;
    assert(linux_mkdir(0, LINUX_AT_FDCWD, (uintptr_t)"/tmp/a", 0700) == K_EPERM);
    assert(linux_mkdir(&process, LINUX_AT_FDCWD, 0, 0700) == K_EFAULT);
    assert(linux_mkdir(&process, LINUX_AT_FDCWD, 1, 0700) == K_EFAULT);
    assert(linux_mkdir(&process, LINUX_AT_FDCWD, (uintptr_t)"", 0700) == K_ENOENT);
    assert(linux_mkdir(&process, LINUX_AT_FDCWD, (uintptr_t)"///", 0700) == K_EEXIST);
    assert(linux_mkdir(&process, 99, (uintptr_t)"relative", 0700) == K_EBADF);
    char long_path[300]; memset(long_path, 'a', sizeof(long_path)); long_path[299] = 0;
    assert(linux_mkdir(&process, LINUX_AT_FDCWD, (uintptr_t)long_path, 0700) == K_ENAMETOOLONG);
    assert(calls == before);
    assert(linux_mkdir(&process,42,(uintptr_t)"relative",0700)==K_OK);
    assert(!strcmp(last_path,"/tmp/renamed-directory/relative") && fd_gets==fd_puts);
    assert(linux_mkdir(&process,43,(uintptr_t)"relative",0700)==K_ENOTDIR && fd_gets==fd_puts);
    directory_error=K_ENOENT;
    assert(linux_mkdir(&process,42,(uintptr_t)"relative",0700)==K_ENOENT && fd_gets==fd_puts);
    directory_error=0;
    result = K_EEXIST;
    assert(linux_mkdir(&process, LINUX_AT_FDCWD, (uintptr_t)"/tmp/a", 0700) == K_EEXIST);
    result = K_ENOMEM;
    assert(linux_mkdir(&process, LINUX_AT_FDCWD, (uintptr_t)"/tmp/b", 0700) == K_ENOMEM);
    result = K_OK;
    arch_trap_frame_t frame = {LINUX_SYS_MKDIR, (uintptr_t)"/tmp/c", 0750, 0};
    assert(dispatch(&process, &frame) == K_OK && last_mode == 0040750);
    frame = (arch_trap_frame_t){LINUX_SYS_MKDIRAT, (uint64_t)LINUX_AT_FDCWD,
                               (uintptr_t)"relative", 0710};
    assert(dispatch(&process, &frame) == K_OK && last_mode == 0040710);
    assert(!strcmp(last_path, "/tmp/relative"));
}
'''
with tempfile.TemporaryDirectory(prefix='linux-mkdir-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'include'), str(base/'test.c'), '-o', str(base/'test')],
                   check=True)
    subprocess.run([str(base/'test')], check=True)
print('Linux mkdir: real path resolution, syscall routing, trailing separators, '
      'zero permissions, existing root, invalid pointers/paths and VFS failures passed')
