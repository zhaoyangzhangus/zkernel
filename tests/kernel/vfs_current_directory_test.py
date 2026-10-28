"""验证真实用户路径、工作目录同步和文件系统调用的组合行为。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


process = (root / 'kernel/process/process.c').read_text()
core = (root / 'kernel/fs/vfs/core.c').read_text()
syscall = (root / 'kernel/syscall/filesystem.c').read_text()
linux = (root / 'kernel/syscall/linux.c').read_text()
exec_source = (root / 'kernel/process/exec.c').read_text()
program = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/process.h>
#include <kernel/vfs.h>
#include <uapi/linux_abi.h>
#define VFS_PATH_LIMIT 256U
#define LINUX_MAX_PATH 256U
#define LINUX_EFAULT K_EFAULT
#define LINUX_ERANGE (-34LL)
#define EXEC_PATH_CAPACITY 256U
#define OS_DEBUG_SERIAL 0
static process_t process;
static _Thread_local unsigned preempt;
static char actual[256], old_actual[256];
static bool change_during_rename;
static unsigned kernel_open_calls, stat_calls;
static uint32_t actual_open_flags;
process_t *current_process(void) { return &process; }
void sched_preempt_disable(void) { ++preempt; }
void sched_preempt_enable(void) { assert(preempt); --preempt; }
static void process_thread_lock(process_t *p) { spinlock_lock(&p->thread_lock); }
static void process_thread_unlock(process_t *p) { spinlock_unlock(&p->thread_lock); }
kstatus_t copy_from_user(void *out, const void __user *in, size_t size) {
 if (!out || (uintptr_t)in < 4096U) return K_EACCES;
 const char *s = (const char *)in; char *d = out;
 for (size_t i=0;i<size;++i) { d[i]=s[i]; if (!s[i]) { memset(d+i+1,0,size-i-1); break; } }
 return K_OK;
}
kstatus_t copy_to_user(void __user *out, const void *in, size_t size) {
 if ((uintptr_t)out < 4096U || !in) return K_EACCES;
 memcpy(out,in,size); return K_OK;
}
size_t vfs_string_length(const char *s) { return s ? strnlen(s,256) : 0; }
static void *vfs_fat32_mount_for_path(const char *s,char relative[256]) {
 (void)relative;
 return !strncmp(s,"/disk/",6) ? (void *)(uintptr_t)1U : 0;
}
struct entry { const char *path; unsigned type; const char *link; };
static struct entry entries[] = {
 {"/",OS_FILE_TYPE_DIRECTORY,0}, {"/work",OS_FILE_TYPE_DIRECTORY,0},
 {"/work/sub",OS_FILE_TYPE_DIRECTORY,0}, {"/work/old",OS_FILE_TYPE_REGULAR,0},
 {"/work/link",OS_FILE_TYPE_SYMLINK,"/other/sub"},
 {"/work/dangling",OS_FILE_TYPE_SYMLINK,"missing"},
 {"/disk",OS_FILE_TYPE_DIRECTORY,0}, {"/disk/app",OS_FILE_TYPE_DIRECTORY,0},
 {"/disk/app/file",OS_FILE_TYPE_REGULAR,0},
 {"/other",OS_FILE_TYPE_DIRECTORY,0}, {"/other/sub",OS_FILE_TYPE_DIRECTORY,0},
 {"/other/file",OS_FILE_TYPE_REGULAR,0}
};
kstatus_t vfs_stat_kernel(const char *path, os_file_info_t *out) {
 ++stat_calls;
 if (change_during_rename && !strcmp(path,"/work/old")) {
  change_during_rename=false; assert(process_cwd_set(&process,"/other")==K_OK);
 }
 for (unsigned i=0;i<sizeof(entries)/sizeof(entries[0]);++i)
  if (!strcmp(path,entries[i].path)) { memset(out,0,sizeof(*out)); out->type=entries[i].type; return K_OK; }
 return K_ENOENT;
}
kstatus_t vfs_readlink_kernel(const char *path,char *out,size_t cap,size_t *bytes) {
 for (unsigned i=0;i<sizeof(entries)/sizeof(entries[0]);++i)
  if (!strcmp(path,entries[i].path) && entries[i].link) {
   *bytes=strlen(entries[i].link); assert(*bytes<=cap); memcpy(out,entries[i].link,*bytes); return K_OK;
  }
 return K_ENOENT;
}
kstatus_t vfs_open_kernel(const char *p,uint32_t flags,uint32_t mode,file_t **out) {
 (void)mode;++kernel_open_calls;actual_open_flags=flags;
 strcpy(actual,p); *out=(file_t *)(uintptr_t)1U; return K_OK;
}
kstatus_t vfs_enumerate_kernel(const char *p,uint32_t n,os_file_info_t *out) {
 (void)n;(void)out; strcpy(actual,p); return K_OK;
}
kstatus_t vfs_enumerate_kernel_batch(const char *p,uint32_t n,os_file_info_t *out,uint32_t cap,uint32_t *count) {
 (void)n;(void)out;(void)cap; strcpy(actual,p);*count=1;return K_OK;
}
kstatus_t vfs_remove_kernel(const char *p) { strcpy(actual,p);return K_OK; }
kstatus_t vfs_mkdir_kernel(const char *p,uint32_t mode) { (void)mode;strcpy(actual,p);return K_OK; }
kstatus_t vfs_rename_kernel(const char *a,const char *b) { strcpy(old_actual,a);strcpy(actual,b);return K_OK; }
static void linux_copy(void *out,const void *in,size_t size) { memmove(out,in,size); }
static kstatus_t linux_fd_lookup(process_t *p,int64_t fd,linux_fd_entry_t *entry) {
 (void)p;(void)fd;(void)entry;return K_EBADF;
}
static void linux_fd_entry_put(linux_fd_entry_t *entry) { (void)entry; }
kstatus_t vfs_directory_path_kernel(file_t *file,char *path,size_t capacity) {
 (void)file;(void)path;(void)capacity;return K_EBADF;
}
typedef struct { int unused; } nv_frontend_file_t;
typedef struct { int node; } linux_proc_file_t;
typedef struct { int unused; } window_server_window_t;
static uint32_t g_linux_tmp_open_failure_logs;
static void kernel_serial_printf_serial_only(const char *fmt, ...) { (void)fmt; }
static int64_t linux_fd_install(process_t *p,int64_t requested,file_t *f,
                                void *private_data,unsigned kind,uint32_t flags) {
 (void)private_data;(void)flags;assert(p && requested==-1);
 if(kind==LINUX_FD_FILE)assert(f==(file_t *)(uintptr_t)1U);
 return 7;
}
static int nv_frontend_open(bool control,unsigned flags,nv_frontend_file_t **out) {
 (void)control;(void)flags;(void)out;return K_ENOSYS;
}
static void nv_frontend_file_put(nv_frontend_file_t *f) { (void)f; }
static int window_server_lookup_owner(process_t *p,window_server_window_t **out) {
 (void)p;(void)out;return K_ENOSYS;
}
static void window_server_put(window_server_window_t *w) { (void)w; }
static int linux_proc_open_path(process_t *p,const char *s,linux_proc_file_t **out) {
 (void)p;(void)s;(void)out;return K_ENOENT;
}
static void linux_proc_file_put(linux_proc_file_t *p) { (void)p; }
static bool linux_proc_node_is_directory(int node) { (void)node;return false; }
static void linux_file_put(file_t *file) { (void)file; }
''' + '\n'.join(function(process, s) for s in [
    'kstatus_t process_cwd_copy(', 'kstatus_t process_cwd_set(']) + '\n' + \
    function(core, 'bool vfs_path_needs_resolution(') + '\n' + \
    function(core, 'kstatus_t vfs_resolve_path_kernel(') + '\n' + \
    '\n'.join(function(syscall, s) for s in [
        'int64_t syscall_file_chdir(', 'int64_t syscall_file_getcwd(']) + '\n' + \
    '\n'.join(function(linux, s) for s in [
        'static kstatus_t linux_copy_string(', 'static kstatus_t linux_system_config_path(',
        'static kstatus_t linux_path_from_user(', 'static int64_t linux_getcwd(',
        'static int64_t linux_chdir(', 'static int64_t linux_mkdir(',
        'static uint32_t linux_open_flags(', 'static bool linux_exact_path(',
        'static kstatus_t linux_resolve_open_path(', 'static int64_t linux_openat(',
        'static kstatus_t linux_vfs_stat_path(']) + '\n' + \
    '\n'.join(function(exec_source, s) for s in [
        'static kstatus_t exec_copy_user_path(', 'static bool exec_path_is_self_exe(',
        'static kstatus_t exec_resolve_path(']) + r'''
static void *writer(void *unused) {
 (void)unused;
 for (unsigned i=0;i<100000;++i)
  assert(process_cwd_set(&process,(i&1)?"/work/sub":"/other")==K_OK);
 return 0;
}
static void *reader(void *unused) {
 (void)unused; char cwd[PROCESS_CWD_CAPACITY];
 for (unsigned i=0;i<100000;++i) {
  assert(process_cwd_copy(&process,cwd)==K_OK);
  assert(!strcmp(cwd,"/work/sub") || !strcmp(cwd,"/other"));
 }
 return 0;
}
int main(void) {
 spinlock_init(&process.info_lock);spinlock_init(&process.thread_lock);
 assert(process_cwd_set(&process,"/")==K_OK);
 file_t *file=0;os_file_info_t info;char cwd[256];
 assert(!vfs_path_needs_resolution("/disk/app/file"));
 assert(vfs_path_needs_resolution("/disk/app/../file"));
 assert(vfs_path_needs_resolution("/disk/app/file/"));
 assert(vfs_path_needs_resolution("/work/link"));
 unsigned prior_stats=stat_calls;
 assert(linux_vfs_stat_path("/disk/app/file",0,&info)==K_OK);
 assert(info.type==OS_FILE_TYPE_REGULAR && stat_calls-prior_stats==1);
 prior_stats=stat_calls;
 assert(linux_vfs_stat_path("/disk/app/file",LINUX_AT_SYMLINK_NOFOLLOW,&info)==K_OK);
 assert(stat_calls-prior_stats==1);
 assert(linux_vfs_stat_path("/disk/app/file/child",0,&info)==K_ENOTDIR);
 assert(linux_vfs_stat_path("/disk/app/file/",0,&info)==K_ENOTDIR);
 assert(linux_vfs_stat_path("/disk/app/missing",0,&info)==K_ENOENT);
 assert(linux_vfs_stat_path("/work/link",0,&info)==K_OK && info.type==OS_FILE_TYPE_DIRECTORY);
 assert(linux_vfs_stat_path("/work/link",LINUX_AT_SYMLINK_NOFOLLOW,&info)==K_OK && info.type==OS_FILE_TYPE_SYMLINK);
 assert(vfs_open("download.bin",VFS_OPEN_WRITE|VFS_OPEN_CREATE,0,&file)==K_OK);
 assert(!strcmp(actual,"/download.bin"));
 assert(syscall_file_chdir((uintptr_t)"/work",0,0,0,0,0)==K_OK);
 assert(syscall_file_getcwd((uintptr_t)cwd,sizeof(cwd),0,0,0,0)==6 && !strcmp(cwd,"/work"));
 assert(syscall_file_getcwd((uintptr_t)cwd,5,0,0,0,0)==K_ENAMETOOLONG);
 assert(syscall_file_getcwd(1,sizeof(cwd),0,0,0,0)==K_EACCES);
 assert(vfs_open("sub/../output.bin",VFS_OPEN_WRITE|VFS_OPEN_CREATE,0,&file)==K_OK);
 assert(!strcmp(actual,"/work/output.bin"));
 assert(vfs_open("/work/link/../file",VFS_OPEN_READ,0,&file)==K_OK);
 assert(!strcmp(actual,"/other/file"));
 assert(vfs_open("/work/link",VFS_OPEN_READ|VFS_OPEN_DIRECTORY,0,&file)==K_OK);
 assert(!strcmp(actual,"/other/sub"));
 assert(vfs_stat("/work/link",&info)==K_OK && info.type==OS_FILE_TYPE_DIRECTORY);
 assert(vfs_mkdir("/work/link/new",0700)==K_OK && !strcmp(actual,"/other/sub/new"));
 assert(vfs_remove("/work/link")==K_OK && !strcmp(actual,"/work/link"));
 assert(vfs_rename("/work/link","/work/newlink")==K_OK &&
        !strcmp(old_actual,"/work/link") && !strcmp(actual,"/work/newlink"));
 assert(linux_mkdir(&process,LINUX_AT_FDCWD,(uintptr_t)"link/../new",0700)==K_OK);
 assert(!strcmp(actual,"/other/new"));
 char open_path[256];const char *opened=0;
 assert(linux_resolve_open_path("/work/link",0,open_path,&opened)==K_OK &&
        !strcmp(opened,"/other/sub"));
 assert(linux_resolve_open_path("/work/link",LINUX_O_NOFOLLOW,open_path,&opened)==K_ELOOP);
 assert(linux_resolve_open_path("/work/link",LINUX_O_CREAT|LINUX_O_EXCL,
                                open_path,&opened)==K_OK && !strcmp(opened,"/work/link"));
 assert(linux_resolve_open_path("/work/dangling",LINUX_O_CREAT,
                                open_path,&opened)==K_OK && !strcmp(opened,"/work/missing"));
 assert(linux_resolve_open_path("/work/dangling",LINUX_O_NOFOLLOW|LINUX_O_CREAT,
                                open_path,&opened)==K_ELOOP);
 assert(linux_resolve_open_path("/work/link/new",LINUX_O_CREAT,
                                open_path,&opened)==K_OK && !strcmp(opened,"/other/sub/new"));
 assert(linux_resolve_open_path("/disk/app/file",0,open_path,&opened)==K_OK &&
        !strcmp(opened,"/disk/app/file"));
 assert(linux_openat(&process,LINUX_AT_FDCWD,(uintptr_t)"link",LINUX_O_DIRECTORY,0)==7);
 assert(!strcmp(actual,"/other/sub") && (actual_open_flags&VFS_OPEN_DIRECTORY));
 unsigned before_open=kernel_open_calls;
 assert(linux_openat(&process,LINUX_AT_FDCWD,(uintptr_t)"/work/link",LINUX_O_NOFOLLOW,0)==K_ELOOP);
 assert(kernel_open_calls==before_open);
 assert(linux_openat(&process,LINUX_AT_FDCWD,(uintptr_t)"/work/dangling",LINUX_O_CREAT,0600)==7);
 assert(!strcmp(actual,"/work/missing") && (actual_open_flags&VFS_OPEN_CREATE));
 assert(linux_openat(&process,LINUX_AT_FDCWD,(uintptr_t)"/work/dangling",
                     LINUX_O_CREAT|LINUX_O_EXCL,0600)==7);
 assert(!strcmp(actual,"/work/dangling") && (actual_open_flags&VFS_OPEN_EXCLUSIVE));
 assert(vfs_open("/work//sub/../old",VFS_OPEN_READ,0,&file)==K_OK);
 assert(!strcmp(actual,"/work/old"));
 assert(exec_resolve_path(&process,"old",cwd)==K_OK && !strcmp(cwd,"/work/old"));
 assert(exec_resolve_path(&process,"link/../file",cwd)==K_OK && !strcmp(cwd,"/other/file"));
 assert(exec_resolve_path(&process,"",cwd)==K_ENOENT);
 assert(exec_resolve_path(&process,"missing",cwd)==K_ENOENT);
 assert(exec_resolve_path(&process,"/proc/self/exe",cwd)==K_ENOENT);
 strcpy(process.linux_exe,"/work/old");
 assert(exec_resolve_path(&process,"/proc/self/exe",cwd)==K_OK && !strcmp(cwd,"/work/old"));
 assert(vfs_stat("sub",&info)==K_OK && info.type==OS_FILE_TYPE_DIRECTORY);
 assert(vfs_enumerate("sub",0,&info)==K_OK && !strcmp(actual,"/work/sub"));
 assert(vfs_mkdir("newdir",0)==K_OK && !strcmp(actual,"/work/newdir"));
 assert(vfs_remove("old")==K_OK && !strcmp(actual,"/work/old"));
 change_during_rename=true;
 assert(vfs_rename("old","new")==K_OK);
 assert(!strcmp(old_actual,"/work/old") && !strcmp(actual,"/work/new"));
 assert(vfs_change_directory("/work/link/..")==K_OK);
 assert(process_cwd_copy(&process,cwd)==K_OK && !strcmp(cwd,"/other"));
 assert(vfs_change_directory("file")==K_ENOTDIR);
 assert(vfs_change_directory("missing")==K_ENOENT);
 assert(process_cwd_copy(&process,cwd)==K_OK && !strcmp(cwd,"/other"));
 assert(vfs_open("file",VFS_OPEN_READ,0,&file)==K_OK && !strcmp(actual,"/other/file"));
 assert(vfs_open((const char *)(uintptr_t)1,VFS_OPEN_READ,0,&file)==K_EACCES);
 assert(linux_getcwd(&process,(uintptr_t)cwd,sizeof(cwd))==7 && !strcmp(cwd,"/other"));
 assert(linux_getcwd(&process,(uintptr_t)cwd,0)==LINUX_ERANGE);
 assert(linux_getcwd(&process,(uintptr_t)cwd,6)==LINUX_ERANGE);
 assert(linux_getcwd(&process,0,sizeof(cwd))==LINUX_EFAULT);
 assert(linux_chdir(&process,(uintptr_t)"/work/link/..")==K_OK);
 assert(syscall_file_getcwd((uintptr_t)cwd,sizeof(cwd),0,0,0,0)==7 && !strcmp(cwd,"/other"));
 assert(linux_chdir(&process,(uintptr_t)"/work/sub/..")==K_OK);
 assert(syscall_file_getcwd((uintptr_t)cwd,sizeof(cwd),0,0,0,0)==6 && !strcmp(cwd,"/work"));
 assert(linux_chdir(&process,(uintptr_t)"old")==K_ENOTDIR);
 assert(process_cwd_set(&process,"/other")==K_OK);
 char long_path[255];memset(long_path,'x',sizeof(long_path)-1);long_path[sizeof(long_path)-1]=0;
 assert(vfs_open(long_path,VFS_OPEN_READ,0,&file)==K_ENAMETOOLONG);
 process_t child={0};spinlock_init(&child.info_lock);
 assert(process_cwd_copy(&process,child.cwd)==K_OK);
 assert(process_cwd_set(&process,"/work/sub")==K_OK && !strcmp(child.cwd,"/other"));
 pthread_t a,b;assert(!pthread_create(&a,0,writer,0));assert(!pthread_create(&b,0,reader,0));
 assert(!pthread_join(a,0));assert(!pthread_join(b,0));
 puts("PASS: native/Linux cwd, real openat and mkdir routing, symlink and nofollow semantics, rename race and concurrent updates");
}
'''

with tempfile.TemporaryDirectory(prefix='vfs-cwd-') as directory:
    tmp = Path(directory)
    (tmp / 'test.c').write_text(program)
    command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-pthread', '-fsanitize=undefined', '-I' + str(root / 'include'),
               '-I' + str(root / 'kernel/fs/vfs'), str(tmp / 'test.c')]
    subprocess.run(command + [str(root / 'kernel/fs/vfs/user_api.c'),
                              '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)
    # 恢复旧的绝对路径直通行为，确保测试能识别链接被绕过的回退。
    source = (root / 'kernel/fs/vfs/user_api.c').read_text()
    old = '(resolve_absolute || vfs_path_needs_resolution(input))'
    assert source.count(old) == 1
    previous = source.replace(old, '(resolve_absolute || old_lexical_check(input))')
    previous = previous.replace('static kstatus_t vfs_user_path(', '''
static bool old_lexical_check(const char *path) {
    for (size_t i=0;path[i];++i)
        if (path[i]=='/' && (path[i+1]=='/' || path[i+1]=='.')) return true;
    return false;
}
static kstatus_t vfs_user_path(''', 1)
    (tmp / 'old_user_api.c').write_text(previous)
    subprocess.run(command + [str(tmp / 'old_user_api.c'),
                              '-o', str(tmp / 'old-test')], check=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    rejected = subprocess.run([str(tmp / 'old-test')], capture_output=True)
    assert rejected.returncode and b'/other/sub' in rejected.stderr, rejected.stderr
    print('PASS: negative control detects absolute symlink resolution regression')
    # 回到逐级查询时，成功的普通磁盘路径不再满足一次元数据查询的开销约束。
    fast = 'if (!vfs_path_needs_resolution(path)) {\n        kstatus_t fast = vfs_stat_kernel(path, info);'
    assert program.count(fast) == 1
    (tmp / 'slow-stat.c').write_text(program.replace(fast,
        'if (false) {\n        kstatus_t fast = vfs_stat_kernel(path, info);'))
    slower = command.copy()
    slower[-1] = str(tmp / 'slow-stat.c')
    subprocess.run(slower + [str(root / 'kernel/fs/vfs/user_api.c'),
                             '-o', str(tmp / 'slow-stat')], check=True)
    rejected = subprocess.run([str(tmp / 'slow-stat')], capture_output=True)
    assert rejected.returncode and b'stat_calls-prior_stats==1' in rejected.stderr, rejected.stderr
    print('PASS: negative control detects repeated directory metadata queries')
