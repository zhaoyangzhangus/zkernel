"""执行真实桌面输入策略、UI 布局与合成快照，覆盖拖动、菜单、运行状态和窗口切换。"""
from pathlib import Path
import subprocess
import resource
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parents[2]
shell = (root / 'kernel/graphics/shell.c').read_text()
router = (root / 'kernel/graphics/input_router.c').read_text()
interaction = (root / 'kernel/graphics/desktop_interaction.c').read_text().replace('#include "internal.h"', '')

def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/input.h>
#include <kernel/desktop_interaction.h>
#include "user/runtime/control_file.h"
#define DESKTOP_APP_NONE 0
#define DESKTOP_APP_WALLPAPER 8
#define DESKTOP_APP_SETTINGS 4
#define OS_WINDOW_VISIBLE 1
typedef struct {unsigned identifier,flags,desktop_app;bool client_show_inactive,minimized;
 char title[32];} window_server_window_t;
static struct {unsigned display_width,display_height,pointer_x,pointer_y,desktop_pending_launch;
 unsigned focused_identifier,desktop_hovered_app;struct {unsigned count,control_count;window_server_window_t *children[64];const os_control_node_t *controls;} scene;
 desktop_interaction_t desktop;} g_window_server;
static struct {desktop_interaction_t desktop;} g_compositor_snapshot;
static struct {unsigned node_index;} g_desktop_programs[8];
static unsigned marks,focuses,minimized_events,paired_releases,paints,rehits;
static void window_input_refresh_pointer_locked(void){assert(!g_window_server.desktop.switch_open);++rehits;}
static Rect damages[1024];
static bool desktop_shell_assets_available(void){return true;}
static void window_mark_rect_locked(int32_t x,int32_t y,uint32_t w,uint32_t h){
 assert(w&&h);damages[marks++%1024]=(Rect){x,y,x+(int32_t)w,y+(int32_t)h};
}
static window_server_window_t *window_scene_find_locked(unsigned id){
 for(unsigned i=0;i<g_window_server.scene.count;++i)if(g_window_server.scene.children[i]->identifier==id)return g_window_server.scene.children[i];return 0;
}
static void window_scene_focus_locked(window_server_window_t *w){assert(w&&!w->minimized);g_window_server.focused_identifier=w->identifier;++focuses;}
static void window_mark_window_locked(window_server_window_t *w){assert(w);++marks;}
static void window_enqueue_minimized_event_locked(window_server_window_t *w){assert(!w->minimized);++minimized_events;}
static void window_animation_restore_locked(window_server_window_t *w){assert(w);}
static void window_enqueue_event_locked(window_server_window_t *w,const input_event_t *e){assert(w&&e->value==INPUT_VALUE_RELEASE);++paired_releases;}
struct window_control_document {int unused;};
static struct window_control_document overlay;
static unsigned overlay_prepares,overlay_layouts;
static bool overlay_fail;
static os_control_node_t prepared_nodes[DESKTOP_MENU_NODES];
static control_tree_box_t prepared_boxes[DESKTOP_MENU_NODES];
static char prepared_text[160];
static unsigned prepared_count,prepared_hover,prepared_pressed;
static bool check_prepared;
static bool counted_overlay_layout(const os_control_node_t *n,uint32_t count,uint32_t size,
 Rect viewport,control_tree_box_t *boxes){
 ++overlay_layouts;return control_tree_layout(n,count,size,viewport,boxes);
}
static struct window_control_document *window_control_prepare_overlay(struct window_control_document *previous,
 const os_control_node_t *n,unsigned count,const char *text,unsigned size,const control_tree_box_t *boxes){
 (void)previous;assert(n&&count&&text&&size&&boxes);++overlay_prepares;
 assert(size<=sizeof(prepared_text));prepared_count=count;
 memcpy(prepared_nodes,n,count*sizeof(*n));memcpy(prepared_boxes,boxes,count*sizeof(*boxes));
 memcpy(prepared_text,text,size);return overlay_fail?0:&overlay;
}
static void window_control_document_put(struct window_control_document *d){(void)d;}
static void compositor_control_nodes(Rect clip,const os_control_node_t *n,unsigned count,const control_tree_box_t *boxes,const char *text,unsigned hover,unsigned pressed,const struct window_control_document *prepared){(void)clip;
 assert(prepared);
 if(check_prepared){
  assert(count==prepared_count&&hover==prepared_hover&&pressed==prepared_pressed);
  assert(!memcmp(n,prepared_nodes,count*sizeof(*n)));
  assert(!memcmp(boxes,prepared_boxes,count*sizeof(*boxes)));
  for(unsigned i=0;i<count;++i)assert(!memcmp(text+n[i].text_offset,
    prepared_text+n[i].text_offset,n[i].text_length));
 }
 assert(n&&boxes&&text&&count>=1&&count<=DESKTOP_MENU_NODES);(void)hover;(void)pressed;++paints;
 for(unsigned i=1;i<count;++i){assert(n[i].text_offset+n[i].text_length<=160);if(n[i].text_length && !(n[i].flags&OS_CONTROL_HIDDEN))assert(boxes[i].visible);}
}
''' + function(router, 'bool window_input_request_desktop_launch_locked(') + '\n' + function(router, 'uint32_t window_input_take_desktop_launch_locked(') + '\n'.join(function(shell, s) for s in [
    'static Rect desktop_viewport(', 'bool desktop_base_icon_box_locked(',
    'bool desktop_icon_box_locked(', 'uint32_t desktop_app_at_locked(',
    'void desktop_mark_icon_box_locked(', 'void desktop_mark_app_locked(']) + '\n#define control_tree_layout counted_overlay_layout\n' + interaction + r'''
