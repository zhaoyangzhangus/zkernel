"""Exercise the actual bounded persistent cube diagnostic writer."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/nvidia/opengl_cube_demo.c').read_text()
function = source[source.index('static void cube_record_log('):
                  source.index('static void cube_emit(')]
program = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
static char result[128];
static unsigned used,calls,syncs,closes,opens,mode;
static int mock_open(const char *path,int flags,int perms){
 assert(!strcmp(path,"/log/cube-result.log"));
 assert(flags==(O_WRONLY|O_CREAT|O_TRUNC) && perms==0644);++opens;
 if(mode==1)return -1;return 17;
}
static ssize_t mock_write(int fd,const void *p,size_t n){
 assert(fd==17);++calls;
 if(mode==2 && calls==1){errno=EINTR;return -1;}
 if(mode==3)return 0;
 if(mode==4){errno=ENOSPC;return -1;}
 if(mode==5)return n+1;
 if(n>3)n=3;memcpy(result+used,p,n);used+=n;return n;
}
static int mock_fsync(int fd){assert(fd==17);++syncs;return -1;}
static int mock_close(int fd){assert(fd==17);++closes;return -1;}
#define open mock_open
#define write mock_write
#define fsync mock_fsync
#define close mock_close
''' + function + r'''
int main(void){
 cube_record_result(0);assert(opens==0);
 for(mode=0;mode<=5;++mode){
  used=calls=syncs=closes=opens=0;memset(result,0,sizeof(result));
  cube_record_result("OS_CUBE_FBO_STATUS_FAIL\n");
  assert(opens==1);
  if(mode==1){assert(!calls && !syncs && !closes);continue;}
  assert(syncs==1 && closes==1);
  if(mode<=2)assert(!strcmp(result,"OS_CUBE_FBO_STATUS_FAIL\n"));
  else assert(calls==1 && used==0);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='cube-result-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Cube result log: short writes/EINTR/open/full-disk/zero/invalid writes and cleanup PASS')
