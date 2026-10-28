"""用真实设置窗口验证声音页能发布，点击、静音和字体缩放不会破坏控件树。"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'user/desktop/settings/main.c').read_text()
a=s.index('    register uint64_t rax',s.index('static int64_t settings_syscall_one'))
b=s.index('\n}',a)
s=s[:a]+'    return settings_host_call(number,arg0);'+s[b:]
s=s.replace('#include <uapi/all.h>','#include <uapi/all.h>\n#include <uapi/control_validation.h>\nstatic int64_t settings_host_call(uint64_t,uint64_t);')
s=s.replace('int main(void)', 'int settings_program_main(void)')
s=s.replace('#include "',' #include "')
# 保留相对头文件的原始解析位置。
s=s.replace(' #include "../../runtime/', '#include "'+str(root/'user/runtime')+'/')
for h in ['network_tree.h','system_info.h','sound.h']:
 s=s.replace(' #include "'+h+'"','#include "'+str(root/'user/desktop/settings'/h)+'"')
s=r'''#include <assert.h>
#include <stdio.h>
'''+s+r'''
static uint32_t test_font=16,publishes;
static os_audio_settings_t policy={.backend=1,.volume=70,.available=14,.effective_backend=1};
uint32_t ui_text_size(void){return test_font;}
uint32_t ui_text_line_height(void){return test_font+2;}
uint32_t ui_text_advance(void){return test_font/2;}
bool ui_text_init(uint32_t n){test_font=n;return true;}
bool ui_text_adjust(int32_t n){(void)n;return false;}
static int64_t settings_host_call(uint64_t number,uint64_t arg){
 if(number==OS_SYS_AUDIO_SETTINGS){
  os_audio_settings_t *p=(void *)(uintptr_t)arg;
  if(p->operation==OS_AUDIO_SETTINGS_WRITE){policy.backend=p->backend;policy.volume=p->volume;policy.muted=p->muted;}
  p->backend=policy.backend;p->volume=policy.volume;p->muted=policy.muted;p->available=14;
  return 0;
 }
 if(number==OS_SYS_WINDOW_CONTROL_TREE){
  os_window_control_tree_t *tree=(void *)(uintptr_t)arg;
  assert(os_control_tree_validate((void *)(uintptr_t)tree->nodes,tree->count,tree->text_size));
  tree->revision=++publishes;return 0;
 }
 if(number==OS_SYS_WINDOW_UPDATE)return 0;
 return -1;
}
static void click(uint32_t target){
 for(uint32_t i=0;i<g_tree.count;i++)if(g_published_nodes[i].identifier==target){
  os_window_event_t event={.type=OS_WINDOW_EVENT_CONTROL};
  event.control=(os_control_event_t){.action=OS_CONTROL_CLICK,.target=target,
   .tree_revision=g_tree.revision,.node_index=i,.node_kind=g_published_nodes[i].kind,
   .user_data=g_published_nodes[i].user_data};
  handle_event(&event);return;
 }
 assert(0);
}
int main(void){
 g_window.width=960;g_window.height=560;render();
 uint32_t before=publishes;click(1002);
 assert(g_page==SETTINGS_SOUND&&g_sound_valid&&publishes>before);
 click(1102);assert(policy.backend==2);
 click(1103);assert(policy.backend==3);
 click(1110);assert(policy.volume==60);
 click(1111);assert(policy.volume==70);
 click(1112);assert(policy.muted==1);
 click(1112);assert(policy.muted==0);
 for(test_font=14;test_font<=50;test_font+=4){
  for(unsigned mask=0;mask<16;mask+=2){
   g_sound.available=mask;render();assert(!g_tree_overflow);
  }
 }
 puts("Settings sound UI: real sidebar click, device choice, mute, gain and scaled tree validation passed");
}
'''
with tempfile.TemporaryDirectory() as d:
 exe=str(Path(d)/'test')
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(root/'include'),'-x','c','-','-o',exe],input=s,text=True,check=True)
 subprocess.run([exe],check=True)
