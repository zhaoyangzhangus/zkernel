"""执行真实编辑器控件树与模态子树发布，不允许应用写像素。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/desktop/notepad/main.c').read_text()

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
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <uapi/all.h>
#include <uapi/control_validation.h>
#include <kernel/control_tree.h>
#include "user/runtime/message_box.h"
#include "user/runtime/file_picker.h"
#include "user/runtime/modal_tree.h"
#include "user/runtime/caption.h"
#include "user/runtime/caption_tree.h"
#undef OS_TEXT_WIDTH
#define OS_TEXT_WIDTH os_control_text_cell_width(' ',ui_text_size())
static uint32_t font=20,submissions,updates,titles;
static bool fail_tree;
uint32_t ui_text_size(void){return font;}
uint32_t ui_text_line_height(void){return font+4;}
uint32_t ui_text_measure_range(const char *text,size_t length){(void)text;return (uint32_t)length*(font/2U);}
static int64_t notepad_syscall_one(uint64_t number,uint64_t address){
 if(number==OS_SYS_WINDOW_TITLE){++titles;return 0;}
 if(number==OS_SYS_WINDOW_UPDATE){++updates;return 0;}
 assert(number==OS_SYS_WINDOW_CONTROL_TREE);++submissions;
 os_window_control_tree_t *t=(void *)(uintptr_t)address;
 os_control_node_t *nodes=(void *)(uintptr_t)t->nodes;
 assert(os_control_tree_validate(nodes,t->count,t->text_size));
 control_tree_box_t boxes[OS_CONTROL_TREE_MAX_NODES];
 assert(control_tree_layout(nodes,t->count,t->text_size,
  control_tree_window_viewport(nodes,t->count,640,480,OS_WINDOW_CAPTION_HEIGHT(user_caption_height())),boxes));
 for(unsigned i=0;i<t->count;++i)assert(nodes[i].kind!=OS_CONTROL_SURFACE);
 t->revision=submissions;return fail_tree?-1:0;
}
'''
state = source[source.index('#define NOTEPAD_TEXT_CAPACITY'):source.index('/* MinGW')]
body = ''.join(function(signature) for signature in [
    'static void fill_rect(',
    'static void draw_character(', 'static uint32_t editor_columns(',
    'static uint32_t editor_visible_lines(', 'static void prepare_position(',
    'static void advance_position(', 'static void cursor_position(', 'static size_t offset_at_position(',
    'static void ensure_cursor_visible(', 'static void draw_editor(',
    'static bool sync_caption_tree(', 'static bool sync_message_tree(',
    'static bool sync_file_picker_tree(', 'static void update_window(', 'static void render('])
body += ''.join(function(signature) for signature in [
    'static void snapshot_capture(', 'static void snapshot_restore(',
    'static void push_snapshot(', 'static bool pop_snapshot(', 'static void begin_edit(',
    'static bool editor_control_event_valid(',
    'static bool has_selection(', 'static void erase_selection(',
    'static void backspace(', 'static void delete_character('])
main = r'''
static unsigned kind_count(unsigned kind){unsigned n=0;for(unsigned i=0;i<g_node_count;++i)
 if(g_nodes[i].kind==kind)++n;return n;}
static unsigned document_text_count(void){unsigned n=0;for(unsigned i=0;i<g_node_count;++i)
 if(g_nodes[i].kind==OS_CONTROL_TEXT&&g_nodes[i].value==OS_CONTROL_TEXT_APP_SELECTION)++n;return n;}
static size_t caption_length(void){char title[32];user_caption_format_app_title(title,"记事本",g_file_path,g_dirty);return strlen(title);}
static unsigned find_node(unsigned identifier){unsigned i=0;while(i<g_node_count&&g_nodes[i].identifier!=identifier)++i;assert(i<g_node_count);return i;}
int main(void){
 g_window=(notepad_window_t){.identifier=7,.width=640,.height=480,.pixels=(void *)1};
 strcpy(g_text,"abc\tde\nxy");g_text_length=strlen(g_text);g_cursor=g_text_length;
 render();assert(g_tree.count && submissions==1 && updates==1);
 assert(document_text_count()==2 && kind_count(OS_CONTROL_TEXT)==3 && g_text_used==14+caption_length());
 assert(!memcmp(g_tree_text,"abc dexy",8));
 unsigned title=find_node(0x7fffffe1U);
 assert(g_nodes[title].text_length==sizeof("记事本 - /notes.txt")-1U &&
     !memcmp(g_tree_text+g_nodes[title].text_offset,"记事本 - /notes.txt",sizeof("记事本 - /notes.txt")-1U));
 unsigned first_updates=updates;render();assert(updates==first_updates);
 os_control_event_t queued={.target=0x20000000U,.node_index=0,
  .node_kind=OS_CONTROL_CONTAINER,.tree_revision=g_tree.revision,.action=OS_CONTROL_UP};
 ++g_tree.revision;assert(!editor_control_event_valid(&queued));
 g_mouse_selecting=true;assert(editor_control_event_valid(&queued));
 queued.action=OS_CONTROL_MOVE;assert(editor_control_event_valid(&queued));
 queued.action=OS_CONTROL_DOWN;assert(!editor_control_event_valid(&queued));
 queued.action=OS_CONTROL_UP;g_message.active=true;assert(!editor_control_event_valid(&queued));
 g_message.active=false;g_picker.active=true;assert(!editor_control_event_valid(&queued));
 g_picker.active=false;queued.user_data=1;assert(!editor_control_event_valid(&queued));
 queued.user_data=0;queued.tree_revision=g_tree.revision+1;
 assert(!editor_control_event_valid(&queued));g_mouse_selecting=false;
 g_selection_anchor=0;render();
 assert(kind_count(OS_CONTROL_CONTAINER)==5 && document_text_count()==2);
 /* 模态根必须覆盖禁用的编辑器子树，不能丢失正文或错误重映射父索引。 */
 ui_message_show(&g_message,"Unsaved","Keep edits?",OS_MESSAGE_SAVE_DISCARD_CANCEL);
 g_pending=NOTEPAD_PENDING_CLOSE;render();
 assert(g_nodes[0].identifier==ui_message_target(&g_message,0));
 assert(g_nodes[1].identifier==0x20000000U && g_nodes[1].parent==0);
 assert(g_nodes[1].flags&OS_CONTROL_DISABLED);
 assert(kind_count(OS_CONTROL_BUTTON)==5);
 for(unsigned i=1;i<g_node_count;++i)assert(g_nodes[i].parent<i);
 g_message.active=false;render();assert(g_nodes[0].identifier==0x20000000U);
 g_picker.active=true;g_picker.generation=1;strcpy(g_picker.directory,"/");
 render();assert(g_tree.count && g_nodes[0].identifier==ui_file_picker_target(&g_picker,0));
 assert(g_nodes[1].flags&OS_CONTROL_DISABLED);ui_file_picker_close(&g_picker);render();
 /* 提交失败只取消确认动作，绝不能丢弃未保存文档。 */
 fail_tree=true;g_dirty=true;render();assert(!g_tree.count && g_dirty && g_text_length==9);
 fail_tree=false;render();assert(g_tree.count && g_dirty);
 /* 最大文本和跨行选区按行合并，节点数量不能随字符数量增长。 */
 memset(g_text,'a',8191);g_text[8191]=0;g_text_length=g_cursor=8191;g_selection_anchor=0;
 g_window.height=4096;render();assert(g_tree.count && g_node_count<512 && g_text_used==8197+caption_length());
 for(unsigned size=1;size<64;++size){g_window.width=size;g_window.height=size;render();assert(g_tree.count);}
 /* 极端尺寸耗尽节点时拒绝发布不完整画面，恢复正常大小后可重试。 */
 g_window.width=21;g_window.height=65536;g_scroll_line=0;g_follow_cursor=false;
 unsigned old_submissions=submissions;render();assert(g_tree_overflow&&!g_tree.count&&submissions==old_submissions);
 g_window.width=640;g_window.height=480;render();assert(g_tree.count);
 for(font=14;font<=50;font+=4){
  render();assert(g_nodes[0].font_size==font);
  control_tree_box_t boxes[OS_CONTROL_TREE_MAX_NODES],hit;
  Rect viewport=control_tree_window_viewport(g_nodes,g_node_count,g_window.width,g_window.height,
      OS_WINDOW_CAPTION_HEIGHT(user_caption_height()));
  assert(control_tree_layout(g_nodes,g_node_count,g_text_used,viewport,boxes));
  unsigned button=find_node(0x1ffffff1U);
  assert(g_nodes[button].identifier==0x1ffffff1U&&g_nodes[button].kind==OS_CONTROL_BUTTON);
  assert(g_nodes[button].text_length==6&&!memcmp(g_tree_text+g_nodes[button].text_offset,"打开",6));
  assert(boxes[button].bounds.y0<0&&boxes[button].bounds.y1<=0);
  assert(boxes[button].bounds.x1<=(int)g_window.width-3*(int)os_window_caption_button_width(OS_WINDOW_CAPTION_HEIGHT(user_caption_height())));
  assert(control_tree_locate(g_nodes,g_node_count,viewport,
      (boxes[button].bounds.x0+boxes[button].bounds.x1)/2,
      (boxes[button].bounds.y0+boxes[button].bounds.y1)/2,&hit)==button);
  for(unsigned action=OS_CONTROL_WINDOW_MINIMIZE;action<=OS_CONTROL_WINDOW_CLOSE;++action) {
   unsigned control=find_node(0x7fffffe1U+action);Rect b=boxes[control].bounds;
   assert(g_nodes[control].kind==OS_CONTROL_WINDOW_BUTTON && g_nodes[control].value==action);
   assert(b.x0>=0 && b.x1<=(int)g_window.width && b.y0==-(int)user_caption_height() && b.y1==0);
   assert(control_tree_locate(g_nodes,g_node_count,viewport,(b.x0+b.x1)/2,(b.y0+b.y1)/2,0)==control);
  }
  assert(boxes[1].bounds.y0==0&&boxes[1].bounds.y1==(int)g_window.height);
 }
 /* 以字节偏移存储文档，以完整码点定位、绘制和删除。 */
 font=20;g_window.width=640;g_window.height=480;g_scroll_line=0;g_follow_cursor=true;
 const char mixed[]="A\xe4\xb8\xad\xf0\x9f\x98\x80" "B";
 strcpy(g_text,mixed);g_text_length=strlen(mixed);g_cursor=g_text_length;g_selection_anchor=SIZE_MAX;
 render();assert(g_text_used==g_text_length+6+caption_length()&&!memcmp(g_tree_text,mixed,g_text_length));
 assert(document_text_count()==1);
 uint32_t line,column;cursor_position(3,&line,&column);assert(line==2&&column==0);
 g_cursor=4;cursor_position(2,&line,&column);assert(line==2&&column==0);
 assert(offset_at_position(3,0,2)==4);
 g_cursor=4;backspace();assert(!strcmp(g_text,"A\xf0\x9f\x98\x80" "B")&&g_cursor==1);
 assert(pop_snapshot(&g_undo)&&g_cursor==4&&!strcmp(g_text,mixed));
 g_cursor=1;delete_character();assert(!strcmp(g_text,"A\xf0\x9f\x98\x80" "B")&&g_cursor==1);
 assert(pop_snapshot(&g_undo)&&g_cursor==1&&!strcmp(g_text,mixed));
 g_cursor=8;g_selection_anchor=1;begin_edit();erase_selection();assert(!strcmp(g_text,"AB")&&g_cursor==1);
 assert(pop_snapshot(&g_undo)&&g_cursor==8&&!strcmp(g_text,mixed));
 g_selection_anchor=SIZE_MAX;g_cursor=g_text_length;
 while(g_cursor){size_t before=g_text_length;backspace();assert(g_text_length<before);}
 assert(!g_text_length&&!g_text[0]);
 /* 独立的码点布局参考：先计算边界与展开后的字符位置，再比对真实树。 */
 uint32_t seed=0x82abcU;
 for(unsigned sample=0;sample<100;++sample)for(unsigned cols=1;cols<=16;++cols){
  uint32_t codes[64],lines[65],columns[65],expected_code[256],expected_x[256],expected_y[256];
  size_t offsets[65],length=0;unsigned expected_count=0,row=0,col=0;
  const uint32_t choices[]={'A','B','\n','\t',0x4e2d,0x6587,0xe9,0x1f600};
  for(unsigned i=0;i<64;++i){
   seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;codes[i]=choices[seed%8];
   unsigned cells=codes[i]<128?1:2;if(cells>cols)cells=cols;
   if(codes[i]!='\n'&&codes[i]!='\t'&&col+cells>cols){++row;col=0;}
   offsets[i]=length;lines[i]=row;columns[i]=col;
   length+=os_utf8_encode(codes[i],g_text+length);
   unsigned count=codes[i]=='\n'?0:codes[i]=='\t'?4-col%4:1;
   if(codes[i]=='\t'&&count>cols-col)count=cols-col;
   for(unsigned j=0;j<count;++j){
    expected_code[expected_count]=codes[i]=='\t'?' ':codes[i];
    expected_x[expected_count]=10+(col+j)*10;
    expected_y[expected_count++]=row*24;
   }
   if(codes[i]=='\n'){++row;col=0;}
   else {col=codes[i]=='\t'?(col/4+1)*4:col+cells;if(col>=cols){++row;col=0;}}
  }
  offsets[64]=length;lines[64]=row;columns[64]=col;g_text[length]=0;g_text_length=length;
  for(unsigned i=0;i<=64;++i){g_cursor=offsets[i];cursor_position(cols,&line,&column);
   assert(line==lines[i]&&column==columns[i]);}
  for(unsigned y=0;y<=row;++y)for(unsigned x=0;x<cols;++x){
   size_t found=offset_at_position(cols,y,x);bool boundary=false;
   for(unsigned i=0;i<=64;++i)if(found==offsets[i])boundary=true;
   assert(boundary);
  }
  g_cursor=length;g_selection_anchor=SIZE_MAX;g_window.width=20+cols*10;
  g_window.height=(row+2)*24;g_scroll_line=0;g_follow_cursor=false;render();
  unsigned actual=0;
  for(unsigned i=0;i<g_node_count;++i)if(g_nodes[i].kind==OS_CONTROL_TEXT&&g_nodes[i].value==OS_CONTROL_TEXT_APP_SELECTION){
   unsigned x=(unsigned)g_nodes[i].x;
   for(size_t byte=0;byte<g_nodes[i].text_length;){size_t used;
    uint32_t code=os_utf8_next(g_tree_text+g_nodes[i].text_offset+byte,g_nodes[i].text_length-byte,&used);
    assert(actual<expected_count&&code==expected_code[actual]&&x==expected_x[actual]&&
     (unsigned)g_nodes[i].y==expected_y[actual]);
    ++actual;x+=os_control_text_cell_width(code,font);byte+=used;
   }
  }
  assert(actual==expected_count);
 }
 puts("Editor tree: no pixel access, text/selection batching, modal reparenting, no Surface, failure retention, capacity bounds and retry PASS");
 puts("Editor UTF-8: full sequences, wide wrapping, hit boundaries, whole-codepoint deletion, selection and undo PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='notepad-tree-') as directory:
    c = Path(directory) / 'test.c'
    c.write_text(prefix + state + body + main)
    exe = Path(directory) / 'test'
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-Wno-unused-function', '-Wno-misleading-indentation',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-I' + str(root), str(c),
                    str(root / 'kernel/graphics/control_tree.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
