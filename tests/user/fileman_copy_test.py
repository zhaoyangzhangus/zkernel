"""在真实目录上验证复制引擎的错误、取消和不覆盖语义。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
static unsigned mode,reads,writes;
static atomic_bool *cancel_flag;
static ssize_t copy_read(int fd,void *data,size_t n){
 ++reads;
 if(mode==1&&reads==1){errno=EINTR;return -1;}
 if(mode==6&&reads==2){errno=EIO;return -1;}
 return read(fd,data,n);
}
static ssize_t copy_write(int fd,const void *data,size_t n){
 ++writes;
 if(mode==1&&writes==1){errno=EINTR;return -1;}
 if(mode==1&&n>3)n=3;
 if(mode==2)return 0;
 if(mode==3){errno=ENOSPC;return -1;}
 ssize_t result=write(fd,data,n);
 if(mode==4)atomic_store(cancel_flag,true);
 return result;
}
static int copy_fsync(int fd){if(mode==5){errno=EIO;return -1;}return fsync(fd);}
/* 模拟本机 realpath 保留 FAT 路径大小写，而不是返回磁盘上的规范大小写。 */
static char *copy_realpath(const char *path,char *resolved){
 char *result=realpath(path,resolved);
 if(result&&mode==8){
  char *part=strstr(result,"/tree/nested");
  if(part)memcpy(part+1,"TREE",4);
 }
 return result;
}
#define read copy_read
#define write copy_write
#define fsync copy_fsync
#define realpath copy_realpath
#include "user/desktop/fileman/file_copy.h"
#undef read
#undef write
#undef fsync
#undef realpath
static unsigned descriptors(void){
 DIR *d=opendir("/proc/self/fd");assert(d);unsigned n=0;
 while(readdir(d))++n;
 closedir(d);return n;
}
static int run(unsigned m,const char *source,const char *dest,uint64_t *bytes){
 fm_copy_context_t c={0};cancel_flag=&c.cancel;mode=m;reads=writes=0;
 if(m==7)atomic_store(&c.cancel,true);
 unsigned before=descriptors();int error=fm_copy_run(&c,source,dest);
 assert(before==descriptors()&&!c.buffer);*bytes=atomic_load(&c.bytes);return error;
}
int main(int argc,char **argv){
 assert(argc==2);char source[256],dest[256],path[256];uint64_t bytes;
 assert(!fm_copy_child_path(source,argv[1],"source.bin"));
 assert(!fm_copy_child_path(dest,argv[1],"short.bin"));
 assert(run(1,source,dest,&bytes)==0&&bytes==150000);
 assert(!fm_copy_child_path(dest,argv[1],"existing.bin"));
 assert(run(0,source,dest,&bytes)==EEXIST&&bytes==0);
 unsigned expected[]={0,0,EIO,ENOSPC,ECANCELED,EIO,EIO,ECANCELED};
 for(unsigned m=2;m<=7;++m){
  char name[32];snprintf(name,sizeof name,"error-%u.bin",m);
  assert(!fm_copy_child_path(dest,argv[1],name));
  assert(run(m,source,dest,&bytes)==(int)expected[m]);
  struct stat s;
  if(m==7)assert(lstat(dest,&s)!=0&&errno==ENOENT);
  else {assert(!stat(dest,&s));assert((uint64_t)s.st_size==bytes);}
 }
 assert(!fm_copy_child_path(source,argv[1],"tree"));
 assert(!fm_copy_child_path(dest,argv[1],"tree-copy"));
 assert(!run(0,source,dest,&bytes)&&bytes==12);
 assert(!fm_copy_child_path(dest,source,"inside"));
 assert(run(0,source,dest,&bytes)==EINVAL);
 struct stat s;assert(stat(dest,&s)!=0&&errno==ENOENT);
 assert(!fm_copy_child_path(path,source,"nested/.."));
 assert(!fm_copy_child_path(dest,path,"alias-inside"));
 assert(run(0,source,dest,&bytes)==EINVAL);
 assert(fm_copy_inside_source("/Tree","/tree/child"));
 assert(fm_copy_inside_source("/Tree","/TREE"));
 assert(fm_copy_inside_source("/","/other"));
 assert(!fm_copy_inside_source("/tree","/tree-copy"));
 assert(!fm_copy_inside_source("/tree/long","/tree"));
 assert(!fm_copy_inside_source("/中文","/其他"));
 assert(!fm_copy_child_path(path,source,"nested"));
 assert(!fm_copy_child_path(dest,path,"case-alias"));
 assert(run(8,source,dest,&bytes)==EINVAL&&bytes==0);
 assert(stat(dest,&s)!=0&&errno==ENOENT);
 assert(!fm_copy_child_path(source,argv[1],"link"));
 assert(!fm_copy_child_path(dest,argv[1],"link-copy"));
 assert(run(0,source,dest,&bytes)==ENOTSUP);
 char huge[300];memset(huge,'a',299);huge[299]=0;
 assert(fm_copy_child_path(path,"/",huge)==ENAMETOOLONG);
 puts("File copy: recursive/empty folders, short writes, EINTR, ENOSPC, zero writes, flush/read errors, cancellation, no overwrite, subtree and symlink rejection; no descriptor leaks PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='fileman-copy-') as directory:
    path = Path(directory)
    data = bytes(i % 251 for i in range(150000))
    (path / 'source.bin').write_bytes(data)
    (path / 'existing.bin').write_bytes(b'keep')
    (path / 'tree/nested').mkdir(parents=True)
    (path / 'tree/empty').mkdir()
    (path / 'tree/中文.txt').write_bytes(b'hello')
    (path / 'tree/nested/data').write_bytes(b'content')
    (path / 'link').symlink_to(path / 'tree')
    source = path / 'test.c'
    source.write_text(program)
    binary = path / 'test'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(path)], check=True)
    assert (path / 'short.bin').read_bytes() == data
    assert (path / 'existing.bin').read_bytes() == b'keep'
    assert (path / 'tree-copy/中文.txt').read_bytes() == b'hello'
    assert (path / 'tree-copy/nested/data').read_bytes() == b'content'
    assert (path / 'tree-copy/empty').is_dir()
