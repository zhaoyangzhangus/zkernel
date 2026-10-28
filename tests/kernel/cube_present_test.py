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
static void cube_emit(const char *s){(void)s;}
static void cube_emit_u64(uint64_t n){(void)n;}
static void read_pixels(int x,int y,int w,int h,unsigned format,unsigned type,void *data){
 assert(x==0&&y==0&&w==8&&h==8&&format==CUBE_GL_BGRA&&type==CUBE_GL_UNSIGNED_BYTE);
 assert(data==pixels);++reads;
 for(unsigned i=0;i<sizeof(pixels);++i)pixels[i]=(i%4==3)?invalid_x:255;
}
static unsigned get_error(void){assert(reads);return error;}
static void cube_draw_overlay(uint8_t *data,uint32_t w,uint32_t h,uint32_t f,uint64_t ns){
 assert(reads&&data==pixels&&w==8&&h==8&&f==1&&ns==123);++overlays;
 for(unsigned i=0;i<64;++i)assert(data[i*4+3]==0);
}
static int cube_window_command(int fd,unsigned op,int x,int y,unsigned w,unsigned h){
 assert(overlays&&fd==7&&op==1&&x==0&&y==0&&w==8&&h==8);++presents;return 1;
}
''' + function + r'''
int main(void){
 cube_gl_api_t api={read_pixels,get_error};
 g_cube_window_buffer_address=(uintptr_t)pixels;
 cube_timing_sample_t timing={0};
 assert(cube_present(&api,7,8,8,1,123,&timing));
 assert(timing.valid&&timing.readback_ns==1000&&timing.publish_ns==1000);
 cube_timing_totals_t totals={0};
 cube_timing_add(&totals,&timing);
 timing.readback_ns=3000;timing.publish_ns=500;
 cube_timing_add(&totals,&timing);
 assert(totals.frames==2&&totals.readback_ns==4000&&totals.publish_ns==1500);
 assert(totals.readback_max_ns==3000&&totals.publish_max_ns==1000);
 assert(reads==1&&overlays==1&&presents==1);
 error=1;
 assert(!cube_present(&api,7,8,8,1,123,&timing));
 assert(!timing.valid);
 cube_timing_add(&totals,&timing);
 assert(totals.frames==2);
 assert(reads==2&&overlays==1&&presents==1);
 error=0;invalid_x=255;g_cube_shared_buffer_reported=0;
 assert(!cube_present(&api,7,8,8,1,123,0));
 assert(reads==3&&overlays==1&&presents==1&&!g_cube_shared_buffer_reported);
 assert(!cube_present(0,7,8,8,1,123,0));
 assert(!cube_present(&api,-1,8,8,1,123,0));
 assert(!cube_present(&api,7,0,8,1,123,0));
 assert(!cube_present(&api,7,UINT32_MAX,UINT32_MAX,1,123,0));
 g_cube_window_buffer_address=0;
 assert(!cube_present(&api,7,8,8,1,123,0));
 assert(reads==3&&overlays==1&&presents==1);
 g_cube_window_buffer_address=(uintptr_t)pixels;
 invalid_x=0;g_cube_shared_buffer_reported=1;
 for(clock_mode=1;clock_mode<=2;++clock_mode){
  timing.valid=1;
  assert(cube_present(&api,7,8,8,1,123,&timing));
  assert(!timing.valid); /* Diagnostics must not break rendering. */
 }
 unsigned before=clock_calls;
 assert(cube_present(&api,7,8,8,1,123,0));
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
