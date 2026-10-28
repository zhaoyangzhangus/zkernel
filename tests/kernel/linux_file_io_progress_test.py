"""Linux 文件读写在部分完成后遇到错误时必须报告已完成字节。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
copy_status = source[source.index('static int64_t linux_copy_status('):source.index('static bool linux_fd_valid(')]
read_start = source.index('    uint64_t bytes = 0U;\n    status = vfs_read(entry.file,')
read_end = source.index('\n}', read_start)
write_start = source.index('    uint64_t bytes = 0U;\n    status = vfs_write(entry.file,')
write_end = source.index('\n}', write_start)
positioned_start = source.index('static int64_t linux_positioned_io(')
positioned_end = source.index('\nstatic int64_t linux_openat(', positioned_start)
program = r'''
#include <assert.h>
/* 单句柄测试只桩化偏移锁；共享句柄并发由专门的回归测试覆盖。 */
#define vfs_file_position_init(file) ((void)(file))
#define vfs_file_position_lock(file) ((void)(file), K_OK)
#define vfs_file_position_unlock(file) ((void)(file))
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define __user
#define K_OK 0
#define K_EACCES -13
#define K_EIO -5
#define K_EINVAL -22
#define K_ESPIPE -29
#define LINUX_EFAULT -14
#define LINUX_FD_FILE 1
#define KOBJECT_TYPE_FILE 1
#define VFS_OPEN_APPEND 32
#define VFS_OPEN_WRITE 2
#define VFS_OPEN_READ 1
typedef int kstatus_t;
typedef struct {int dummy;} process_t;
typedef struct {struct {unsigned type;} object; void *vnode,*ops,*private_data;
 uint64_t position;unsigned flags,rights;} file_t;
typedef struct {int kind;file_t *file;} linux_fd_entry_t;
static file_t file;
static int requested_status,holds,releases,ops,lookup_status,kind=LINUX_FD_FILE;
static uint64_t reported,expected_offset;
static bool positioned;
static int linux_fd_lookup(process_t *p,int64_t fd,linux_fd_entry_t *entry){
 (void)p;(void)fd;if(lookup_status)return lookup_status;
 ++holds;*entry=(linux_fd_entry_t){kind,&file};return K_OK;
}
static void linux_fd_entry_put(linux_fd_entry_t *entry){(void)entry;assert(holds>0);--holds;++releases;}
static int transfer(file_t *f,void *buffer,size_t len,uint64_t *bytes){
 (void)buffer;assert(holds&&reported<=len);++ops;
 if(positioned){assert(f!=&file&&f->position==expected_offset&&f->vnode==file.vnode);}
 else assert(f==&file);
 f->position+=reported;*bytes=reported;return requested_status;
}
static int vfs_read(file_t *f,void *b,size_t n,uint64_t *v){return transfer(f,b,n,v);}
static int vfs_write(file_t *f,const void *b,size_t n,uint64_t *v){return transfer(f,(void *)b,n,v);}
''' + copy_status + '''
static int64_t read_tail(linux_fd_entry_t entry,uint64_t buffer,uint64_t length){
 kstatus_t status;
''' + source[read_start:read_end] + '''
}
static int64_t write_tail(linux_fd_entry_t entry,uint64_t buffer,uint64_t length){
 kstatus_t status;
''' + source[write_start:write_end] + '\n}\n' + source[positioned_start:positioned_end] + r'''
int main(void){
 file=(file_t){.vnode=(void*)3,.position=19,.rights=VFS_OPEN_READ|VFS_OPEN_WRITE};
 for(unsigned mode=0;mode<4;mode++)for(unsigned count=0;count<3;count++)for(unsigned kindio=0;kindio<4;kindio++){
  requested_status=mode==0?K_OK:mode==1?K_EIO:mode==2?K_EACCES:K_EINVAL;
  reported=count==0?0:count==1?1:4096;holds=releases=ops=0;lookup_status=0;kind=LINUX_FD_FILE;
  file.position=19;positioned=kindio>=2;expected_offset=41;
  int64_t expected=reported?(int64_t)reported:requested_status==K_EACCES?LINUX_EFAULT:requested_status;
  int64_t result;
  if(positioned)result=linux_positioned_io(0,7,0,8192,41,kindio==3);
  else {linux_fd_entry_t entry={LINUX_FD_FILE,&file};holds=1;
   result=kindio==0?read_tail(entry,0,8192):write_tail(entry,0,8192);}
  assert(result==expected&&holds==0&&releases==1&&ops==1);
  assert(file.position==(positioned?19:19+reported));
 }
 lookup_status=K_EIO;holds=releases=ops=0;
 assert(linux_positioned_io(0,7,0,8192,41,true)==K_EIO&&!holds&&!releases&&!ops);
 lookup_status=0;kind=2;
 assert(linux_positioned_io(0,7,0,8192,41,true)==K_ESPIPE&&!holds&&releases==1&&!ops);
 assert(linux_positioned_io(0,7,0,8192,-1,true)==K_EINVAL&&!holds&&releases==1&&!ops);
 puts("Linux file I/O: partial reads/writes and positioned I/O report committed bytes; zero-progress errors, offsets and references PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='linux-file-progress-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=program,text=True,check=True)
    subprocess.run([binary],check=True)
