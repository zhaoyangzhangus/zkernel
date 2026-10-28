"""验证当前编辑器模态子树合并、容量回滚和语义事件，不再测试已移除的 Surface 发布路径。"""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'user/desktop/notepad/main.c').read_text()
def function(signature):
    start=source.index(signature)
    end=source.index('{',start)+1
    depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]

program=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
#include "user/runtime/message_box.h"
#include "user/runtime/modal_tree.h"
static ui_message_box_t g_message;
static bool g_ctrl,g_shift,g_picker_overwrite;
static struct {bool active;} g_picker;
static void commit_file_picker(bool overwrite){assert(!overwrite);}
static struct {uint32_t identifier,width,height;} g_window={7,800,600};
static os_control_node_t g_nodes[OS_CONTROL_TREE_MAX_NODES];
static char g_tree_text[OS_CONTROL_TREE_MAX_TEXT];
static uint32_t g_node_count,g_text_used,font=16,completions,renders;
static enum ui_message_result completed;
uint32_t ui_text_size(void){return font;}
uint32_t ui_text_line_height(void){return font+4;}
static void complete_message(enum ui_message_result result){++completions;completed=result;}
static void render(void){++renders;}
static void editor(void) {
 g_nodes[0]=(os_control_node_t){.identifier=0x20000000U,.parent=OS_CONTROL_NO_PARENT,
  .kind=OS_CONTROL_CONTAINER};
 g_nodes[1]=(os_control_node_t){.identifier=0x20000001U,.parent=0,.kind=OS_CONTROL_TEXT,
  .width=120,.height=20,.text_length=6};
 memcpy(g_tree_text,"编辑",6);g_node_count=2;g_text_used=6;
}
'''+function('static bool sync_message_tree(')+function('static bool handle_message_event(')+r'''
int main(void) {
 editor();assert(sync_message_tree()&&g_node_count==2);
 for(font=12;font<=96;font+=4) {
  editor();ui_message_show(&g_message,"未保存的中文标题😀","是否保存修改？",OS_MESSAGE_SAVE_DISCARD_CANCEL);
  ui_message_document_t document;assert(ui_message_build(&g_message,7,800,600,&document));
  assert(sync_message_tree()&&g_node_count==document.request.count+1U);
  assert(g_nodes[0].identifier==ui_message_target(&g_message,0));
  assert(g_nodes[1].flags&OS_CONTROL_DISABLED);
  assert((g_nodes[1].flags&OS_CONTROL_INERT)==OS_CONTROL_INERT);
  assert(g_nodes[2].parent==1 && !memcmp(g_tree_text,"编辑",6));
  assert(control_tree_validate(g_nodes,g_node_count,g_text_used));
  control_tree_box_t boxes[OS_CONTROL_TREE_MAX_NODES];
  assert(control_tree_layout(g_nodes,g_node_count,g_text_used,(Rect){0,0,800,600},boxes));
  for(unsigned i=0;i<g_node_count;++i)assert(g_nodes[i].kind!=OS_CONTROL_SURFACE);
 }
 /* 合并前完成容量校验，不能把未保存编辑器变成半棵模态树。 */
 editor();g_node_count=OS_CONTROL_TREE_MAX_NODES;
 assert(!sync_message_tree()&&g_nodes[0].identifier==0x20000000U);
 editor();g_text_used=sizeof(g_tree_text)-1;
 assert(!sync_message_tree()&&g_nodes[0].identifier==0x20000000U);
 /* 标识冲突和非法占位子节点必须在写入前拒绝，不能破坏原应用树。 */
 font=16;editor();ui_message_document_t bad;
 assert(ui_message_build(&g_message,7,800,600,&bad));
 os_control_node_t saved[2];memcpy(saved,g_nodes,sizeof(saved));
 bad.nodes[0].identifier=g_nodes[0].identifier;
 assert(!ui_modal_tree_attach(g_nodes,&g_node_count,OS_CONTROL_TREE_MAX_NODES,
  g_tree_text,&g_text_used,sizeof(g_tree_text),&bad.request));
 assert(g_node_count==2&&g_text_used==6&&!memcmp(saved,g_nodes,sizeof(saved)));
 assert(!memcmp(g_tree_text,"编辑",6));
 assert(ui_message_build(&g_message,7,800,600,&bad));bad.nodes[2].parent=1;
 assert(!ui_modal_tree_attach(g_nodes,&g_node_count,OS_CONTROL_TREE_MAX_NODES,
  g_tree_text,&g_text_used,sizeof(g_tree_text),&bad.request));
 assert(g_node_count==2&&g_text_used==6&&!memcmp(saved,g_nodes,sizeof(saved)));
 font=120;editor();assert(!sync_message_tree()&&g_node_count==2);font=20;
 os_window_event_t event={.type=OS_WINDOW_EVENT_CONTROL,
  .control={.action=OS_CONTROL_KEY,.code=0xe1,.value=OS_INPUT_VALUE_PRESS}};
 assert(handle_message_event(&event)&&g_shift&&!renders&&!completions);
 event.control.value=OS_INPUT_VALUE_RELEASE;assert(handle_message_event(&event)&&!g_shift);
 event=(os_window_event_t){.type=OS_WINDOW_EVENT_INPUT,
  .input={.type=OS_INPUT_EVENT_KEY,.code=0xe0,.value=OS_INPUT_VALUE_PRESS}};
 assert(handle_message_event(&event)&&g_ctrl);
 event.input.value=OS_INPUT_VALUE_RELEASE;assert(handle_message_event(&event)&&!g_ctrl);
 event=(os_window_event_t){.type=OS_WINDOW_EVENT_CONTROL,
  .control={.action=OS_CONTROL_CLICK,.target=ui_message_target(&g_message,2)}};
 ui_message_show(&g_message,"新消息","正文",OS_MESSAGE_OK);
 assert(handle_message_event(&event)&&g_message.active&&!completions&&!renders);
 event.control.target=ui_message_target(&g_message,2);
 assert(handle_message_event(&event)&&!g_message.active&&completions==1&&renders==1);
 assert(completed==OS_MESSAGE_ACCEPT&&!handle_message_event(&event));
 puts("Editor message: real subtree merge, Chinese text, bounded capacity rollback, modifier release and stale/exactly-once events PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='message-tree-submission-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root),'-I'+str(root/'include'),str(path/'test.c'),
                    str(root/'kernel/graphics/control_tree.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
