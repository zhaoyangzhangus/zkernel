"""验证服务端装饰所有权、真实创建/更新协议与客户端度量。"""
from pathlib import Path
import subprocess
import tempfile
import sys
root = Path(__file__).resolve().parents[2]
for app in ('gshell', 'imageview', 'notepad', 'taskmgr', 'fileman', 'settings'):
    path = 'user/desktop/settings/main.c' if app == 'settings' else 'user/desktop/' + app + '/main.c'
    source = (root / path).read_text()
    if app == 'settings':
        source += (root / 'user/desktop/settings/network_tree.h').read_text()
    obsolete_paths = ('user_client_chrome_', 'USER_CLIENT_CHROME_', 'OS_WINDOW_CLIENT_DECORATIONS')
    for obsolete in obsolete_paths:
        assert obsolete not in source, (app, obsolete)
    metric = 'user_caption_height()'
    for required in ('OS_WINDOW_STANDARD_CAPTION_CONTROLS', 'OS_WINDOW_EVENT_CLOSE_REQUEST', metric):
        assert required in source, (app, required)
    assert f'OS_WINDOW_CAPTION_HEIGHT({metric})' in source or (
        'caption=user_caption_height()' in source and 'OS_WINDOW_CAPTION_HEIGHT(caption)' in source), app
