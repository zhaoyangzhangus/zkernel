"""Actual resize hit testing, cursor selection, capture and raster geometry."""
from pathlib import Path
import hashlib
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
hit = (root/'kernel/graphics/input.c').read_text().replace('#include "internal.h"', '')
router = (root/'kernel/graphics/input_router.c').read_text()
scene = (root/'kernel/graphics/scene.c').read_text()
policy = scene[scene.index('bool window_scene_pointer_captured_locked('):]
select = router[router.index('uint32_t window_input_cursor_shape_locked('):
                router.index('static window_server_window_t *keyboard_window_locked(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "uapi/window.h"
#include "cursor_shape.h"
#define WINDOW_RESIZE_GRAB 8U
#define WINDOW_RESIZE_LEFT 1U
#define WINDOW_RESIZE_RIGHT 2U
#define WINDOW_RESIZE_TOP 4U
#define WINDOW_RESIZE_BOTTOM 8U
typedef struct {int32_t x,y;uint32_t width,height,flags;bool maximized;} window_server_window_t;
enum {DESKTOP_APP_NONE=0};
static struct {uint32_t pointer_x,pointer_y,resize_edges,dragging_identifier;
 struct {bool menu_open,switch_open;uint32_t pressed_app;} desktop;} g_window_server;
static unsigned hit_queries;
static window_server_window_t window={100,100,200,100,OS_WINDOW_RESIZABLE,false};
static bool covered;
const uint32_t g_linux_cursor_argb[24*24]={[27]=0xff123456};
static uint32_t window_outer_width(uint32_t w,uint32_t f){return w+((f&OS_WINDOW_CLIENT_DECORATIONS)?0:2);}
static uint32_t window_outer_height(uint32_t h,uint32_t f){return h+((f&OS_WINDOW_CLIENT_DECORATIONS)?0:40);}
static window_server_window_t *window_scene_hit_test_locked(uint32_t x,uint32_t y){(void)x;(void)y;++hit_queries;return covered?0:&window;}
''' + hit + policy + select + r'''
static void hover(uint32_t x,uint32_t y,uint32_t expected){
 g_window_server.pointer_x=x;g_window_server.pointer_y=y;
 assert(window_input_cursor_shape_locked()==expected);
}
int main(void){
 for(unsigned client=0;client<2;++client){
  window.flags=OS_WINDOW_RESIZABLE|(client?OS_WINDOW_CLIENT_DECORATIONS:0);
  uint32_t right=100+window_outer_width(window.width,window.flags)-1;
  uint32_t bottom=100+window_outer_height(window.height,window.flags)-1;
  hover(100,150,WINDOW_CURSOR_SIZE_WE);hover(right,150,WINDOW_CURSOR_SIZE_WE);
  hover(200,100,WINDOW_CURSOR_SIZE_NS);hover(200,bottom,WINDOW_CURSOR_SIZE_NS);
  hover(100,100,WINDOW_CURSOR_SIZE_NWSE);hover(right,bottom,WINDOW_CURSOR_SIZE_NWSE);
  hover(right,100,WINDOW_CURSOR_SIZE_NESW);hover(100,bottom,WINDOW_CURSOR_SIZE_NESW);
  hover(107,107,WINDOW_CURSOR_SIZE_NWSE);hover(107,150,WINDOW_CURSOR_SIZE_WE);
  hover(108,108,WINDOW_CURSOR_ARROW);hover(99,150,WINDOW_CURSOR_ARROW);
  hover(right+1,150,WINDOW_CURSOR_ARROW);
  window.maximized=true;hover(100,100,WINDOW_CURSOR_ARROW);window.maximized=false;
 }
 /* 桌面捕获不允许后方窗口设置缩放光标，也不必扫描窗口。 */
 unsigned queries=hit_queries;
 g_window_server.desktop.menu_open=true;hover(100,100,WINDOW_CURSOR_ARROW);
 g_window_server.desktop.menu_open=false;g_window_server.desktop.switch_open=true;
 hover(100,100,WINDOW_CURSOR_ARROW);
 g_window_server.desktop.switch_open=false;g_window_server.desktop.pressed_app=7;
 hover(100,100,WINDOW_CURSOR_ARROW);
 assert(hit_queries==queries);g_window_server.desktop.pressed_app=DESKTOP_APP_NONE;
 hover(100,100,WINDOW_CURSOR_SIZE_NWSE);assert(hit_queries==queries+1);
 covered=true;hover(100,100,WINDOW_CURSOR_ARROW);covered=false;
 window.flags=0;hover(100,100,WINDOW_CURSOR_ARROW);window.flags=OS_WINDOW_RESIZABLE;
 g_window_server.dragging_identifier=1;
 g_window_server.resize_edges=WINDOW_RESIZE_RIGHT|WINDOW_RESIZE_TOP;
 queries=hit_queries;g_window_server.desktop.menu_open=true;
 hover(900,900,WINDOW_CURSOR_SIZE_NESW); // captured direction survives leaving the window
 assert(hit_queries==queries);g_window_server.desktop.menu_open=false;
 g_window_server.resize_edges=0;hover(100,100,WINDOW_CURSOR_ARROW); // title drag
 g_window_server.dragging_identifier=0;hover(900,900,WINDOW_CURSOR_ARROW);
 assert(window_cursor_pixel(WINDOW_CURSOR_ARROW,3,1)==0xff123456);
 for(unsigned shape=1;shape<=4;++shape){
  unsigned opaque=0,partial=0,clear=0;
  const unsigned hx[]={0,11,4,8,8},hy[]={0,4,11,8,8};
  assert(window_cursor_hotspot_x(shape)==hx[shape] && window_cursor_hotspot_y(shape)==hy[shape]);
  for(unsigned y=0;y<24;++y)for(unsigned x=0;x<24;++x){
   uint32_t p=window_cursor_pixel(shape,x,y);
   opaque+=(p>>24)==255;partial+=(p>>24)>0&&(p>>24)<255;clear+=p==0;
  }
  assert(opaque>20 && partial>0 && clear>400 && opaque+partial+clear==576);
  assert(window_cursor_pixel(shape,24,0)==0 && window_cursor_pixel(shape,0,24)==0);
  for(unsigned y=0;y<24;++y)for(unsigned x=0;x<24;++x){
   uint32_t p=window_cursor_pixel(shape,x,y);
   unsigned char bytes[]={p&255,(p>>8)&255,(p>>16)&255,p>>24};
   assert(fwrite(bytes,1,4,stdout)==4);
  }
 }
}
'''
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
with tempfile.TemporaryDirectory(prefix='liteos-resize-cursor-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/graphics'),
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    pixels = subprocess.check_output([binary])
    assert len(pixels) == 4*24*24*4
    expected = (
        'ca81172cc5a1cb86b072b66885a38455e892e3b76cba0bf10f6ec78b00d54910',
        'dc933fe44a580d9fe577143aa8caa803d03a40c0e915916272be569626ce8c11',
        '9237051e507070e740354161c9c68023ab898bab38f8969d2d16b451dd819f77',
        '1d9cd5675e1a5f33fc1d5cd926a14fb6ce4c5e00a95b3333f9e16d69d7caac91')
    for number, digest in enumerate(expected):
        assert hashlib.sha256(pixels[number*2304:(number+1)*2304]).hexdigest() == digest, \
            'Compiled cursor differs from the original Windows raster'
    negative=code.replace('window_scene_pointer_window_locked(),',
        'window_scene_hit_test_locked(g_window_server.pointer_x,g_window_server.pointer_y),')
    assert negative!=code
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/graphics'),
                    '-x','c','-','-o',binary],input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True)
    assert result.returncode!=0 and b'Assertion' in result.stderr
    print('负面对照通过：桌面捕获期间查询后方窗口会触发光标回归')
print('Resize cursor: enlarged eight-pixel native/client edges, capture/release, exclusions and four Windows alpha rasters/hotspots passed')
