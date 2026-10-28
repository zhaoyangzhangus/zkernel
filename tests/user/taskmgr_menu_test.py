"""编译真实任务管理器，验证纯控件树绘制和交互。"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'user/desktop/taskmgr/main.c').read_text()

def function(name):
    match = re.search(r'^static [^\n]*\b'+name+r'\([^;{}]*\)\s*\{', source, re.M)
    assert match, name
    start = match.start()
    end = source.index('{', start)+1
    depth = 1
    while depth:
        depth += (source[end] == '{')-(source[end] == '}')
        end += 1
    return source[start:end]

definitions = source[source.index('#define TASKMGR_MAP_BASE'):source.index('void __main(void)')]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <uapi/all.h>
#include <uapi/window_theme.h>
#include <kernel/control_tree.h>
#include "user/runtime/text.h"
#include "user/runtime/caption.h"
#include "user/runtime/caption_tree.h"
''' + definitions + r'''
static unsigned submits,updates,actions,refreshes;static bool thread_action,fail;
static uint64_t revision;
static os_control_node_t captured[OS_CONTROL_TREE_MAX_NODES];
static control_tree_box_t boxes[OS_CONTROL_TREE_MAX_NODES];
static char labels[OS_CONTROL_TREE_MAX_TEXT];static unsigned count;
static unsigned text_bytes,viewport_width,viewport_height;
static int64_t taskmgr_syscall_one(uint64_t n,uint64_t p){
 if(n==OS_SYS_WINDOW_UPDATE){
  os_window_update_t *u=(void*)(uintptr_t)p;
  assert(u->flags==OS_WINDOW_CAPTION_HEIGHT(user_caption_height()));++updates;return 0;
 }
 assert(n==OS_SYS_WINDOW_CONTROL_TREE);++submits;
 if(fail)return -12;
 os_window_control_tree_t *r=(void*)(uintptr_t)p;
 assert(r->identifier==7&&r->hdr.size==sizeof(*r)&&r->hdr.version==OS_SYSCALL_ABI_VERSION);
 if(count==r->count&&text_bytes==r->text_size&&viewport_width==g_window.width&&viewport_height==g_window.height&&
    !memcmp(captured,(void*)(uintptr_t)r->nodes,count*sizeof(*captured))&&
    !memcmp(labels,(void*)(uintptr_t)r->text,text_bytes)){r->revision=revision;return 0;}
 text_bytes=r->text_size;viewport_width=g_window.width;viewport_height=g_window.height;
 count=r->count;assert(count&&count<=OS_CONTROL_TREE_MAX_NODES);
 memcpy(captured,(void*)(uintptr_t)r->nodes,count*sizeof(*captured));
 memcpy(labels,(void*)(uintptr_t)r->text,r->text_size);
 assert(control_tree_layout(captured,count,r->text_size,control_tree_window_viewport(captured,count,g_window.width,g_window.height,OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_CAPTION_HEIGHT(user_caption_height())),boxes));
 for(unsigned i=0;i<count;++i)assert(captured[i].kind!=OS_CONTROL_SURFACE);
 r->revision=++revision;return 0;
}
static void end_selected_process(bool thread_only){
 ++actions;thread_action=thread_only;g_confirm_pid=42;g_confirm_tid=thread_only?43:0;
}
static void refresh_processes(void){++refreshes;}
static void taskmgr_exit(uint64_t status){(void)status;assert(!"unexpected exit");}
''' + '\n'.join(function(n) for n in (
    'taskmgr_scale','text_length','copy_text','append_text','append_u64','taskmgr_summary','set_status',
    'visible_index_rebuild','entry_visible','visible_entry','entry_row','visible_count',
    'process_state_text','thread_state_text','menu_width','menu_rows',
    'menu_next_generation','menu_target','dismiss_menu','visible_rows',
    'ensure_selection_visible','select_delta','tree_text','table_label','build_table',
    'render','key_is','handle_menu_event','handle_event')) + r'''
static unsigned index_of(unsigned id){
 for(unsigned i=0;i<count;++i)if(captured[i].identifier==id)return i;
 assert(!"missing node");return 0;
}
static os_window_event_t event(unsigned id,unsigned action,unsigned code,int value){
 unsigned i=index_of(id);const os_control_node_t *n=g_nodes+i;
 return (os_window_event_t){.type=OS_WINDOW_EVENT_CONTROL,.pointer_x=300,.pointer_y=140,
  .control={.target=id,.action=action,.code=code,.value=value,
   .node_index=i,.node_kind=n->kind,.tree_revision=g_tree.revision,.user_data=n->user_data}};
}
static bool text_is(unsigned id,const char *text){
 const os_control_node_t *n=captured+index_of(id);
 return n->text_length==strlen(text)&&!memcmp(labels+n->text_offset,text,n->text_length);
}
static void send(unsigned id,unsigned action,unsigned code,int value){
 os_window_event_t e=event(id,action,code,value);handle_event(&e);
}
static void verify_menu(void){
 unsigned font=ui_text_size();
 unsigned panel=index_of(menu_target(1));
 int width=boxes[panel].bounds.x1-boxes[panel].bounds.x0;
 int height=boxes[panel].bounds.y1-boxes[panel].bounds.y0;
 int inset=(font+2U)/4U;
 int px=width>=2*inset?inset:width/2,py=height>=2*inset?inset:height/2;
 assert(captured[panel].flags&OS_CONTROL_MENU_PANEL);
 assert(captured[panel].height==0);
 assert(boxes[panel].bounds.x0>=0&&boxes[panel].bounds.y0>=0);
 assert(boxes[panel].bounds.x1<=(int)g_window.width&&boxes[panel].bounds.y1<=(int)g_window.height);
 assert(height==(int)(os_control_menu_height(font,menu_rows())<g_window.height?
     os_control_menu_height(font,menu_rows()):g_window.height));
 assert(boxes[panel].style.background==OS_THEME_SURFACE);
 for(unsigned row=0;row<menu_rows();++row){
  unsigned i=index_of(menu_target(2+row));
  assert(boxes[i].style.font_size==font);
  assert(boxes[i].bounds.x0==boxes[panel].bounds.x0+px);
  assert(boxes[i].bounds.x1==boxes[panel].bounds.x1-px);
  assert(boxes[i].bounds.y0==boxes[panel].bounds.y0+py+(int)(row*font*2U));
  int left=boxes[i].bounds.x0>boxes[i].clip.x0?boxes[i].bounds.x0:boxes[i].clip.x0;
  int top=boxes[i].bounds.y0>boxes[i].clip.y0?boxes[i].bounds.y0:boxes[i].clip.y0;
  int right=boxes[i].bounds.x1<boxes[i].clip.x1?boxes[i].bounds.x1:boxes[i].clip.x1;
  int bottom=boxes[i].bounds.y1<boxes[i].clip.y1?boxes[i].bounds.y1:boxes[i].clip.y1;
  if(boxes[i].enabled&&left<right&&top<bottom)
   assert(control_tree_locate(captured,count,control_tree_window_viewport(captured,count,g_window.width,g_window.height,OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_CAPTION_HEIGHT(user_caption_height())),left,top,0)==i);
 }
}
int main(void){
 assert(ui_text_init(OS_TEXT_DEFAULT_SIZE));
 g_window.identifier=7;g_window.width=1040;g_window.height=680;
 /* 禁止访问像素，遗留的应用绘制路径会立即失败。 */
 g_window.pixels=(void*)(uintptr_t)1;
 g_entry_count=3;g_entries[0]=(taskmgr_entry_t){.id=42,.expanded=true,.cpu=12345,.cpu_sample_valid=true,
  .state=OS_PROCESS_RUNNING,.resources={.rss_bytes=12582912}};
 strcpy(g_entries[0].name,"中文进程");
 g_entries[1]=(taskmgr_entry_t){.thread=true,.id=43,.parent_id=42,.state=OS_THREAD_BLOCKED};
 strcpy(g_entries[1].name,"worker");
 g_entries[2]=(taskmgr_entry_t){.id=99,.resources={.flags=OS_TASK_KERNEL|OS_TASK_PROTECTED}};
 strcpy(g_entries[2].name,"compositor");
 g_selection_valid=true;render();assert(count>20&&updates==1&&!g_menu_published);
 assert(text_is(10,"名称")&&text_is(0x10002,"中文进程"));
 /* 汇总只在标题栏，客户区不再保留底栏。 */
 assert(captured[index_of(6)].parent==0);
 assert(boxes[index_of(6)].bounds.y1<=0);
 assert(boxes[index_of(3)].bounds.y1==680);
 for(unsigned i=0;i<count;++i)assert(captured[i].identifier!=4&&captured[i].identifier!=5);

 assert(text_is(0x10005,"12.345%")&&text_is(0x10006,"12.0"));
 assert(text_is(0x1000e,"-")&&text_is(0x10013,"-"));
 uint64_t stable=g_tree.revision;
 for(unsigned i=0;i<100;++i)render();assert(updates==1&&g_tree.revision==stable);
 unsigned hit=control_tree_locate(captured,count,control_tree_window_viewport(captured,count,1040,680,OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_CAPTION_HEIGHT(user_caption_height())),100,TASKMGR_TABLE_TOP+6,0);
 assert(captured[hit].identifier==0x10000&&captured[hit].user_data==0);
 send(0x10001,OS_CONTROL_CLICK,OS_INPUT_BUTTON_LEFT,0);assert(!g_entries[0].expanded&&visible_count()==2);
 send(0x10001,OS_CONTROL_CLICK,OS_INPUT_BUTTON_LEFT,0);assert(g_entries[0].expanded&&visible_count()==3);
 send(0x10008,OS_CONTROL_DOWN,OS_INPUT_BUTTON_LEFT,1);assert(g_selected_entry==1);
 send(0x10008,OS_CONTROL_DOWN,OS_INPUT_BUTTON_RIGHT,1);assert(g_menu_open&&g_menu_published);
 assert(boxes[index_of(3)].inert&&!boxes[index_of(3)].enabled);
 assert(boxes[index_of(0x10008)].inert);
 assert(captured[index_of(0x10008)].flags&OS_CONTROL_CHECKED);
 assert(text_is(menu_target(2),"结束线程")&&text_is(menu_target(3),"结束进程"));verify_menu();
 os_window_event_t stale=event(menu_target(2),OS_CONTROL_CLICK,OS_INPUT_BUTTON_LEFT,0);
 render();assert(g_tree.revision==stale.control.tree_revision);
 ++g_entries[0].cpu;render();handle_event(&stale);assert(!actions);
 send(menu_target(2),OS_CONTROL_CLICK,OS_INPUT_BUTTON_LEFT,0);assert(actions==1&&thread_action);
 assert(text_is(menu_target(2),"确认结束线程"));
 handle_event(&stale);assert(actions==1);
 send(menu_target(3),OS_CONTROL_CLICK,OS_INPUT_BUTTON_LEFT,0);assert(actions==2&&!thread_action);
 assert(text_is(menu_target(3),"确认结束进程"));
 send(menu_target(0),OS_CONTROL_DOWN,OS_INPUT_BUTTON_LEFT,1);assert(!g_menu_open&&!g_menu_published&&count);
 send(1,OS_CONTROL_KEY,0xe0,1);send(1,OS_CONTROL_KEY,0x2e,1);send(1,OS_CONTROL_KEY,0xe0,0);
 assert(ui_text_size()==20&&updates==2&&boxes[index_of(0x10002)].style.font_size==20);
 /* 非菜单状态的普通窗口键盘是原始输入，缩放还应同步改变行高和展开控件的命中范围。 */
 int before_width=captured[index_of(0x10001)].width;
 int before_height=captured[index_of(0x10000)].height;
 os_window_event_t raw={.type=OS_WINDOW_EVENT_INPUT,.input={.type=OS_INPUT_EVENT_KEY,.code=0xe0,.value=1}};
 handle_event(&raw);raw.input.code=0x2e;handle_event(&raw);
 assert(ui_text_size()==24&&captured[index_of(0x10000)].height>before_height);
 assert(captured[index_of(0x10001)].width>before_width);
 assert(boxes[index_of(0x10002)].style.font_size==24);
 raw.input.code=0x2d;handle_event(&raw);assert(ui_text_size()==20);
 raw.input.type=OS_INPUT_EVENT_RELATIVE;raw.input.code=OS_INPUT_REL_WHEEL;raw.input.value=1;
 handle_event(&raw);assert(ui_text_size()==24);
 raw.input.type=OS_INPUT_EVENT_KEY;raw.input.code=0xe0;raw.input.value=0;
 handle_event(&raw);assert(!g_ctrl);
 /* 修饰键释放即使排在旧快照后，也不能因自动刷新而遗失。 */
 os_window_event_t delayed=event(1,OS_CONTROL_KEY,0xe0,0);
 send(1,OS_CONTROL_KEY,0xe0,1);++g_entries[0].cpu;render();
 assert(delayed.control.tree_revision<g_tree.revision);handle_event(&delayed);assert(!g_ctrl);
 send(1,OS_CONTROL_KEY,0x15,1);send(1,OS_CONTROL_KEY,0x3e,1);assert(refreshes==0);
 send(0x10010,OS_CONTROL_DOWN,OS_INPUT_BUTTON_RIGHT,1);
 assert(captured[index_of(menu_target(2))].flags&OS_CONTROL_DISABLED);
 dismiss_menu();render();fail=true;render();assert(!g_tree.count&&!g_menu_published);
 handle_event(&stale);assert(actions==2);fail=false;render();assert(g_tree.count);
 for(unsigned font=OS_TEXT_MIN_SIZE;font<=OS_TEXT_MAX_SIZE;font+=OS_TEXT_SIZE_STEP){
  assert(ui_text_init(font));
  g_selected_entry=1;g_menu_open=true;menu_next_generation();render();verify_menu();
 }
 for(unsigned w=1;w<=100;w+=7)for(unsigned h=1;h<=80;h+=7){
  g_window.width=w;g_window.height=h;g_menu_x=UINT32_MAX;g_menu_y=UINT32_MAX;
  g_selected_entry=1;g_menu_open=true;menu_next_generation();render();assert(g_menu_published);verify_menu();
 }
 assert(ui_text_init(OS_TEXT_MIN_SIZE));g_window.width=1920;g_window.height=4096;dismiss_menu();
 g_entry_count=TASKMGR_MAX_ENTRIES;g_visible_dirty=true;
 for(unsigned i=0;i<g_entry_count;++i){g_entries[i]=(taskmgr_entry_t){.id=i+1};strcpy(g_entries[i].name,"entry");}
 render();assert(g_tree.count&&count<OS_CONTROL_TREE_MAX_NODES);
 assert(g_tree.text_size<OS_CONTROL_TREE_MAX_TEXT&&count<g_entry_count*7);
 ui_text_shutdown();
 puts("Task manager tree: zero pixel access, visible rows, CPU/RSS labels, indexed selection/expansion, menu confirmation, stale events, failure recovery, fonts/tiny viewports/maximum list PASS");
}
'''
assert not any(name in source for name in ('fill_rect(', 'draw_row(', 'ui_text_draw', 'sync_context_menu('))
with tempfile.TemporaryDirectory(prefix='taskmgr-tree-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-Wno-unused-variable','-Wno-unused-function','-Wno-misleading-indentation',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root),'-I'+str(root/'include'),str(path/'test.c'),
                    str(root/'kernel/graphics/control_tree.c'),str(root/'user/runtime/text_layout.c'),
                    '-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
