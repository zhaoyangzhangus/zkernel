#!/usr/bin/env python3
"""Exercise the actual Linux positioned-I/O helper with interleaved calls."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
text = (root / 'kernel/syscall/linux.c').read_text()
start = text.index('static int64_t linux_positioned_io(')
helper = text[start:text.index('\nstatic int64_t linux_openat(', start)]
harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
/* 单句柄测试只桩化偏移锁；共享句柄并发由专门的回归测试覆盖。 */
#define vfs_file_position_init(file) ((file)->position_initialized=1)
#define vfs_file_position_lock(file) ((void)(file), K_OK)
#define vfs_file_position_unlock(file) ((void)(file))
#define __user
#define K_OK 0
#define K_EINVAL -22
#define K_ESPIPE -29
#define K_EBADF -9
#define KOBJECT_TYPE_FILE 1
#define LINUX_FD_FILE 1
typedef int kstatus_t;
typedef struct { int unused; } process_t;
typedef struct { struct { int type; } object; void *vnode,*ops;
 uint64_t position; unsigned flags,rights; void *private_data; int position_initialized; } file_t;
typedef struct { int kind; file_t *file; } linux_fd_entry_t;
static file_t shared = {.position=777};
static int puts,interleave,kind=LINUX_FD_FILE;
static int64_t linux_positioned_io(process_t*,int64_t,uint64_t,uint64_t,int64_t,bool);
static int linux_fd_lookup(process_t*p,int64_t fd,linux_fd_entry_t*e) {
 (void)p; if(fd<0)return K_EBADF; e->kind=kind;e->file=&shared;return 0;
}
static void linux_fd_entry_put(linux_fd_entry_t*e){(void)e;++puts;}
static int linux_copy_status(int s){return s;}
static int io(file_t*f,uint64_t*bytes) {
 uint64_t own=f->position;
 assert(f!=&shared && shared.position==777 && f->position_initialized);
 if(!interleave){interleave=1;assert(linux_positioned_io(0,3,0,8,99,true)==8);}
 assert(f->position==own && shared.position==777);
 f->position+=8;*bytes=8;return 0;
}
static int vfs_read(file_t*f,void*b,size_t n,uint64_t*out){(void)b;(void)n;return io(f,out);}
static int vfs_write(file_t*f,const void*b,size_t n,uint64_t*out){(void)b;(void)n;return io(f,out);}
'''
main = r'''
int main(void) {
 assert(linux_positioned_io(0,3,0,8,10,false)==8);
 assert(shared.position==777 && puts==2);
 assert(linux_positioned_io(0,3,0,8,-1,false)==K_EINVAL);
 assert(linux_positioned_io(0,3,0,UINT64_MAX,0,false)==K_EINVAL);
 assert(linux_positioned_io(0,-1,0,8,0,false)==K_EBADF);
 kind=2;assert(linux_positioned_io(0,3,0,8,0,false)==K_ESPIPE);
 assert(puts==3 && shared.position==777);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-positioned-io-') as directory:
    source = pathlib.Path(directory) / 'test.c'
    binary = pathlib.Path(directory) / 'test'
    source.write_text(harness + helper + main)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('positioned I/O: interleaving preserves independent offsets; invalid arguments and descriptor cleanup passed')
