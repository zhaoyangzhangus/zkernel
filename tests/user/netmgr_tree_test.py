"""直接执行网络管理器，验证无像素绘制的控件树与刷新、滚轮事件。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/desktop/settings/main.c').read_text()
start = source.index('static int64_t settings_syscall_one(')
end = source.index('__attribute__((noreturn))', start)
source = source[:start] + 'static int64_t settings_syscall_one(uint64_t,uint64_t);\n' + source[end:]
source = source.replace('#include "network_tree.h"',
                        (root / 'user/desktop/settings/network_tree.h').read_text().replace('#pragma once', ''))
source = source.replace('#include "system_info.h"', (root / 'user/desktop/settings/system_info.h').read_text().replace('#pragma once', ''))
source = '#define main settings_application_main\n' + source + '\n#undef main\n'
fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <kernel/control_tree.h>
#include <uapi/control_validation.h>
static uint32_t font_size=16,submissions,reads,system_reads;
static bool fail_system;
static bool fail_submit,fail_read;
bool ui_text_init(uint32_t size){font_size=size;return true;}
bool ui_text_adjust(int32_t direction){font_size=direction>0?font_size+4:font_size-4;return true;}
uint32_t ui_text_size(void){return font_size;}
uint32_t ui_text_line_height(void){return font_size+8;}
static int64_t settings_syscall_one(uint64_t number,uint64_t argument) {
 if(number==OS_SYS_WINDOW_CONTROL_TREE) {
  os_window_control_tree_t *tree=(void*)(uintptr_t)argument;
  assert(tree->count>0&&tree->count<=SETTINGS_NODE_CAPACITY);
  control_tree_box_t boxes[SETTINGS_NODE_CAPACITY];
  assert(control_tree_layout(g_nodes,tree->count,tree->text_size,
     control_tree_window_viewport(g_nodes,tree->count,g_window.width,g_window.height,OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_CAPTION_HEIGHT(user_caption_height())),boxes));
  for(unsigned i=0;i<tree->count;++i) {
   assert(g_nodes[i].kind!=OS_CONTROL_SURFACE);
   assert((uint64_t)g_nodes[i].text_offset+g_nodes[i].text_length<=tree->text_size);
  }
  /* 背景不吞事件，根控件必须能接到滚轮。 */
  assert(g_nodes[0].flags&OS_CONTROL_WINDOW_FRAME);
  assert(!(g_nodes[1].flags&OS_CONTROL_POINTER_PASSTHROUGH));
  ++submissions;if(fail_submit)return -12;
  tree->revision=submissions;return 0;
 }
 if(number==OS_SYS_WINDOW_UPDATE) {
  os_window_update_t *update=(void*)(uintptr_t)argument;
  assert(!update->width&&!update->height);return 0;
 }
 if(number==OS_SYS_NET_ENUM_ADAPTER){
  os_net_adapter_t *a=(void*)(uintptr_t)argument;a->count=1;a->vendor=0x10ec;
  a->device=0x8126;a->flags=OS_NET_ADAPTER_ACTIVE;return 0;
 }
 if(number==OS_SYS_THEME_SETTINGS){
  os_theme_settings_t *a=(void*)(uintptr_t)argument;
  static uint32_t theme,background;
  if(a->operation==OS_THEME_WRITE){theme=a->theme;background=a->background;}
  a->theme=theme;a->background=background;return 0;
 }
 if(number==OS_SYS_NET_GET_STATUS){++reads;return fail_read?-5:0;}
 if(number==OS_SYS_MEMORY_INFO){
  ++system_reads;if(fail_system)return -5;
  os_memory_info_t *m=(void*)(uintptr_t)argument;
  assert(m->hdr.size==sizeof(*m)&&m->hdr.version==OS_SYSCALL_ABI_VERSION);
  m->page_size=4096;m->total_pages=1048576;m->free_pages=262144;return 0;
 }
 if(number==OS_SYS_GPU_GET_INFO){
  if(fail_system)return -5;
  os_gpu_info_t *g=(void*)(uintptr_t)argument;
  assert(g->hdr.size==sizeof(*g)&&g->hdr.version==OS_SYSCALL_ABI_VERSION);
  if(g->index>=2)return -2;
  g->count=2;g->vendor_id=g->index?0x10de:0x1234;g->device_id=g->index?0x2f04:0x1111;return 0;
 }
 if(number==OS_SYS_TASK_QUERY){
  os_task_info_t *t=(void*)(uintptr_t)argument;assert(t->pid==OS_KERNEL_WORKERS_PID);
  t->cpu_count=8;return fail_system?-5:0;
 }
 if(number==OS_SYS_DISPLAY_GET_INFO){
  os_display_info_t *d=(void*)(uintptr_t)argument;d->width=1920;d->height=1080;
  return fail_system?-5:0;
 }

 assert(!"unexpected syscall");return -1;
}
static unsigned node_id(unsigned id) {
 for(unsigned i=0;i<g_tree.count;++i)if(g_published_nodes[i].identifier==id)return i;
 assert(!"missing node");return 0;
}
static void click_id(unsigned id) {
 unsigned i=node_id(id);os_window_event_t e={.type=OS_WINDOW_EVENT_CONTROL,.control={
  .target=id,.node_index=i,.node_kind=g_published_nodes[i].kind,
  .tree_revision=g_tree.revision,.action=OS_CONTROL_CLICK}};handle_event(&e);
}
static bool contains_text(const char *text) {
 for(unsigned i=0;i<g_tree.count;++i){const os_control_node_t *n=g_published_nodes+i;
  if(n->text_length==strlen(text)&&!memcmp(g_published_text+n->text_offset,text,n->text_length))return true;}
 return false;
}
int main(void) {
 g_window.identifier=7;g_window.pixels=(void*)1;
 g_window.width=720;g_window.height=430;
 fail_submit=true;render();assert(!g_tree.count);
 fail_submit=false;
 unsigned widths[]={1,32,320,520,720,1920},heights[]={1,24,320,430,1080};
 for(unsigned f=14;f<=50;f+=4)for(unsigned w=0;w<6;++w)
  for(unsigned h=0;h<5;++h)for(unsigned state=0;state<8;++state) {
   font_size=f;g_window.width=widths[w];g_window.height=heights[h];
   g_status_valid=state!=0;g_status.flags=state;
   render();assert(g_tree.count&&!g_tree_overflow);
  }
 g_window.width=720;g_window.height=430;font_size=16;
 /* 设置必须包含独立网络栏目；只由已发布的真实刷新按钮触发主动读取。 */
 render();unsigned network_index=node_id(1001);
 assert(network_index&&g_published_nodes[network_index].kind==OS_CONTROL_LIST_ITEM);
 assert(g_published_nodes[network_index].flags&OS_CONTROL_CHECKED);
 unsigned nav_reads=reads;click_id(1001);assert(reads==nav_reads+1);
 click_id(1001);assert(reads==nav_reads+2);
 assert(!contains_text("每 500 毫秒自动刷新"));
 uint64_t old_revision=g_tree.revision;
 g_status_valid=!g_status_valid;
 fail_submit=true;render();assert(g_tree.count&&g_tree.revision==old_revision);
 fail_submit=false;render();assert(g_tree.count&&g_tree.revision!=old_revision);
 g_window.width=720;render();
 unsigned before_submit=submissions;
 for(unsigned i=0;i<1000;++i)render();
 assert(submissions==before_submit);
 assert(g_tree.nodes==(uintptr_t)g_published_nodes&&g_tree.text==(uintptr_t)g_published_text);
 unsigned before=reads;assert(refresh_status());assert(reads==before+1);
 fail_read=true;assert(!refresh_status()&&!g_status_valid);fail_read=false;
 os_window_event_t event={.type=OS_WINDOW_EVENT_CONTROL,.control={
  .target=1,.node_index=0,.node_kind=OS_CONTROL_CONTAINER,.tree_revision=g_tree.revision,
  .action=OS_CONTROL_WHEEL,.code=OS_INPUT_REL_WHEEL,.value=1}};
 /* 候选缓冲区不是已发布快照，即使构建到一半也不应影响旧事件。 */
 g_nodes[0].identifier=99;assert(settings_control_valid(&event.control));g_nodes[0].identifier=1;
 g_ctrl=true;handle_event(&event);assert(font_size==20);
 /* 重布局后的旧根键盘事件仍可释放 Ctrl，伪造元数据与未来版本均不能通过。 */
 event.control.action=OS_CONTROL_KEY;event.control.code=0xE0;event.control.value=0;
 handle_event(&event);assert(!g_ctrl);
 event.control.node_kind=OS_CONTROL_BUTTON;event.control.value=1;
 handle_event(&event);assert(!g_ctrl);
 event.control.node_kind=OS_CONTROL_CONTAINER;
 event.control.tree_revision=g_tree.revision+1;handle_event(&event);assert(font_size==20);
 assert(!g_ctrl);
 /* 新画面提交失败不能撤销旧画面的事件接收能力。 */
 event.control.tree_revision=g_tree.revision;event.control.value=1;
 fail_submit=true;g_status_valid=!g_status_valid;render();handle_event(&event);assert(g_ctrl);
 fail_submit=false;render();assert(g_tree.revision>event.control.tree_revision);
 /* 真实导航事件切换页面，系统数据不得误用宿主占位值或网络状态。 */
 g_scroll=20;before=reads;click_id(1003);
 assert(g_page==SETTINGS_SYSTEM&&!g_scroll&&reads==before&&system_reads==1);
 assert(g_published_nodes[node_id(1003)].flags&OS_CONTROL_CHECKED);
 assert(!(g_published_nodes[node_id(1001)].flags&OS_CONTROL_CHECKED));
 assert(contains_text("总计  4096 MiB")&&contains_text("已用  3072 MiB")&&contains_text("可用  1024 MiB"));
 assert(contains_text("在线逻辑处理器  8")&&contains_text("分辨率  1920 × 1080"));
 assert(g_processor_name[0]&&strlen(g_processor_name)<=48);
 assert(contains_text("NVIDIA GeForce RTX 5070")&&contains_text("PCI 标识  10DE:2F04")&&g_system_gpu_count==2);
 for(unsigned f=14;f<=50;f+=4)for(unsigned w=0;w<6;++w)for(unsigned h=0;h<5;++h) {
  font_size=f;g_window.width=widths[w];g_window.height=heights[h];render();
  assert(!g_tree_overflow&&g_tree.count);
 }
 font_size=16;g_window.width=720;g_window.height=430;
 fail_system=true;assert(refresh_status());assert(!g_memory_valid&&!g_display_valid&&!g_tasks_valid);
 assert(contains_text("总计  不可用")&&contains_text("分辨率  不可用"));
 fail_system=false;assert(refresh_status());
 click_id(1001);assert(g_page==SETTINGS_NETWORK&&reads==before+1);
 /* 主题和背景独立切换，真实控件事件不能互相重置另一项。 */
 click_id(1004);assert(g_page==SETTINGS_APPEARANCE&&g_appearance_valid);
 assert(contains_text("背景色")&&contains_text("标题栏颜色")&&!contains_text("主题"));
 for(unsigned bg=0;bg<OS_THEME_BACKGROUND_COUNT;++bg) {
  click_id(1300+bg);assert(g_appearance.background==bg);
  for(unsigned theme=0;theme<OS_THEME_COUNT;++theme) {
   click_id(1200+theme);assert(g_appearance.theme==theme&&g_appearance.background==bg);
   assert(g_published_nodes[node_id(1200+theme)].flags&OS_CONTROL_CHECKED);
   assert(g_published_nodes[node_id(1300+bg)].flags&OS_CONTROL_CHECKED);
  }
 }
 for(unsigned f=14;f<=50;f+=4)for(unsigned w=0;w<6;++w)for(unsigned h=0;h<5;++h) {
  font_size=f;g_window.width=widths[w];g_window.height=heights[h];render();
  assert(!g_tree_overflow&&g_tree.count);
 }
 puts("Appearance: 12 theme/background combinations, real clicks and 300 scaled layouts PASS");
 puts("Settings system: actual navigation, memory/CPU/display values, failed reads and 300 scaled layouts PASS");
 puts("Settings network: sidebar sampling, 2400 layouts, 1000 no-op refreshes, failed snapshot, stale/forged input PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='settings-tree-') as directory:
    path = Path(directory)
    command = ['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'),
                    '-I' + str(root / 'user/desktop/settings'),
                    str(path / 'test.c'), str(root / 'kernel/graphics/control_tree.c'),
                    '-o', str(path / 'test')]
    (path / 'test.c').write_text(source + fixture)
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
    for label, original, replacement in (
        ('duplicate submissions', 'if (g_tree.revision && g_tree.count==g_node_count',
         'if (false && g_tree.count==g_node_count'),
        ('candidate event validation', 'os_control_event_node(&g_tree,g_published_nodes,event)',
         'os_control_event_node(&g_tree,g_nodes,event)'),
    ):
        assert original in source
        (path / 'test.c').write_text(source.replace(original, replacement, 1) + fixture)
        subprocess.run(command, check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True, timeout=30)
        assert result.returncode != 0 and b'Assertion' in result.stderr, (label, result.stderr)
        print('Negative control rejected:', label)
