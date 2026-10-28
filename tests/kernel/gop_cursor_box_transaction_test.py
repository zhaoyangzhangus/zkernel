"""真实 GOP 发布函数：先写光标包围盒，之后的场景复制不得再次覆盖它。"""
from pathlib import Path
import subprocess
import resource
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/present_cursor.c').read_text()
blend = source[source.index('static uint32_t cursor_blend_xrgb('):source.index('bool window_present_cursor_into_buffer(')]
functions = source[source.index('bool window_present_cursor_into_buffer('):source.index('void compositor_present_cursor_direct(')]
overlay = source[source.index('bool window_present_cursor_overlay('):]
assert 'scanline[' not in source
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <kernel/rect.h>
#define WINDOW_CURSOR_WIDTH 4U
#define WINDOW_CURSOR_HEIGHT 4U
static uint32_t front[10*8], scene[10*8], writes[10*8];
static struct {
 volatile uint32_t *framebuffer;
 uint32_t *composite_framebuffer;
 uint32_t display_width,display_height,display_stride;
 bool presented_pointer_valid;
 uint32_t presented_pointer_x,presented_pointer_y,presented_cursor_shape;
 uint32_t pointer_x,pointer_y;
} g_window_server;
static bool native;
static unsigned initialized,span_calls,pixel_calls,preempt,locked,current_shape;
static void window_lock(void){assert(!locked);locked=1;}
static void window_unlock(void){assert(locked);locked=0;}
static unsigned window_input_cursor_shape_locked(void){assert(locked);return current_shape;}
static void sched_preempt_disable(void){assert(!preempt);preempt=1;}
static void sched_preempt_enable(void){assert(preempt);preempt=0;}
static bool display_core_has_native_scanout(unsigned n){assert(!n);return native;}
static int window_cursor_hotspot_x(unsigned s){return s?3:1;}
static int window_cursor_hotspot_y(unsigned s){return s?2:1;}
static uint32_t window_cursor_pixel(unsigned s,unsigned x,unsigned y){
 assert(x<4&&y<4);return ((x+y)%3==0?0U:(x+y)%3==1?0x80000000U:0xff000000U)|0x00eeddccU|s;
}
static void compositor_present_cursor_direct(bool force){assert(!force);++initialized;}
static void display_core_publish_xrgb8888_span(volatile uint32_t *d,const uint32_t *s,uint32_t n){
 assert(d>=front&&d+n<=front+80);++span_calls;
 for(uint32_t i=0;i<n;++i){++writes[d+i-front];d[i]=s[i];}
}
static void display_core_publish_xrgb8888_pixel(volatile uint32_t *d,uint32_t pixel){
 assert(d>=front&&d<front+80);++pixel_calls;++writes[d-front];*d=pixel;
}
''' + blend + functions + overlay + r'''
int main(void){
 g_window_server.framebuffer=front;g_window_server.composite_framebuffer=scene;
 g_window_server.display_width=8;g_window_server.display_height=6;g_window_server.display_stride=10;
 g_window_server.presented_pointer_valid=true;
 Rect rect={0,0,8,6};
 for(unsigned shape=0;shape<2;++shape)for(unsigned py=0;py<10;++py)
 for(unsigned px=0;px<12;++px)for(unsigned pass=0;pass<2;++pass){
  g_window_server.presented_pointer_x=px;g_window_server.presented_pointer_y=py;
  g_window_server.presented_cursor_shape=shape;
  int left=(int)px-window_cursor_hotspot_x(shape),top=(int)py-window_cursor_hotspot_y(shape);
  for(unsigned i=0;i<80;++i){front[i]=0xdeadbeef;scene[i]=0x00335577+i+pass;writes[i]=0;}
  uint32_t original_scene[80];
  memcpy(original_scene,scene,sizeof(scene));
  window_present_cursor_prepare_rect(&rect);
  /* 光标只改变最终屏幕像素，提交画面及行填充必须逐字节保持不变。 */
  assert(!memcmp(original_scene,scene,sizeof(scene)));
  /* 所有光标行必须先完成；之后复制每行时包围盒的写次数不能变化。 */
  for(unsigned y=0;y<6;++y)for(unsigned x=0;x<8;++x){
   bool in=(int)x>=left&&(int)x<left+4&&(int)y>=top&&(int)y<top+4;
   assert(writes[y*10+x]==(unsigned)in);
  }
  for(unsigned y=0;y<6;++y){
   if(!window_present_cursor_publish_span(front+y*10,scene+y*10,0,y,8))
    display_core_publish_xrgb8888_span(front+y*10,scene+y*10,8);
  }
  for(unsigned y=0;y<8;++y)for(unsigned x=0;x<10;++x){
   unsigned i=y*10+x;
   if(y>=6||x>=8){assert(!writes[i]&&front[i]==0xdeadbeef);continue;}
   bool in=(int)x>=left&&(int)x<left+4&&(int)y>=top&&(int)y<top+4;
   uint32_t expected=scene[i];
   if(in)expected=cursor_blend_xrgb(expected,window_cursor_pixel(shape,x-left,y-top));
   assert(writes[i]==1&&front[i]==expected);
  }
  assert(!memcmp(original_scene,scene,sizeof(scene)));
 }
 memset(writes,0,sizeof(writes));native=true;window_present_cursor_prepare_rect(&rect);native=false;
 window_present_cursor_prepare_rect(0);
 for(unsigned i=0;i<80;++i)assert(!writes[i]);
 g_window_server.presented_pointer_valid=false;window_present_cursor_prepare_rect(&rect);
 assert(initialized==1);
 /* 执行真实移动路径：旧包围盒逐行恢复，新光标直接混合到屏幕。 */
 unsigned moves=0;
 for(unsigned old_shape=0;old_shape<2;++old_shape)
 for(unsigned old_y=0;old_y<10;++old_y)for(unsigned old_x=0;old_x<12;++old_x)
 for(unsigned shape=0;shape<2;++shape)
 for(unsigned py=0;py<10;++py)for(unsigned px=0;px<12;++px){
  for(unsigned i=0;i<80;++i)front[i]=scene[i]=0x00554400+i;
  assert(window_present_cursor_into_buffer(front,old_x,old_y,old_shape));
  memset(writes,0,sizeof(writes));span_calls=pixel_calls=0;
  g_window_server.presented_pointer_valid=true;
  g_window_server.presented_pointer_x=old_x;g_window_server.presented_pointer_y=old_y;
  g_window_server.presented_cursor_shape=old_shape;
  g_window_server.pointer_x=px;g_window_server.pointer_y=py;current_shape=shape;
  bool changed=old_x!=px||old_y!=py||old_shape!=shape;
  int left=(int)px-window_cursor_hotspot_x(shape),top=(int)py-window_cursor_hotspot_y(shape);
  int old_left=(int)old_x-window_cursor_hotspot_x(old_shape);
  int old_top=(int)old_y-window_cursor_hotspot_y(old_shape);
  int64_t out_left=-999,out_top=-999,out_right=-999,out_bottom=-999;
  assert(window_present_cursor_overlay(false,&out_left,&out_top,&out_right,&out_bottom)==changed);
  assert(!preempt&&!locked);
  if(changed)assert(out_left==left&&out_top==top&&out_right==left+4&&out_bottom==top+4);
  else assert(out_left==-999&&out_top==-999&&out_right==-999&&out_bottom==-999);
  unsigned expected_spans=0,expected_pixels=0;
  for(int y=0;y<6;++y){
   if(old_left<8&&old_left+4>0&&y>=old_top&&y<old_top+4)++expected_spans;
  }
  for(unsigned y=0;y<8;++y)for(unsigned x=0;x<10;++x){
   unsigned i=y*10+x;
   bool in=x<8&&y<6&&(int)x>=left&&(int)x<left+4&&(int)y>=top&&(int)y<top+4;
   uint32_t expected=0x00554400+i;
   assert(scene[i]==expected);
   if(in){expected=cursor_blend_xrgb(expected,window_cursor_pixel(shape,x-left,y-top));++expected_pixels;}
   assert(front[i]==expected);
   if(x>=8||y>=6)assert(!writes[i]);
  }
  assert(span_calls==(changed?expected_spans:0));
  assert(pixel_calls==(changed?expected_pixels:0));
  ++moves;
 }
 assert(moves==57600);
}
'''
with tempfile.TemporaryDirectory(prefix='cursor-box-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-no-pie',
                    '-fsanitize=address,undefined', '-I'+str(root/'include'),
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
    # 省略背景恢复必须被真实移动的像素和调用次数检查拒绝。
    assert 'if (!draw_cursor) {' in program
    (path / 'test.c').write_text(program.replace('if (!draw_cursor) {',
                                               'if (false && !draw_cursor) {'))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-no-pie',
                    '-I'+str(root/'include'), str(path/'test.c'), '-o', str(path/'test')], check=True)
    result = subprocess.run([str(path/'test')], capture_output=True,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode != 0 and b'Assertion' in result.stderr
print('GOP cursor box: 480 box transactions and 57600 real cursor moves, span restoration and rejected missing-restore mutation, box pixels written once, alpha/shapes/edges/stride and path exclusions PASS')
