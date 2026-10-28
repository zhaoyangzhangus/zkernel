"""终端真实控件树发布、文本裁剪、选区和刷新期间的键盘配对。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/desktop/gshell/main.c').read_text()

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uapi/all.h>
#include <uapi/window_theme.h>
#include <uapi/control_validation.h>
#include <uapi/utf8.h>
#include <kernel/control_tree.h>
#include "user/runtime/caption_tree.h"
#define OS_TEXT_HEIGHT 24U
#define OS_TEXT_WIDTH 8U
static uint32_t font=20,updates,submissions,test_width=640,test_height=480;
static bool fail_tree;
static uint32_t ui_text_size(void){return font;}
static uint32_t user_caption_height(void){return font+20;}
static int64_t gshell_syscall_one(uint64_t number,uint64_t address){
 if(number==OS_SYS_WINDOW_UPDATE){++updates;return 0;}
 assert(number==OS_SYS_WINDOW_CONTROL_TREE);++submissions;
 os_window_control_tree_t *t=(void *)(uintptr_t)address;
 os_control_node_t *nodes=(void *)(uintptr_t)t->nodes;
 assert(os_control_tree_validate(nodes,t->count,t->text_size));
 control_tree_box_t boxes[OS_CONTROL_TREE_MAX_NODES];
 Rect viewport=control_tree_window_viewport(nodes,t->count,test_width,test_height,
     OS_WINDOW_CAPTION_HEIGHT(user_caption_height()));
 assert(control_tree_layout(nodes,t->count,t->text_size,viewport,boxes));
 assert((nodes[0].flags&(OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_WINDOW_FRAME))==
     (OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_WINDOW_FRAME));
 for(unsigned i=0;i<t->count;++i){
  assert(nodes[i].kind!=OS_CONTROL_SURFACE);
  if(i>1 && nodes[i].kind!=OS_CONTROL_WINDOW_BUTTON)assert(nodes[i].flags&OS_CONTROL_POINTER_PASSTHROUGH);
 }
 assert(t->count>=5 && nodes[t->count-4].kind==OS_CONTROL_TEXT);
 assert(nodes[t->count-4].text_length==6 &&
     !memcmp((const char *)(uintptr_t)t->text+nodes[t->count-4].text_offset,"终端",6));
 for(unsigned i=0;i<3;++i) {
  unsigned index=t->count-3+i;
  assert(nodes[index].kind==OS_CONTROL_WINDOW_BUTTON && nodes[index].value==OS_CONTROL_WINDOW_MINIMIZE+i);
  if(test_width>=3U) {
   Rect b=boxes[index].bounds;
   assert(b.x0>=0 && b.x1<=(int)test_width && b.y0==-(int)user_caption_height() && b.y1==0);
   assert(control_tree_locate(nodes,t->count,viewport,(b.x0+b.x1)/2,(b.y0+b.y1)/2,0)==index);
  }
 }
 t->revision=submissions;return fail_tree?-1:0;
}
'''
assert 'shell_damage_' not in source and 'g_damage_' not in source
state = source[source.index('#define SHELL_LINE_CAPACITY'):source.index('static int64_t stat_path(')]
state = state.replace('static bool handle_event(const os_window_event_t *event);', '')
body = ''.join(function(signature) for signature in [
    'static uint32_t text_length(',
    'static void append_text(', 'static char *shell_output_line('])
body += source[source.index('static void fill_rect('):source.index('static bool create_window(')]
body += function('static bool shell_control_event_valid(')
main = r'''
static unsigned text_nodes(void){unsigned count=0;for(unsigned i=0;i<g_node_count;++i)
 if(g_nodes[i].kind==OS_CONTROL_TEXT&&g_nodes[i].value==OS_CONTROL_TEXT_APP_SELECTION)++count;return count;}
int main(void){
 /* 像素地址故意不可访问：任何遗留自绘都会直接失败。 */
 g_shell.identifier=4;g_shell.width=640;g_shell.height=480;g_shell.pixels=(void *)1;
 strcpy(g_output[0],"hello");g_output_count=1;
 strcpy(g_command,"echo ok");g_command_length=g_command_cursor=7;
 assert(shell_text_width("A\xe4\xb8\xad" "B",5)==40);
 render_window();assert(updates==1&&submissions==1&&text_nodes()==2);
 assert(strstr(g_tree_text,"hello"));
 unsigned previous=updates;render_window();assert(updates==previous);
 g_output_anchor=0;g_output_selection_end=4;render_window();
 assert(g_nodes[2].kind==OS_CONTROL_CONTAINER && g_nodes[2].width==40);
 assert(g_nodes[3].kind==OS_CONTROL_TEXT);
 os_control_event_t event={.target=1,.node_index=0,.node_kind=OS_CONTROL_CONTAINER,
  .tree_revision=1,.action=OS_CONTROL_KEY};
 assert(shell_control_event_valid(&event));
 event.action=OS_CONTROL_DOWN;assert(!shell_control_event_valid(&event));
 g_mouse_selecting=2;
 event.action=OS_CONTROL_UP;assert(shell_control_event_valid(&event));
 event.action=OS_CONTROL_MOVE;assert(shell_control_event_valid(&event));
 event.action=OS_CONTROL_DOWN;assert(!shell_control_event_valid(&event));
 g_mouse_selecting=0;
 event.action=OS_CONTROL_UP;assert(!shell_control_event_valid(&event));
 event.action=OS_CONTROL_KEY;event.user_data=1;assert(!shell_control_event_valid(&event));
 event.user_data=0;event.tree_revision=g_tree.revision+1;assert(!shell_control_event_valid(&event));
 fail_tree=true;render_window();assert(!g_tree.count);
 event.tree_revision=1;assert(!shell_control_event_valid(&event));
 fail_tree=false;render_window();assert(g_tree.count);
 /* 最大历史和长命令必须完整发布，不能静默耗尽节点或文字容量。 */
 for(unsigned i=0;i<128;++i){memset(g_output[i],'a'+i%26,255);g_output[i][255]=0;}
 g_output_count=128;g_shell.height=test_height=4096;g_shell.width=test_width=4096;
 memset(g_cwd,'p',255);g_cwd[255]=0;memset(g_command,'c',255);g_command[255]=0;
 g_command_length=g_command_cursor=255;g_input_anchor=0;
 g_output_anchor=0;g_output_selection_end=128*256;
 unsigned previous_submissions=submissions;
 render_window();
 assert(submissions==previous_submissions+1 && g_tree.count==g_node_count);
 assert(text_nodes()==129 && g_text_used==128*255+512-g_input_view_start+6);
 assert(g_node_count<=sizeof(g_nodes)/sizeof(g_nodes[0]));
 for(unsigned size=1;size<45;++size){g_shell.width=test_width=size;g_shell.height=test_height=size;render_window();}
 puts("Shell tree: no pixel access, shared layout, selection, full history, tiny windows, failure retry and queued keys PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='gshell-tree-') as directory:
    c = Path(directory) / 'test.c'
    c.write_text(prefix + state + body + main)
    exe = Path(directory) / 'test'
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-Wno-unused-function', '-Wno-misleading-indentation',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-I' + str(root), str(c),
                    str(root / 'kernel/graphics/control_tree.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