#undef control_tree_layout
static void button(unsigned code,unsigned value,bool window){
 input_event_t e={.type=INPUT_EVENT_BUTTON,.code=code,.value=(int)value};
 assert(desktop_route_button_locked(&e,window));
}
static bool key(unsigned code,unsigned value){input_event_t e={.type=INPUT_EVENT_KEY,.code=code,.value=(int)value};return desktop_route_key_locked(&e);}
static void pointer(int x,int y){g_window_server.pointer_x=(unsigned)x;g_window_server.pointer_y=(unsigned)y;}
static control_tree_box_t box(unsigned app){control_tree_box_t b;assert(desktop_icon_box_locked(app,&b));return b;}
static void menu_item(unsigned id){
 os_control_node_t n[DESKTOP_MENU_NODES];control_tree_box_t b[DESKTOP_MENU_NODES];
 Rect r=desktop_menu_tree(&g_window_server.desktop,g_window_server.display_width,g_window_server.display_height,n);
 assert(control_tree_layout(n,DESKTOP_MENU_NODES,sizeof(desktop_menu_text)-1U,r,b));
 unsigned index=0;while(index<DESKTOP_MENU_NODES && n[index].identifier!=id)++index;
 if(index>=DESKTOP_MENU_NODES || !b[index].visible)fprintf(stderr,"menu id=%u target=%u submenu=%u width=%u\n",id,g_window_server.desktop.menu_app,g_window_server.desktop.menu_submenu,g_window_server.display_width);
 assert(index<DESKTOP_MENU_NODES && b[index].visible);
 pointer(b[index].bounds.x0+15,b[index].bounds.y0+15);
 assert(desktop_route_motion_locked());assert(g_window_server.desktop.menu_hover==id);
 button(INPUT_BUTTON_LEFT,1,false);button(INPUT_BUTTON_LEFT,0,false);
}
int main(int argc,char **argv){
 assert(!window_input_request_desktop_launch_locked(DESKTOP_APP_NONE));
 assert(window_input_take_desktop_launch_locked()==DESKTOP_APP_NONE);
 for(unsigned app=1;app<=8;++app){
  assert(window_input_request_desktop_launch_locked(app));
  assert(!window_input_request_desktop_launch_locked(app));
  assert(!window_input_request_desktop_launch_locked(app+1));
  assert(!window_input_request_desktop_launch_locked(DESKTOP_APP_NONE));
  assert(window_input_take_desktop_launch_locked()==app);
  assert(window_input_take_desktop_launch_locked()==DESKTOP_APP_NONE);
 }

 assert(argc==2);ui_file_document_t d={0};ui_file_error_t error;
 assert(ui_file_load(argv[1],&d,&error));g_window_server.scene.controls=d.nodes;g_window_server.scene.control_count=d.request.count;
 for(unsigned i=0;i<7;++i){g_desktop_programs[i+1].node_index=d.resources[i].node_index;d.nodes[d.resources[i].node_index].user_data=i+1;}
 g_window_server.display_width=800;g_window_server.display_height=600;
 control_tree_box_t b=box(3);pointer(b.bounds.x0+20,b.bounds.y0+20);
 button(INPUT_BUTTON_LEFT,1,false);assert(!g_window_server.desktop_pending_launch);
 pointer(g_window_server.pointer_x+3,g_window_server.pointer_y);assert(desktop_route_motion_locked());
 button(INPUT_BUTTON_LEFT,0,false);assert(g_window_server.desktop_pending_launch==3);
 g_window_server.desktop_pending_launch=0;
 b=box(3);pointer(b.bounds.x0+20,b.bounds.y0+20);button(INPUT_BUTTON_LEFT,1,false);
 unsigned before=marks;pointer(100,100);assert(desktop_route_motion_locked());
 assert(g_window_server.desktop.dragging&&marks==before+2);
 button(INPUT_BUTTON_LEFT,0,true);assert(!g_window_server.desktop_pending_launch&&!g_window_server.desktop.pressed_app);
 b=box(3);assert(b.bounds.x0==80&&b.bounds.y0==80);assert(desktop_app_at_locked(100,100)==3);
 g_compositor_snapshot.desktop=g_window_server.desktop;
 pointer(100,100);button(INPUT_BUTTON_LEFT,1,false);pointer(799,599);desktop_route_motion_locked();button(INPUT_BUTTON_LEFT,0,false);
 b=box(3);assert(b.bounds.x1==800&&b.bounds.y1==600);
 control_tree_box_t old;assert(desktop_base_icon_box_locked(3,&old));
 desktop_place_box(&g_compositor_snapshot.desktop,3,desktop_viewport(),&old);assert(old.bounds.x0==80&&old.bounds.y0==80);
 /* 空白菜单隐藏打开；展开整理后才能选择边缘位置。 */
 pointer(10,10);button(INPUT_BUTTON_RIGHT,1,false);button(INPUT_BUTTON_RIGHT,0,false);
 assert(g_window_server.desktop.menu_open);
 os_control_node_t empty_menu[DESKTOP_MENU_NODES];
 desktop_menu_tree(&g_window_server.desktop,800,600,empty_menu);
 assert(empty_menu[2].flags&OS_CONTROL_HIDDEN);
 assert((empty_menu[6].flags&OS_CONTROL_HIDDEN)&&(empty_menu[7].flags&OS_CONTROL_HIDDEN));
 menu_item(11);assert(g_window_server.desktop.menu_open&&g_window_server.desktop.menu_submenu);
 menu_item(20);
 assert(!g_window_server.desktop.menu_open&&g_window_server.desktop.positions[3].placed);
 assert(!g_window_server.desktop_pending_launch);
 b=box(2);pointer(b.bounds.x0+20,b.bounds.y0+20);button(INPUT_BUTTON_RIGHT,1,false);
 desktop_menu_tree(&g_window_server.desktop,800,600,empty_menu);
 assert((empty_menu[6].flags&OS_CONTROL_HIDDEN)&&(empty_menu[7].flags&OS_CONTROL_HIDDEN));
 menu_item(10);assert(g_window_server.desktop_pending_launch==2);g_window_server.desktop_pending_launch=0;
 /* 不同来源共用单槽规则，菜单或图标不能覆盖尚未取走的请求。 */
 for(unsigned item=10;item<=13;++item)if(item!=11){
  assert(window_input_request_desktop_launch_locked(7));
  b=box(2);pointer(b.bounds.x0+20,b.bounds.y0+20);
  button(INPUT_BUTTON_RIGHT,1,false);menu_item(item);
  assert(window_input_take_desktop_launch_locked()==7);
 }
 assert(window_input_request_desktop_launch_locked(7));
 b=box(3);pointer(b.bounds.x0+20,b.bounds.y0+20);
 button(INPUT_BUTTON_LEFT,1,false);button(INPUT_BUTTON_LEFT,0,false);
 assert(window_input_take_desktop_launch_locked()==7);
 /* 壁纸操作必须派发专用启动标识，不能误走已删除的排列方式分支。 */
 pointer(10,10);button(INPUT_BUTTON_RIGHT,1,false);menu_item(12);
 assert(g_window_server.desktop_pending_launch==DESKTOP_APP_WALLPAPER);
 g_window_server.desktop_pending_launch=0;
 /* 设置和电源项使用相同布局命中；按下不执行，释放到另一项也不得误触。 */
 pointer(10,10);button(INPUT_BUTTON_RIGHT,1,false);menu_item(13);
 assert(g_window_server.desktop_pending_launch==DESKTOP_APP_SETTINGS);
 g_window_server.desktop_pending_launch=0;
 /* 从设置图标打开同一菜单，验证重启和关机都能命中。 */
 for(unsigned action=1;action<=2;++action) {
  b=box(DESKTOP_APP_SETTINGS);pointer(b.bounds.x0+20,b.bounds.y0+20);
  assert(desktop_app_at_locked(g_window_server.pointer_x,g_window_server.pointer_y)==DESKTOP_APP_SETTINGS);
  button(INPUT_BUTTON_RIGHT,1,false);
  assert(g_window_server.desktop.menu_app==DESKTOP_APP_SETTINGS);
  menu_item(13U+action);
  assert(!g_window_server.desktop.menu_open && g_window_server.desktop.power_request==action);
  g_window_server.desktop.power_request=0;
 }
 b=box(DESKTOP_APP_SETTINGS);pointer(b.bounds.x0+20,b.bounds.y0+20);
 button(INPUT_BUTTON_RIGHT,1,false);
 os_control_node_t power_nodes[DESKTOP_MENU_NODES];control_tree_box_t power_boxes[DESKTOP_MENU_NODES];
 Rect power_view=desktop_menu_tree(&g_window_server.desktop,800,600,power_nodes);
 assert(control_tree_layout(power_nodes,DESKTOP_MENU_NODES,sizeof(desktop_menu_text)-1U,power_view,power_boxes));
 pointer(power_boxes[6].bounds.x0+15,power_boxes[6].bounds.y0+15);
 button(INPUT_BUTTON_LEFT,1,false);assert(!g_window_server.desktop.power_request);
 pointer(power_boxes[7].bounds.x0+15,power_boxes[7].bounds.y0+15);
 button(INPUT_BUTTON_LEFT,0,false);assert(!g_window_server.desktop.power_request);
 /* 设置菜单靠近屏幕边缘时，两个电源项仍在可见区域内并命中各自控件。 */
 const unsigned menu_screens[][2]={{320,480},{800,600},{1920,1080}};
 for(unsigned screen=0;screen<3;++screen) {
  unsigned width=menu_screens[screen][0],height=menu_screens[screen][1];
  desktop_interaction_t edge={.menu_app=DESKTOP_APP_SETTINGS,
      .menu_x=(int32_t)width-1,.menu_y=(int32_t)height-1};
  power_view=desktop_menu_tree(&edge,width,height,power_nodes);
  assert(control_tree_layout(power_nodes,DESKTOP_MENU_NODES,sizeof(desktop_menu_text)-1U,power_view,power_boxes));
  for(unsigned i=6;i<=7;++i) {
   Rect bounds=power_boxes[i].bounds;
   assert(power_boxes[i].visible && bounds.x0>=0 && bounds.y0>=0 &&
       bounds.x1<=(int32_t)width && bounds.y1<=(int32_t)height);
   assert(control_tree_locate(power_nodes,DESKTOP_MENU_NODES,power_view,
       bounds.x0+2,bounds.y0+2,0)==i);
  }
  assert(!strcmp(desktop_menu_text+power_nodes[6].text_offset,"重启"));
  assert(!strcmp(desktop_menu_text+power_nodes[7].text_offset,"关机"));
 }
 /* 外部点击必须吞掉整对按钮事件，不能激活下面的应用。 */
 pointer(10,10);button(INPUT_BUTTON_RIGHT,1,false);pointer(500,100);
 button(INPUT_BUTTON_LEFT,1,true);assert(!g_window_server.desktop.menu_open);
 button(INPUT_BUTTON_LEFT,0,true);assert(!g_window_server.desktop.swallow_release);
 pointer(799,599);button(INPUT_BUTTON_RIGHT,1,false);
 os_control_node_t menu[DESKTOP_MENU_NODES];Rect r=desktop_menu_tree(&g_window_server.desktop,800,600,menu);
 assert(r.x1==800&&r.y1==600);g_compositor_snapshot.desktop=g_window_server.desktop;
 desktop_prepare_text();desktop_draw_menu(((Rect){0,0,800,600}));assert(paints==1);assert(key(0x29,1)&&!g_window_server.desktop.menu_open);
 /* 不同分辨率下菜单裁剪和布局不会回绕。 */
 for(unsigned w=1;w<1000;w+=17)for(unsigned h=1;h<700;h+=19){
  control_tree_box_t boxes[DESKTOP_MENU_NODES];r=desktop_menu_tree(&g_window_server.desktop,w,h,menu);
  assert(r.x0>=0&&r.y0>=0&&r.x1<=(int)w&&r.y1<=(int)h);
  assert(control_tree_layout(menu,DESKTOP_MENU_NODES,sizeof(desktop_menu_text)-1U,r,boxes));
 }
 /* 四个菜单方向均使用真实布局和命中；所有图标留在视口内且不重叠。 */
 const unsigned resolutions[][2]={{800,600},{320,480},{1920,1080}};
 for(unsigned screen=0;screen<3;++screen)for(unsigned direction=0;direction<4;++direction) {
  g_window_server.display_width=resolutions[screen][0];g_window_server.display_height=resolutions[screen][1];
  pointer(10,10);button(INPUT_BUTTON_RIGHT,1,false);menu_item(11);menu_item(20U+direction);
  assert(!g_window_server.desktop.menu_open);
  control_tree_box_t boxes[DESKTOP_APP_SLOTS];
  for(unsigned app=1;app<DESKTOP_APP_SLOTS;++app) {
   boxes[app]=box(app);
   assert(boxes[app].bounds.x0>=0&&boxes[app].bounds.y0>=0);
   assert(boxes[app].bounds.x1<=(int)g_window_server.display_width&&boxes[app].bounds.y1<=(int)g_window_server.display_height);
   assert(desktop_app_at_locked(boxes[app].bounds.x0+20,boxes[app].bounds.y0+20)==app);
   for(unsigned old=1;old<app;++old)assert(!rect_intersects(boxes[old].bounds,boxes[app].bounds));
  }
  Rect extent=boxes[1].bounds;
  for(unsigned app=2;app<DESKTOP_APP_SLOTS;++app) {
   Rect r=boxes[app].bounds;
   if(r.x0<extent.x0)extent.x0=r.x0;if(r.y0<extent.y0)extent.y0=r.y0;
   if(r.x1>extent.x1)extent.x1=r.x1;if(r.y1>extent.y1)extent.y1=r.y1;
  }
  if(direction==0)assert(extent.x0==18);
  if(direction==1)assert(extent.x1==(int)g_window_server.display_width-18);
  if(direction==2)assert(extent.y0==18);
  if(direction==3)assert(extent.y1==(int)g_window_server.display_height-18);
  int symmetry=direction<2 ? extent.y0+extent.y1-(int)g_window_server.display_height :
      extent.x0+extent.x1-(int)g_window_server.display_width;
  assert(symmetry>=-1 && symmetry<=1);
 }
 g_window_server.display_width=800;g_window_server.display_height=600;
 unsigned switch_rehits=rehits;
 puts("Desktop arrange: four centered edges, submenu, hidden open and wallpaper dispatch, three resolutions, in-bounds non-overlapping icons and exact hit tests PASS");
 window_server_window_t a={.identifier=11,.flags=1,.desktop_app=1,.title="Files"};
 window_server_window_t c={.identifier=12,.flags=1,.desktop_app=6,.title="Chrome",.minimized=true};
 window_server_window_t popup={.identifier=13,.flags=1,.desktop_app=6,.client_show_inactive=true,.title="Popup"};
 g_window_server.scene.children[0]=&a;g_window_server.scene.children[1]=&c;g_window_server.scene.children[2]=&popup;
 g_window_server.scene.count=3;g_window_server.focused_identifier=11;
 desktop_capture_state_locked();assert(g_window_server.desktop.running_mask==((1U<<1)|(1U<<6)));
 assert(g_window_server.desktop.active_mask==(1U<<1));
 assert(!key(0xe2,1));assert(key(0x2b,1));assert(g_window_server.desktop.switch_open);
 assert(g_window_server.desktop.switch_count==2&&paired_releases==1);
 assert(g_window_server.desktop.switch_windows[g_window_server.desktop.switch_selected].identifier==12);
 g_compositor_snapshot.desktop=g_window_server.desktop;desktop_prepare_text();desktop_draw_menu(((Rect){0,0,800,600}));assert(paints==2);
 assert(key(0x2b,0));assert(key(0xe2,0));assert(!g_window_server.desktop.switch_open);
 assert(g_window_server.focused_identifier==12&&!c.minimized&&minimized_events==1&&focuses==1&&rehits==switch_rehits+1U);
 assert(!key(0xe2,1));assert(key(0x2b,1));assert(key(0x29,1));
 assert(g_window_server.focused_identifier==12&&rehits==switch_rehits+2U);key(0x2b,0);key(0xe2,0);
 /* 切换期间目标退出，快照丢弃旧 ID；最后一个窗口退出后关闭栏。 */
 key(0xe2,1);key(0x2b,1);g_window_server.scene.count=1;desktop_capture_state_locked();
 assert(g_window_server.desktop.switch_count==1);g_window_server.scene.count=0;desktop_capture_state_locked();
 assert(!g_window_server.desktop.switch_open&&!g_window_server.desktop.running_mask);
 key(0x2b,0);key(0xe2,0);
 /* 同帧分段绘制仅消费已准备的布局，后续生产者和快照变化不得改变本帧。 */
 for(unsigned mode=0;mode<2;++mode){
  g_window_server.display_width=800;g_window_server.display_height=600;
  desktop_interaction_t state={0};
  if(mode){
   state.switch_open=true;state.switch_count=2;state.switch_selected=1;
   state.switch_windows[0].identifier=11;state.switch_windows[1].identifier=12;
   strcpy(state.switch_windows[0].title,"Shell");strcpy(state.switch_windows[1].title,"Chrome");
  }else {state.menu_open=true;state.menu_x=20;state.menu_y=20;state.menu_hover=2;state.menu_pressed=2;}
  g_compositor_snapshot.desktop=state;
  unsigned layouts=overlay_layouts,prepares=overlay_prepares;
  desktop_prepare_text();assert(overlay_layouts==layouts+1&&overlay_prepares==prepares+1);
  prepared_hover=mode?0:state.menu_hover;prepared_pressed=mode?0:state.menu_pressed;
  check_prepared=true;unsigned painted=paints;
  g_compositor_snapshot.desktop=(desktop_interaction_t){0};
  g_window_server.scene.count=0;g_window_server.display_width=1;g_window_server.display_height=1;
  for(unsigned row=0;row<600;row+=20)desktop_draw_menu((Rect){0,row,800,row+20});
  assert(paints==painted+30&&overlay_layouts==layouts+1&&overlay_prepares==prepares+1);
  check_prepared=false;desktop_prepare_text();painted=paints;
  desktop_draw_menu((Rect){0,0,800,600});assert(paints==painted);
  g_window_server.display_width=800;g_window_server.display_height=600;
  g_compositor_snapshot.desktop=state;overlay_fail=true;desktop_prepare_text();
  desktop_draw_menu((Rect){0,0,800,600});assert(paints==painted);overlay_fail=false;
 }
 ui_file_destroy(&d);puts("Desktop drag/click, damage, snapshot, menu, arrange, running state, Alt+Tab lifecycle PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='desktop-interaction-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(program)
    command = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root), '-I'+str(root/'include'), str(work/'test.c'),
                    str(root/'user/runtime/control_file.c'), str(root/'kernel/graphics/control_tree.c'),
                    '-o', str(work/'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(work/'test'), str(root/'user/ui/desktop.ui')], check=True)

    # 取消忙槽保护时，真实菜单和直接申请的先到请求检查必须拒绝覆盖。
    broken = program.replace('g_window_server.desktop_pending_launch != DESKTOP_APP_NONE', 'false')
    assert broken != program
    (work / 'test.c').write_text(broken)
    subprocess.run(command, check=True)
    assert subprocess.run([str(work/'test'), str(root/'user/ui/desktop.ui')],
                          stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL).returncode != 0

    # 绘制重新读取快照时，分段提交会失去已准备的弹层；反例必须由同帧断言拒绝。
    broken = program.replace('if (!g_desktop_overlay_frame.count) return;',
        'if (!g_compositor_snapshot.desktop.menu_open && '
        '!g_compositor_snapshot.desktop.switch_open) return;')
    assert broken != program
    (work / 'test.c').write_text(broken)
    subprocess.run(command, check=True)
    assert subprocess.run([str(work/'test'), str(root/'user/ui/desktop.ui')],
                          stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL).returncode != 0
print('桌面启动请求：空请求、重复申请、跨应用竞争、取走清空及菜单/图标忙槽保护通过，覆盖请求反例被拒绝')
print('桌面弹层：菜单及窗口切换栏每帧仅布局一次，30 个条带复用布局，关闭及准备失败不绘制旧内容，重读快照反例被拒绝')
