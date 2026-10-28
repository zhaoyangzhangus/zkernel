"""Exercise production caption hit testing and client maximize/restore geometry."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/input_router.c').read_text()
def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <kernel/window_geometry.h>
#include <uapi/chrome_caption.h>
#define DESKTOP_TOPBAR_HEIGHT 0U
#define WINDOW_RESIZE_GRAB 8U
#define WINDOW_RESIZE_LEFT 1U
#define WINDOW_RESIZE_RIGHT 2U
#define WINDOW_RESIZE_TOP 4U
#define WINDOW_RESIZE_BOTTOM 8U
typedef struct {
 uint32_t flags,identifier,width,height,restore_width,restore_height;
 int32_t x,y,restore_x,restore_y;
 void *controls;bool maximized,dirty,resize_pending,fullscreen;
} window_server_window_t;
static struct { uint32_t display_width,display_height,dragging_identifier; } g_window_server={1920,1080,0};
static bool window_control_has_caption(const void *p){return p!=0;}
static unsigned marks,resizes,rehits;
static void window_input_refresh_pointer_locked(void){++rehits;}
static int32_t damage_x,damage_y;
static uint32_t damage_width,damage_height;
static void window_mark_rect_locked(int32_t x,int32_t y,uint32_t width,uint32_t height) {
 damage_x=x;damage_y=y;damage_width=width;damage_height=height;
}
static void window_mark_window_locked(window_server_window_t *w) { assert(w); ++marks; }
static void window_input_clear_drag_locked(void) { g_window_server.dragging_identifier=0; }
static void window_enqueue_resize_event_locked(window_server_window_t *w) { assert(w->resize_pending); ++resizes; }
''' + (root/'kernel/graphics/input.c').read_text().replace('#include "internal.h"','') + function('static void window_mark_caption_button_locked(') + '\n' + function('uint32_t window_title_button_at_locked(') + '\n' + function('static bool window_toggle_maximize_locked(') + r'''
int main(void) {
 const uint32_t flags=OS_WINDOW_VISIBLE|OS_WINDOW_RESIZABLE|OS_WINDOW_CLIENT_DECORATIONS|OS_WINDOW_STANDARD_CAPTION_CONTROLS;
 const uint32_t dark=flags|OS_WINDOW_DARK_CAPTION|OS_WINDOW_CAPTION_HEIGHT(46U);
 assert(window_client_drag_region(dark,0,0,960,90,20));
 assert(!window_client_drag_region(dark,0,0,960,400,20));
 assert(!window_client_drag_region(dark,0,0,960,180,20));
 window_server_window_t w={.flags=flags,.identifier=1,.x=240,.y=135,.width=1440,.height=810};
 for (unsigned button=1;button<=3;++button) {
  int x,y; assert(window_caption_button_rect(flags,w.x,w.y,w.width,button,&x,&y));
  assert(x==w.x+(int)w.width-(4-(int)button)*32 && y==w.y);
  assert(window_title_button_at_locked(&w,x,y)==0); // top resize strip
  assert(window_title_button_at_locked(&w,x+16,y+20)==button);
  assert(window_title_button_at_locked(&w,x+31,y+39)==(button==3?0:button));
  assert(window_title_button_at_locked(&w,x+16,y+40)==0);
 }
 assert(window_title_button_at_locked(&w,1583,155)==0);
 w.flags=flags|OS_WINDOW_CAPTION_HEIGHT(76U);
 assert(window_title_button_at_locked(&w,1664,210)==3);
 assert(window_title_button_at_locked(&w,1664,211)==0);
 w.flags=flags;
 assert(window_title_button_at_locked(0,1664,155)==0);
 /* 服务端装饰在每种缩放下，三个按钮的左右边界必须与绘制矩形完全一致。 */
 for(unsigned height=40;height<=128;++height) {
  w.flags=OS_WINDOW_VISIBLE|OS_WINDOW_RESIZABLE|OS_WINDOW_STANDARD_CAPTION_CONTROLS|
      OS_WINDOW_CAPTION_HEIGHT(height);
  unsigned bw=os_window_caption_button_width(w.flags);
  for(unsigned button=1;button<=3;++button) {
   int x,y;assert(window_caption_button_rect(w.flags,w.x,w.y,w.width,button,&x,&y));
   window_mark_caption_button_locked(&w,button);
   assert(damage_x==x && damage_y==y && damage_width==bw && damage_height==height);
   assert(window_title_button_at_locked(&w,x+1,y+height/2)==button);
   assert(window_title_button_at_locked(&w,x+(int)bw-9,y+height/2)==button);
   assert(window_title_button_at_locked(&w,x+(int)bw/2,y+(int)height)==0);
  }
 }
 w.flags=flags;
 w.flags &= ~OS_WINDOW_STANDARD_CAPTION_CONTROLS;
 assert(window_title_button_at_locked(&w,1664,155)==0);
 assert(window_toggle_maximize_locked(&w)); // Programmatic CSD maximize shares geometry policy.
 assert(window_toggle_maximize_locked(&w));
 marks=resizes=rehits=0;
 w.flags=flags;
 g_window_server.dragging_identifier=1;
 assert(window_toggle_maximize_locked(&w));
 assert(w.maximized && w.x==0 && w.y==0 && w.width==1920 && w.height==1080);
 assert(w.resize_pending && g_window_server.dragging_identifier==0);
 assert(window_toggle_maximize_locked(&w));
 assert(!w.maximized && w.x==240 && w.y==135 && w.width==1440 && w.height==810);
 assert(resizes==2 && marks==4 && rehits==2);
 w.fullscreen=true;assert(!window_toggle_maximize_locked(&w));w.fullscreen=false;
 w.flags &= ~OS_WINDOW_RESIZABLE;
 assert(!window_toggle_maximize_locked(&w));
 unsigned ink[5]={0};
 for (unsigned icon=1;icon<=4;++icon)
  for (unsigned y=0;y<12;++y) for(unsigned x=0;x<12;++x) ink[icon]+=chrome_caption_alpha(icon,x,y);
 assert(ink[1]==20*255 && ink[2]==64*255 && ink[3]>0 && ink[4]>0);
 assert(chrome_caption_alpha(1,12,0)==0 && chrome_caption_alpha(3,0,12)==0);
 assert(chrome_frame_inset(0,320)==4 && chrome_frame_inset(8,320)==0);
 assert(chrome_frame_row_inset(199,320,200)==4 && chrome_frame_alpha(0,199,320,200)==0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-caption-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), str(path / 'test.c'),
                    str(root / 'kernel/graphics/window_geometry.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Standard caption: production hit boundaries, Chrome opt-out, maximize/restore and vector paths passed')
