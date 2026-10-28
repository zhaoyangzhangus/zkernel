"""执行图片查看器真实打开事件和替换逻辑，验证旧事件及失败回滚。"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'user/desktop/imageview/main.c').read_text()
def extract(signature):
 start=source.index(signature);end=source.index('{',start)+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
state=source[source.index('#define IMAGEVIEW_DEFAULT_PATH'):source.index('static void copy_path(')]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/all.h>
#include <uapi/window_theme.h>
#include "user/runtime/file_picker.h"
#include "user/runtime/modal_tree.h"
uint32_t ui_text_size(void){return 20;}
uint32_t ui_text_line_height(void){return 24;}
bool ui_text_adjust(int delta){(void)delta;return true;}
static int64_t __libc_native_syscall6(uint64_t op,uint64_t address,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e){
 assert(!a&&!b&&!c&&!d&&!e);
 if(op==OS_SYS_FILE_STAT){os_file_stat_t *r=(void *)(uintptr_t)address;r->info.type=OS_FILE_TYPE_DIRECTORY;return 0;}
 assert(op==OS_SYS_FILE_ENUMERATE_BATCH);return -2;
}
''' +state+r'''
static bool decode_ok;static unsigned decode_calls;
static bool load_image_file(void){++decode_calls;return decode_ok;}
'''+'\n'.join(extract(sig) for sig in ['static void copy_path(', 'static void clamp_pan(', 'static uint32_t fit_zoom(', 'static void reset_view(', 'static void change_zoom(', 'static bool key_matches(', 'static bool handle_key(', 'static char key_to_ascii(', 'static int64_t imageview_picker_call(', 'static void imageview_open_selected(', 'static bool handle_event('])+r'''
int main(void){
 g_window.width=640;g_window.height=480;g_image_width=320;g_image_height=240;
 copy_path("/old.png");g_nodes[0]=(os_control_node_t){.identifier=7,.kind=OS_CONTROL_BUTTON};
 g_tree=(os_window_control_tree_t){.count=1,.revision=1};
 os_window_event_t event={.type=OS_WINDOW_EVENT_CONTROL,.control={.target=7,.node_index=0,.node_kind=OS_CONTROL_BUTTON,.tree_revision=1,.action=OS_CONTROL_DOWN}};
 assert(handle_event(&event)&&!g_picker.active);
 event.control.action=OS_CONTROL_CLICK;event.control.tree_revision=2;
 assert(!handle_event(&event)&&!g_picker.active);
 event.control.tree_revision=1;assert(handle_event(&event)&&g_picker.active&&!decode_calls);
 /* 选择器只因可见状态变化要求重绘，移动、修饰键及松开不重建控件树。 */
 os_window_event_t quiet={.type=OS_WINDOW_EVENT_INPUT,
  .input={.type=OS_INPUT_EVENT_RELATIVE,.code=OS_INPUT_REL_X,.value=1}};
 assert(!handle_event(&quiet));
 quiet.input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=0xe0,.value=OS_INPUT_VALUE_PRESS};
 assert(!handle_event(&quiet)&&g_picker.ctrl);
 quiet.input.value=OS_INPUT_VALUE_RELEASE;assert(!handle_event(&quiet)&&!g_picker.ctrl);
 quiet.input.code=0x04;assert(!handle_event(&quiet));
 quiet.input.value=OS_INPUT_VALUE_PRESS;assert(handle_event(&quiet)&&!strcmp(g_picker.name,"a"));
 quiet.input.value=OS_INPUT_VALUE_RELEASE;assert(!handle_event(&quiet));
 /* 错误图片保留窗口、旧路径及原图；重新选择有效图片后重置缩放。 */
 strcpy(g_picker.name,"/bad.bin");g_picker.accepted=true;g_picker.active=false;
 imageview_open_selected();assert(g_picker.active&&!g_picker.accepted&&g_picker.error[0]);
 assert(!strcmp(g_path,"/old.png")&&g_running&&decode_calls==1);
 strcpy(g_picker.name,"/new.png");g_picker.accepted=true;g_picker.active=false;
 decode_ok=true;g_content_valid=true;g_pan_x=100;g_pan_y=100;
 imageview_open_selected();assert(!g_picker.active&&!g_picker.entries&&!g_content_valid);
 assert(!strcmp(g_path,"/new.png")&&decode_calls==2&&!g_pan_x&&!g_pan_y&&g_zoom_percent==100);
 event.control.action=OS_CONTROL_CLICK;assert(handle_event(&event)&&g_picker.active);
 event=(os_window_event_t){.type=OS_WINDOW_EVENT_CLOSE_REQUEST};
 assert(handle_event(&event)&&!g_picker.active&&g_running);
 event=(os_window_event_t){.type=OS_WINDOW_EVENT_INPUT,.input={.type=OS_INPUT_EVENT_KEY,.code='+',.value=OS_INPUT_VALUE_PRESS}};
 assert(handle_event(&event)&&g_zoom_percent==125);
 (void)g_drawn_width;(void)g_drawn_height;(void)g_drawn_zoom;(void)g_caption_height;
 (void)g_drawn_pan_x;(void)g_drawn_pan_y;(void)g_drawn_image;(void)g_image_pixels;(void)g_image_stride;
 puts("Image picker: click/stale-event validation, common picker cancel, failed decode retention, successful replacement and cache invalidation PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'test.c';c.write_text(code);exe=Path(tmp)/'test'
 subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),'-I'+str(root),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
