"""Execute actual one-shot cube profiling record without rendering a frame."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'tools/nvidia/opengl_cube_demo.c').read_text()
body = source[source.index('static void cube_record_profile('):
              source.index('/* 回读时间包含等待')]
assert 'cube_record_profile(display_width, display_height, cube_width,' in source
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned records;
static char saved[384];
static void cube_record_log(const char *path,const char *text) {
 assert(!strcmp(path,"/log/cube-profile.log"));
 assert(strlen(text)<sizeof(saved));strcpy(saved,text);++records;
}
''' + body + r'''
int main(void) {
 cube_record_profile(1920,1080,800,600,NULL);assert(!records);
 cube_record_profile(1920,1080,800,600,"NVIDIA GeForce RTX 5070");
 assert(records==1);
 assert(!strcmp(saved,"OS_CUBE_PROFILE display=1920x1080 surface=800x600 renderer=NVIDIA GeForce RTX 5070\n"
  "readback=GPU_wait_and_CPU_copy submit=window_present_ACK_wait\n"));
 cube_record_profile(0,0,640,480,"fixture");
 assert(records==2 && strstr(saved,"display=0x0 surface=640x480"));
 char long_name[4096];memset(long_name,'A',sizeof(long_name));long_name[4095]=0;
 cube_record_profile(UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,long_name);
 assert(records==3 && strlen(saved)<sizeof(saved));
 const char* begin=strstr(saved,"renderer=")+9;
 const char* end=strchr(begin,'\n');assert(end-begin==160);
}
'''
with tempfile.TemporaryDirectory(prefix='cube-profile-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=10)
print('Cube profile: actual reporter, geometry, renderer, bounded name, unknown display and uint32 limits passed')
