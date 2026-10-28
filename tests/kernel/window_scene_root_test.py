"""执行真实根节点注册、排序、命中和键盘选择，不维护第二份窗口列表。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
window = (root/'kernel/graphics/window.c').read_text()
registry = window[window.index('void window_registry_reset_locked('):window.index('static bool window_lifecycle_test_fail(')]
scene = (root/'kernel/graphics/scene.c').read_text().replace('#include "internal.h"','')
hit = (root/'kernel/graphics/hit_test.c').read_text().replace('#include "internal.h"','')
zorder = (root/'kernel/graphics/zorder.c').read_text().replace('#include "internal.h"','')
# 统计真实列表遍历，验证重复聚焦不会再扫描窗口。
registry = registry.replace('        if (g_window_server.scene.children[position] == window)',
    '        ++order_visits;\n        if (g_window_server.scene.children[position] == window)')
scene = scene.replace('        window_server_window_t *window = g_window_server.scene.children[index];',
    '        ++find_visits;\n        window_server_window_t *window = g_window_server.scene.children[index];',1)
zorder = zorder.replace('        if (g_window_server.scene.children[position] == window)',
    '        ++order_visits;\n        if (g_window_server.scene.children[position] == window)')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <kernel/window_geometry.h>
#define WINDOW_SERVER_MAX_WINDOWS 64U
#define DESKTOP_APP_NONE 0U
struct window_scene_root;
typedef struct window {
 struct window_scene_root *scene_parent;
 void *owner;uint32_t identifier,desktop_app,flags,width,height;
 int32_t x,y;bool minimized,maximized,client_show_inactive;
} window_server_window_t;
typedef struct window_scene_root {
 window_server_window_t *children[64];uint32_t count;
 const void *controls;uint32_t control_count,hovered_window_identifier,hovered_title_button;
} window_scene_root_t;
static struct {window_scene_root_t scene;uint32_t next_identifier,focused_identifier,pointer_x,pointer_y,dragging_identifier;struct {bool menu_open,switch_open;uint32_t pressed_app;} desktop;} g_window_server;
static unsigned opened,marks,blurs,find_visits,order_visits;
static void window_control_keyboard_blur_locked(window_server_window_t *w){if(w)++blurs;}
static void window_mark_window_locked(window_server_window_t *w){if(w)++marks;}
static uint32_t desktop_window_app_locked(window_server_window_t *w){(void)w;return 3;}
static void desktop_chrome_window_opened_locked(void *p){(void)p;++opened;}
void window_scene_set_focus_identifier_locked(uint32_t id);
''' + registry + hit + scene + zorder + r'''
int main(void){
 window_scene_root_t foreign={0};window_server_window_t windows[65]={0};
 g_window_server.scene.controls=(void*)7;g_window_server.scene.control_count=9;
 window_registry_reset_locked();assert(g_window_server.next_identifier==1);
 assert(!window_registry_append_locked(0));
 for(unsigned i=0;i<64;++i){
  windows[i].flags=OS_WINDOW_VISIBLE;windows[i].width=100;windows[i].height=100;
  windows[i].x=20;windows[i].y=20;
  assert(window_registry_append_locked(&windows[i]));
  assert(windows[i].scene_parent==&g_window_server.scene&&g_window_server.scene.children[i]==&windows[i]);
  assert(windows[i].identifier==i+1);
 }
 assert(opened==64&&g_window_server.scene.count==64);
 assert(!window_registry_append_locked(&windows[64])&&!windows[64].scene_parent);
 assert(!window_registry_append_locked(&windows[0]));
 assert(window_scene_hit_test_locked(60,60)==&windows[63]);
 windows[63].minimized=true;assert(window_scene_hit_test_locked(60,60)==&windows[62]);
 windows[62].flags=0;assert(window_scene_hit_test_locked(60,60)==&windows[61]);
 assert(!window_scene_hit_test_locked(0,0));
 window_registry_move_to_front_locked(&windows[0]);
 assert(g_window_server.scene.children[63]==&windows[0]&&window_scene_hit_test_locked(60,60)==&windows[0]);
 g_window_server.focused_identifier=0;assert(window_scene_keyboard_locked()==&windows[0]);
 assert(window_scene_find_locked(windows[0].identifier)==&windows[0]);
 uint32_t old=windows[0].identifier;
 assert(window_registry_remove_locked(&windows[0])&&!windows[0].scene_parent);
 assert(!window_scene_find_locked(old)&&!window_registry_remove_locked(&windows[0]));
 windows[0].scene_parent=&foreign;assert(!window_registry_append_locked(&windows[0]));
 window_registry_move_to_front_locked(&windows[0]);assert(g_window_server.scene.children[62]==&windows[63]);
 assert(!window_registry_remove_locked(&windows[0]));windows[0].scene_parent=0;
 /* 重新挂接从同一根重新分配标识，不复用已移除窗口的旧事件身份。 */
 assert(window_registry_append_locked(&windows[0])&&windows[0].identifier!=old);
 assert(opened==65&&g_window_server.scene.count==64);

 /* 每种窗口数、目标位置和旧焦点均对照独立数组排序，保持所有相对次序。 */
 unsigned cases=0;
 for(unsigned n=1;n<=64;++n)for(unsigned target=0;target<n;++target)for(unsigned focus=0;focus<=n;++focus){
  window_server_window_t *want[64];
  g_window_server.scene.count=n;g_window_server.focused_identifier=focus;
  for(unsigned i=0;i<n;++i){
   windows[i].identifier=i+1;windows[i].scene_parent=&g_window_server.scene;
   windows[i].flags=OS_WINDOW_VISIBLE;windows[i].minimized=false;
   g_window_server.scene.children[i]=&windows[i];want[i]=&windows[i];
  }
  for(unsigned i=target;i+1<n;++i)want[i]=want[i+1];
  want[n-1]=&windows[target];
  marks=blurs=find_visits=order_visits=0;window_scene_focus_locked(&windows[target]);
  assert(order_visits==(target+1<n?target+1:0));
  assert(find_visits==((focus==target+1&&target+1==n)?0U:focus));
  assert(g_window_server.focused_identifier==target+1);
  for(unsigned i=0;i<n;++i)assert(g_window_server.scene.children[i]==want[i]);
  bool noop=focus==target+1&&target+1==n;
  assert(marks==(noop?0U:(focus&&focus!=target+1?2U:1U)));
  assert(blurs==(focus&&focus!=target+1?1U:0U));
  marks=blurs=find_visits=order_visits=0;
  window_scene_focus_locked(&windows[target]);
  assert(!marks&&!blurs&&!find_visits&&!order_visits);
  ++cases;
 }
 /* 隐藏、最小化和外部节点不能改变场景焦点或制造重绘。 */
 windows[64]=(window_server_window_t){.identifier=100,.flags=OS_WINDOW_VISIBLE,.scene_parent=&foreign};
 unsigned focus_before=g_window_server.focused_identifier;
 window_scene_focus_locked(&windows[64]);assert(g_window_server.focused_identifier==focus_before&&!marks&&!blurs);
 windows[63].minimized=true;window_scene_focus_locked(&windows[63]);
 windows[63].minimized=false;windows[63].flags=0;window_scene_focus_locked(&windows[63]);
 window_scene_focus_locked(NULL);assert(g_window_server.focused_identifier==focus_before&&!marks&&!blurs);
 puts("Focus work: one old-focus lookup, no duplicate same-window damage; frontmost repeat scans 0 entries PASS");
 printf("Focus order: %u independent array oracles, repeat no-op, blur/damage and foreign/hidden/minimized rejection PASS\n",cases);
 window_registry_reset_locked();assert(!g_window_server.scene.count);
 for(unsigned i=0;i<64;++i)assert(!windows[i].scene_parent&&!g_window_server.scene.children[i]);
 assert(g_window_server.scene.controls==(void*)7&&g_window_server.scene.control_count==9);
 puts("Scene root: real registry ownership, duplicate/full/foreign rejection, z-order hit/keyboard selection, detach/reset and desktop controls passed");
}
'''
with tempfile.TemporaryDirectory(prefix='scene-root-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(root/'include'),str(path/'test.c'),str(root/'kernel/graphics/window_geometry.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
