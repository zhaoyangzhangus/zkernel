"""Exercise production shared-buffer readback, ordering and error handling."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'tools/nvidia/opengl_cube_demo.c').read_text()
assert 'gl.finish()' not in source and '"glFinish"' not in source
assert 'PIXEL_PACK_BUFFER' not in source
assert 'cube_output = vec4(interpolated_color, 0.0)' in source
assert 'gl.clear_color(0.035F, 0.045F, 0.075F, 0.0F)' in source
assert 'pixels[index * 4U + 3U] = 0U' not in source
function = 'static int cube_present(' + source.split('static int cube_present(', 1)[1].split(
    'static void cube_record_profile(', 1)[0]
harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include "cube_timing.h"
static unsigned clock_mode,clock_calls;
static int cube_now_ns(uint64_t *value){
 ++clock_calls;
 *value=clock_mode==2?100000-clock_calls*1000:clock_calls*1000;
 return clock_mode!=1;
}
#define CUBE_GL_BGRA 0x80e1U
#define CUBE_GL_UNSIGNED_BYTE 0x1401U
#define CUBE_GL_NO_ERROR 0U
#define LINUX_WINDOW_OPERATION_PRESENT 1U
typedef int cube_gl_sizei_t;
typedef struct {
 void (*read_pixels)(int,int,int,int,unsigned,unsigned,void*);
 unsigned (*get_error)(void);
} cube_gl_api_t;
static uint64_t g_cube_window_buffer_address;
static unsigned g_cube_shared_buffer_reported=0;
static uint8_t pixels[8*8*4];
static unsigned reads,overlays,presents,error,invalid_x;
static unsigned overlay_width=4,expected_damage=8;
static int present_success=1;
static uint32_t previous_width;
static void cube_emit(const char *s){(void)s;}
static void cube_emit_u64(uint64_t n){(void)n;}
static void read_pixels(int x,int y,int w,int h,unsigned format,unsigned type,void *data){
 assert(x==0&&y==0&&w==8&&h==8&&format==CUBE_GL_BGRA&&type==CUBE_GL_UNSIGNED_BYTE);
 assert(data==pixels);++reads;
 for(unsigned i=0;i<sizeof(pixels);++i)pixels[i]=(i%4==3)?invalid_x:255;
}
static unsigned get_error(void){assert(reads);return error;}
static uint32_t cube_draw_overlay(uint8_t *data,uint32_t w,uint32_t h,uint64_t f,uint64_t ns){
 assert(reads&&data==pixels&&w==8&&h==8&&f==1&&ns==123);++overlays;
 for(unsigned i=0;i<64;++i)assert(data[i*4+3]==0);
 return overlay_width;
}
static int cube_window_present(int fd,unsigned w,unsigned h,unsigned dw,unsigned dh){
 assert(overlays&&fd==7&&w==8&&h==8&&dw==expected_damage&&dh==8);
 ++presents;return present_success;
}
''' + function + r'''
int main(void){
 /* 吞吐下降、恢复及长停顿都必须在下一统计周期体现，不能被启动以来的平均值稀释。 */
 cube_fps_counter_t fps={0};
 assert(cube_fps_update(&fps,0,0)==0);
 assert(cube_fps_update(&fps,999,999999999ULL)==0);
 assert(cube_fps_update(&fps,1000,1000000000ULL)==1000);
 assert(cube_fps_update(&fps,1100,2000000000ULL)==100);
 assert(cube_fps_update(&fps,2100,3000000000ULL)==1000);
 assert(cube_fps_update(&fps,2101,13000000000ULL)==0);
 assert(cube_fps_update(&fps,7101,14000000000ULL)==5000);
 assert(cube_fps_update(&fps,7101,14000000000ULL)==5000);
 assert(cube_fps_update(&fps,7101,15000000000ULL)==0);
 assert(cube_fps_update(&fps,7201,1)==0);
 assert(cube_fps_update(&fps,8201,1000000001ULL)==1000);
 fps=(cube_fps_counter_t){.frames=UINT32_MAX-99};
 assert(cube_fps_update(&fps,100,1000000000ULL)==200);
 fps=(cube_fps_counter_t){0};
 assert(cube_fps_update(&fps,UINT32_MAX,1000000000ULL)==UINT32_MAX);
 fps=(cube_fps_counter_t){0};
 assert(cube_fps_update(&fps,3000,1500000000ULL)==2000);
 cube_gl_api_t api={read_pixels,get_error};
 g_cube_window_buffer_address=(uintptr_t)pixels;
 cube_timing_sample_t timing={0};
 assert(cube_present(&api,7,8,8,1,123,true,&previous_width,&timing));
 assert(timing.valid&&timing.readback_ns==1000&&timing.publish_ns==1000);
 cube_timing_totals_t totals={0};
 cube_timing_add(&totals,&timing);
 timing.readback_ns=3000;timing.publish_ns=500;
 cube_timing_add(&totals,&timing);
 assert(totals.frames==2&&totals.readback_ns==4000&&totals.publish_ns==1500);
 assert(totals.readback_max_ns==3000&&totals.publish_max_ns==1000);
 assert(reads==1&&overlays==1&&presents==1);
 error=1;
 assert(!cube_present(&api,7,8,8,1,123,true,&previous_width,&timing));
 assert(!timing.valid);
 cube_timing_add(&totals,&timing);
 assert(totals.frames==2);
 assert(reads==2&&overlays==1&&presents==1);
 error=0;invalid_x=255;g_cube_shared_buffer_reported=0;
 assert(!cube_present(&api,7,8,8,1,123,true,&previous_width,0));
 assert(reads==3&&overlays==1&&presents==1&&!g_cube_shared_buffer_reported);
 assert(!cube_present(0,7,8,8,1,123,true,&previous_width,0));
 assert(!cube_present(&api,-1,8,8,1,123,true,&previous_width,0));
 assert(!cube_present(&api,7,0,8,1,123,true,&previous_width,0));
 assert(!cube_present(&api,7,UINT32_MAX,UINT32_MAX,1,123,true,&previous_width,0));
 g_cube_window_buffer_address=0;
 assert(!cube_present(&api,7,8,8,1,123,true,&previous_width,0));
 assert(reads==3&&overlays==1&&presents==1);
 g_cube_window_buffer_address=(uintptr_t)pixels;
 invalid_x=0;g_cube_shared_buffer_reported=1;
 for(clock_mode=1;clock_mode<=2;++clock_mode){
  timing.valid=1;
  assert(cube_present(&api,7,8,8,1,123,true,&previous_width,&timing));
  assert(!timing.valid); /* Diagnostics must not break rendering. */
 }
 /* 静止仅提交文字条；文字缩短时覆盖旧字尾；场景变化必须完整提交。 */
 expected_damage=4;
 assert(cube_present(&api,7,8,8,1,123,false,&previous_width,&timing));
 assert(previous_width==4);
 overlay_width=2;
 assert(cube_present(&api,7,8,8,1,123,false,&previous_width,&timing));
 assert(previous_width==2);
 expected_damage=2;
 assert(cube_present(&api,7,8,8,1,123,false,&previous_width,&timing));
 expected_damage=8;
 assert(cube_present(&api,7,8,8,1,123,true,&previous_width,&timing));
 expected_damage=2;present_success=0;
 assert(!cube_present(&api,7,8,8,1,123,false,&previous_width,&timing));
 assert(previous_width==0&&!timing.valid);
 expected_damage=8;present_success=1;
 assert(cube_present(&api,7,8,8,1,123,false,&previous_width,&timing));
 assert(previous_width==2);
 assert(!cube_present(&api,7,8,8,1,123,false,0,&timing));
 unsigned before=clock_calls;
 assert(cube_present(&api,7,8,8,1,123,true,&previous_width,0));
 assert(clock_calls==before);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='cube-present-') as directory:
    base=Path(directory)
    code, binary=base/'test.c',base/'test'
    code.write_text(harness)
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-I',str(ROOT/'tools/nvidia'),str(code),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('Cube present: readback before overlay/publication, XRGB, errors and bounds passed')

# 使用真实命令构造函数，防止误把损坏宽度当成完整 surface 的步长。
command = source[source.index('static int cube_window_present('):
                 source.index('static int cube_now_ns(')]
command_harness = r'''
#include <assert.h>
#include <stdint.h>
#include <uapi/linux_abi.h>
static uint64_t g_cube_window_buffer_address=0x8000000;
static unsigned calls;
static linux_window_command_t received;
static int capture_ioctl(int fd,unsigned long op,const linux_window_command_t *c){
 assert(fd==7&&op==LINUX_WINDOW_IOCTL_COMMIT);received=*c;++calls;return 0;
}
#define ioctl capture_ioctl
''' + command + r'''
int main(void){
 assert(cube_window_present(7,800,600,320,22));
 assert(received.operation==LINUX_WINDOW_OPERATION_PRESENT);
 assert(received.address==g_cube_window_buffer_address&&received.length==1920000);
 assert(received.stride==3200&&received.width==320&&received.height==22);
 assert(!received.x&&!received.y&&!received.reserved&&!received.reserved2);
 assert(cube_window_present(7,800,600,800,600));
 assert(received.length==1920000&&received.stride==3200);
 const uint32_t invalid[][4]={{0,600,1,1},{800,0,1,1},{UINT32_MAX,600,1,1},
  {800,600,0,22},{800,600,801,22},{800,600,320,0},{800,600,320,601}};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)
  assert(!cube_window_present(7,invalid[i][0],invalid[i][1],invalid[i][2],invalid[i][3]));
 assert(calls==2);
}
'''
overlay = source[source.index('static uint8_t cube_glyph_row('):source.index('static int cube_present(')]
overlay_harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
static uint8_t image[800*600*4+8];
''' + overlay + r'''
int main(void){
 const unsigned widths[]={1,4,8,16,160,320,800},heights[]={1,8,21,22,32,600};
 for(unsigned w=0;w<7;++w)for(unsigned h=0;h<6;++h)
 for(unsigned t=0;t<4;++t){
  unsigned width=widths[w],height=heights[h];
  memset(image,0xa5,sizeof(image));
  uint32_t frames=t==0?UINT32_MAX:1000;uint64_t elapsed=t==0?1:t*10000000000ULL;
  char text[64];cube_format_overlay(text,frames,elapsed);
  unsigned expected=8+strlen(text)*12;if(expected>width)expected=width;
  unsigned actual=cube_draw_overlay(image+4,width,height,frames,elapsed);
  assert(actual==expected);
  for(unsigned i=0;i<4;++i)assert(image[i]==0xa5);
  unsigned bottom=height<22?height:22;
  for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
   const uint8_t *p=image+4+(y*width+x)*4;
   if(y<bottom&&x<expected)assert(p[3]==0);
   else for(unsigned c=0;c<4;++c)assert(p[c]==0xa5);
  }
  for(unsigned i=4+width*height*4;i<sizeof(image);++i)assert(image[i]==0xa5);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='cube-damage-') as directory:
    base=Path(directory)
    for name, program in [('command',command_harness),('overlay',overlay_harness)]:
        code,binary=base/(name+'.c'),base/name
        code.write_text(program)
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                        '-fno-pie','-no-pie','-I'+str(ROOT/'include'),str(code),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
print('Cube damage: full surface stride/length with partial damage, static/rotation/shorter text/retry transitions and 168 clipped overlay pixel-boundary cases passed')

# 基准旋转由时间驱动，八秒一圈；不依赖本轮渲染快慢或长期 float 累积。
rotation = source[source.index('static float cube_benchmark_yaw('):
                  source.index('int main(int argc, char **argv)')]
rotation_harness = r'''
#include <stdint.h>
#include <assert.h>
#define CUBE_TWO_PI 6.28318530717958647692F
''' + rotation + r'''
int main(void){
 assert(cube_benchmark_yaw(0)==0.55F);
 assert(cube_benchmark_yaw(8000000000ULL)==cube_benchmark_yaw(0));
 assert(cube_benchmark_yaw(1234567890ULL)==cube_benchmark_yaw(9234567890ULL));
 float half=cube_benchmark_yaw(4000000000ULL)-0.55F;
 assert(half>3.1415F&&half<3.1417F);
 assert(cube_benchmark_yaw(UINT64_MAX)>=0.55F);
 assert(cube_benchmark_yaw(UINT64_MAX)<=0.55F+CUBE_TWO_PI);
}
'''
with tempfile.TemporaryDirectory(prefix='cube-rotation-') as directory:
    code,binary=Path(directory)/'test.c',Path(directory)/'test'
    code.write_text(rotation_harness)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(code),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
assert 'benchmark || frame == 0U || yaw != presented_yaw || pitch != presented_pitch' in source
print('Cube benchmark: time-based periodic rotation and forced full presentation passed')
