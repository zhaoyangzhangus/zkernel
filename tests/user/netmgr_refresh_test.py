"""执行真实事件循环：仅首次打开及点击栏目采样，不设置刷新定时器。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/desktop/settings/main.c').read_text()
start = source.index('static int64_t settings_syscall_one(')
end = source.index('__attribute__((noreturn))', start)
source = source[:start] + 'static int64_t settings_syscall_one(uint64_t,uint64_t);\n' + source[end:]
source = '#define main settings_application_main\n' + source + '\n#undef main\n'
fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <setjmp.h>
static jmp_buf done;
static unsigned mode,reads,waits,limit,revision;
static uint32_t font_size;
bool ui_text_init(uint32_t size){font_size=size;return true;}
bool ui_text_adjust(int32_t direction){font_size+=direction>0?4U:0U;return true;}
uint32_t ui_text_size(void){return font_size;}
uint32_t ui_text_line_height(void){return font_size+8U;}
static int64_t settings_syscall_one(uint64_t number,uint64_t argument) {
 if(number==OS_SYS_DISPLAY_GET_INFO){os_display_info_t *d=(void*)(uintptr_t)argument;d->width=1920;d->height=1080;return 0;}
 if(number==OS_SYS_WINDOW_CREATE){os_window_create_t *w=(void*)(uintptr_t)argument;w->window=1;w->identifier=7;w->address=1;return 0;}
 if(number==OS_SYS_WINDOW_CONTROL_TREE){os_window_control_tree_t *t=(void*)(uintptr_t)argument;t->revision=++revision;return 0;}
 if(number==OS_SYS_WINDOW_UPDATE)return 0;
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
 if(number==OS_SYS_NET_GET_STATUS){++reads;return mode==5?-5:0;}
 if(number==OS_SYS_WINDOW_EVENT_READ){
  if(mode==4)return -5;
  if(waits==limit)longjmp(done,1);
  os_window_event_read_t *r=(void*)(uintptr_t)argument;
  assert(r->timeout_ns==UINT64_MAX);++waits;
  if(mode==0||mode==5)return -110;
  if(mode==3)return waits%2?-4:-11;
  r->event=(os_window_event_t){.type=OS_WINDOW_EVENT_INPUT,
   .input={.type=OS_INPUT_EVENT_RELATIVE,.code=OS_INPUT_REL_X,.value=1}};
  if(mode==2){
   unsigned i=0;while(i<g_tree.count&&g_published_nodes[i].identifier!=1001U)++i;
   assert(i<g_tree.count);
   r->event=(os_window_event_t){.type=OS_WINDOW_EVENT_CONTROL,.control={
    .target=1001U,.node_index=i,.node_kind=OS_CONTROL_LIST_ITEM,
    .tree_revision=g_tree.revision,.action=OS_CONTROL_CLICK}};
  }
  return 0;
 }
 assert(!"unexpected syscall");return -1;
}
static void run(unsigned scenario,unsigned events){
 mode=scenario;reads=waits=revision=0;limit=events;g_page=SETTINGS_NETWORK;
 g_tree=(os_window_control_tree_t){0};g_caption_height=0;
 if(!setjmp(done))assert(settings_main()==1);
}
int main(void){
 for(unsigned scenario=0;scenario<=5;++scenario){
  run(scenario,40);
  assert(reads==(scenario==2?41U:1U));
  if(scenario!=4)assert(waits==40);
 }
 puts("Settings sampling: initial read only, 40 dense/idle/interrupted events cause no polling; every sidebar click reads once PASS");
}
'''

with tempfile.TemporaryDirectory(prefix='settings-refresh-') as directory:
    path = Path(directory)
    command = ['cc', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I' + str(root / 'include'), '-I' + str(root / 'user/desktop/settings'),
               str(path / 'test.c'), '-o', str(path / 'test')]
    (path / 'test.c').write_text(source + fixture)
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=15)
    broken=source.replace('request.timeout_ns = UINT64_MAX;', '(void)refresh_status(); request.timeout_ns = UINT64_MAX;',1)
    assert broken!=source
    (path/'test.c').write_text(broken+fixture)
    subprocess.run(command,check=True)
    result=subprocess.run([str(path/'test')],capture_output=True,timeout=15)
    assert result.returncode!=0 and b'Assertion' in result.stderr
    print('Negative control rejected: refreshing before every wait')