image = (root / 'user/desktop/imageview/main.c').read_text()
fileman = (root / 'user/desktop/fileman/main.c').read_text()
assert 'OS_WINDOW_CLIENT_DECORATIONS' not in fileman and 'OS_WINDOW_DARK_CAPTION' not in fileman
assert 'OS_THEME_HEADER' not in fileman
assert 'FM4_CONTROL_CLOSE' not in fileman
assert 'fm4_window_frame' not in fileman
demo = (root / 'user/runtime/dyn_gfx.c').read_text()
assert 'OS_WINDOW_CLIENT_DECORATIONS' not in demo and 'gfx_frame(' not in demo
begin = image.index('static void reset_view(')
layout = image[begin:image.index('static void draw_image(', begin)]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "user/runtime/caption.h"
#include "user/runtime/paint.h"
static uint32_t font_height;
uint32_t ui_text_line_height(void){return font_height;}
uint32_t ui_text_size(void){return font_height;}
#define IMAGEVIEW_STATUS_HEIGHT 32U
static struct {uint32_t width,height;} g_window;
static uint32_t g_zoom_percent;
static int32_t g_pan_x,g_pan_y;
static uint32_t received_width,received_height;
static uint32_t fit_zoom(uint32_t w,uint32_t h){received_width=w;received_height=h;return 75;}
static void clamp_pan(uint32_t w,uint32_t h){received_width=w;received_height=h;}
''' + layout + r'''
int main(void) {
    uint32_t surface[34*32+2];
    for(uint32_t w=1;w<=32;++w) for(uint32_t h=1;h<=32;++h) {
        for(unsigned i=0;i<sizeof(surface)/sizeof(*surface);++i)surface[i]=42;
        user_paint_round_rect(surface+1,34,w,h,0,0,UINT_MAX,UINT_MAX,7);
        assert(surface[0]==42&&surface[34*h+1]==42);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=w;x<34;++x)
            assert(surface[1+y*34+x]==42);
        user_paint_round_rect(surface+1,34,w,h,UINT_MAX,UINT_MAX,32,32,7);
    }
    struct {char before;char title[32];char after;} caption={.before=42,.after=43};
    char path[256];memset(path,'p',255);path[255]=0;
    user_caption_format_title(caption.title,path,true);
    assert(caption.before==42&&caption.after==43&&caption.title[0]=='*'&&caption.title[1]==' '&&caption.title[31]==0);
    user_caption_format_title(caption.title,"/notes.txt",false);
    assert(!strcmp(caption.title,"/notes.txt"));
    for(unsigned i=11;i<32;++i)assert(!caption.title[i]);
    user_caption_format_title(caption.title,NULL,false);assert(!caption.title[0]);
    for(font_height=0;font_height<256;++font_height) {
        uint32_t expected=font_height>=104?128:font_height>16?font_height+24:40;
        assert(user_caption_height()==expected);
    }
    font_height=UINT_MAX;assert(user_caption_height()==128);
    for(uint32_t height=0;height<1200;++height) {
        g_window.width=640;g_window.height=height;g_pan_x=10;g_pan_y=20;
        reset_view();
        assert(g_zoom_percent==75&&!g_pan_x&&!g_pan_y&&received_width==640);
        assert(received_height==(height>32?height-32:1));
        change_zoom(25);assert(g_zoom_percent==100&&received_height==(height>32?height-32:1));
        change_zoom(-100);assert(g_zoom_percent==10);
        change_zoom(1000);assert(g_zoom_percent==400);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='server-caption-apps-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root), '-I' + str(root / 'include'),
                    str(base / 'test.c'), '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=15)

def extract(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

header = next(line for line in fileman.splitlines() if line.startswith('#define FM4_HEADER_HEIGHT'))
def allowed_mask(path, marker):
    text = (root / path).read_text()
    start = text.index(marker) + len(marker)
    return text[start:text.index(')) != 0U', start)]
native_mask = allowed_mask('kernel/syscall/graphics.c', '(arguments.flags & ~(')
server_mask = allowed_mask('kernel/graphics/window.c', '(flags & ~(')
request_code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <uapi/window.h>
#include <uapi/display.h>
#include <uapi/syscall.h>
#include <uapi/window_theme.h>
#include "user/runtime/caption.h"
static uint32_t line_height;
uint32_t ui_text_line_height(void){return line_height;}
uint32_t ui_text_size(void){return line_height;}
#define FILEMAN_MAP_BASE 0x40000000U
static struct {os_handle_t handle;uint32_t identifier,width,height;uint32_t *pixels;} g_window;
static uint32_t g_caption_height,created_flags,updated_flags,updates;
static int64_t fileman_syscall_one(uint64_t number,uint64_t address){
 if(number==OS_SYS_DISPLAY_GET_INFO){
  os_display_info_t *r=(void *)(uintptr_t)address;r->width=1920;r->height=1080;return 0;
 }
 if(number==OS_SYS_WINDOW_CREATE){
  os_window_create_t *r=(void *)(uintptr_t)address;
  created_flags=r->flags;
  assert(r->width==960&&r->height==640&&r->x==480&&r->y==220);
  assert((r->flags&(OS_WINDOW_CLIENT_DECORATIONS|OS_WINDOW_STANDARD_CAPTION_CONTROLS|
   OS_WINDOW_DARK_CAPTION))==OS_WINDOW_STANDARD_CAPTION_CONTROLS);
  r->window=3;r->identifier=7;return 0;
 }
 assert(number==OS_SYS_WINDOW_UPDATE);
 os_window_update_t *r=(void *)(uintptr_t)address;assert(r->identifier==7);
 updated_flags=r->flags;++updates;return 0;
}
''' + header + '\n' + extract(fileman, 'static bool create_window(') + '\n' + extract(fileman, 'static void update_window(') + r'''
int main(void){
 for(unsigned sample=0;sample<=257;++sample){
  line_height=sample==257?UINT32_MAX:sample;
  /* 客户区没有第二条标题/导航栏，字体改变只影响服务端标题栏。 */
  assert(FM4_HEADER_HEIGHT==0);
  assert(create_window());
  uint32_t caption=user_caption_height();
  assert(os_window_caption_height(created_flags)==caption);
  /* 两层真实入口都必须接受创建度量，不能只在应用侧伪造成功。 */
  @MASK_CHECKS@
  assert(os_window_caption_create_valid(created_flags,640));
  g_caption_height=0;update_window();
  assert(os_window_caption_height(updated_flags)==caption&&g_caption_height==caption);
  unsigned before=updates;update_window();assert(updates==before);
 }
 for(unsigned caption=1;caption<256;++caption){
  unsigned flags=OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_CAPTION_HEIGHT(caption);
  assert(os_window_caption_create_valid(flags,640)==(caption>=40&&caption<=128));
  assert(!os_window_caption_create_valid(flags&~OS_WINDOW_STANDARD_CAPTION_CONTROLS,640));
  if(caption>=40&&caption<=128)assert(!os_window_caption_create_valid(flags,caption-1));
 }
}
'''
checks = '\n'.join(f'assert((created_flags & ~({mask}))==0U);' for mask in (native_mask, server_mask))
request_code = request_code.replace('@MASK_CHECKS@', checks)
with tempfile.TemporaryDirectory(prefix='fileman-caption-contract-') as directory:
    base = Path(directory)
    def verify(body, name, passes):
        path = base / (name + '.c')
        path.write_text(body)
        binary = base / name
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I' + str(root), '-I' + str(root / 'include'), str(path),
                        '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True)
        assert (result.returncode == 0) == passes, (name, result.stderr.decode())
    verify(request_code, 'production', True)
    if '--negative-controls' in sys.argv:
        old_initial = request_code.replace('OS_WINDOW_CAPTION_HEIGHT(user_caption_height());', '0U;', 1)
        assert old_initial != request_code
        verify(old_initial, 'initial-height-missing', False)
        old_metric = request_code.replace(header, '#define FM4_HEADER_HEIGHT (ui_text_line_height()+16U>46U?ui_text_line_height()+16U:46U)')
        assert old_metric != request_code
        verify(old_metric, 'unbounded-header', False)
        print('Negative controls rejected: missing initial height, duplicate client header')
print('Server captions: ownership checks, 258 real Files create/update metrics and 1200 image layouts passed')
