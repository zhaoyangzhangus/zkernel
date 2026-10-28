"""执行完整物理输入路由与真实控件指针状态机，验证跨窗口、捕获和桌面覆盖层。"""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
router=(root/'kernel/graphics/input_router.c').read_text()
control=(root/'kernel/graphics/control_document.c').read_text()
window_source=(root/'kernel/graphics/window.c').read_text()
scene=(root/'kernel/graphics/scene.c').read_text().replace('#include "internal.h"','')
hit=(root/'kernel/graphics/hit_test.c').read_text().replace('#include "internal.h"','')
hit=hit.replace('    for (uint32_t index = g_window_server.scene.count;',
    '    ++hit_calls;\n    for (uint32_t index = g_window_server.scene.count;',1)
hit=hit.replace('        window_server_window_t *window = g_window_server.scene.children[index - 1U];',
    '        ++hit_visits;\n        window_server_window_t *window = g_window_server.scene.children[index - 1U];',1)

def function(source, signature):
    a=source.index(signature);b=source.index('\n}',a)+2
    return source[a:b]

pointer=control[control.index('typedef struct control_pointer_query {'):control.index('typedef struct control_text_query {')]
pointer+=r'''/* 本测试的节点没有文字，文本选区状态机由 control_document_test 覆盖。 */
static bool control_text_input(window_server_window_t *w,const os_input_event_t *e,
 int32_t x,int32_t y,bool inside,control_pointer_query_t *query){
 (void)e;(void)x;(void)y;(void)inside;(void)query;
 for(uint32_t i=0;i<w->controls->count;++i)assert(!w->controls->nodes[i].text_length);
 return false;
}
'''+function(control,'bool window_control_route_locked(')+function(control,'void window_control_pointer_hover_locked(')
geometry=function(control,'static Rect control_pointer_bounds(')+function(control,'static Rect control_button_damage(')
geometry+=function(control,'static void control_range_cache_store(')
common=router[router.index('static bool window_route_relative_pointer_locked('):]
title=function(router,'uint32_t window_title_button_at_locked(')
clear=function(router,'void window_clear_title_capture_locked(')
mark=function(router,'static void window_mark_caption_button_locked(')
preamble=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/input.h>
#include <kernel/control_tree.h>
#include <kernel/window_geometry.h>
#include <uapi/window.h>
#define DESKTOP_APP_NONE 0U
#define DESKTOP_TOPBAR_HEIGHT 0U
#define WINDOW_TITLE_DRAG_DISTANCE 4U
#define WINDOW_DRAG_REGION_HEIGHT 31U
enum {WINDOW_RESIZE_LEFT=1,WINDOW_RESIZE_RIGHT=2,WINDOW_RESIZE_TOP=4,WINDOW_RESIZE_BOTTOM=8};
typedef struct {os_control_node_t *nodes;uint32_t count;uint16_t *targets;} window_control_document_t;
static bool window_control_has_caption(const window_control_document_t *p){return p&&(p->nodes[0].flags&OS_CONTROL_WINDOW_FRAME);}
typedef struct window {
 uint32_t identifier,flags,width,height,control_hover,control_pressed,control_focus;
 int32_t x,y;bool client_pointer_capture,client_button_capture,client_hidden,minimized,maximized,fullscreen,client_show_inactive;
 uint32_t control_text_ctrl;bool control_text_drag,animation_seen;uint64_t animation_start;
 bool dirty,resize_pending;int32_t restore_x,restore_y;uint32_t restore_width,restore_height;
 uint64_t present_sequence;window_control_document_t *controls;
 Rect control_hover_damage;uint32_t control_input_width,control_input_height;
 Rect control_range_bounds;uint32_t control_range_target,control_range_width,control_range_height,control_range_flags;
 uint64_t control_revision,control_range_revision;
} window_server_window_t;
static struct {
 struct {window_server_window_t *children[3];uint32_t count,hovered_window_identifier,hovered_title_button;} scene;
 struct {bool menu_open,switch_open,dragging;uint32_t pressed_app;} desktop;
 uint32_t pointer_x,pointer_y,display_width,display_height,focused_identifier,dragging_identifier,resize_edges;
 uint32_t title_pressed_identifier,title_pressed_button,title_pressed_pointer_x,title_pressed_pointer_y;
 bool title_pressed_client;input_event_t title_pressed_event;int32_t drag_offset_x,drag_offset_y;
 uint32_t desktop_hovered_app,desktop_gui_mask,desktop_pending_launch;bool desktop_tab_consumed,desktop_focus_cycle_requested;
} g_window_server;
typedef struct {uint32_t window,target;uint16_t action;uint64_t timestamp;int32_t x,y,value;} record_t;
static record_t records[10000];static unsigned count,updates,cursors,coalesces,activations,raw_events,caption_marks;
static unsigned hit_calls,hit_visits;
static void record(window_server_window_t *w,const os_input_event_t *e,uint32_t target,uint16_t action,int32_t x,int32_t y){
 assert(count<10000);records[count++]=(record_t){w->identifier,target,action,e->timestamp,x,y,e->value};
}
static void window_enqueue_control_event_locked(window_server_window_t *w,const os_input_event_t *e,uint32_t target,uint16_t action,int32_t x,int32_t y){record(w,e,target,action,x,y);}
static void control_mark_damage_locked(window_server_window_t *w,Rect r){(void)w;(void)r;}
static unsigned selection_clears;
static void window_control_clear_text_selection_locked(window_server_window_t *w){assert(w);++selection_clears;}
static void window_mark_window_locked(window_server_window_t *w){(void)w;}
static bool control_route_key(window_server_window_t *w,const os_input_event_t *e,int32_t x,int32_t y){(void)w;(void)e;(void)x;(void)y;return false;}
static void control_set_focus(window_server_window_t *w,const os_input_event_t *e,uint32_t id,int32_t x,int32_t y){(void)e;(void)x;(void)y;w->control_focus=id;}
static void control_activate(window_server_window_t *w,const os_input_event_t *e,const os_control_node_t *n,int32_t x,int32_t y){assert(n);++activations;record(w,e,n->identifier,OS_CONTROL_CLICK,x,y);}
static void window_scene_set_focus_identifier_locked(uint32_t id){g_window_server.focused_identifier=id;}
static void window_input_set_pointer_locked(uint32_t x,uint32_t y){++updates;g_window_server.pointer_x=x;g_window_server.pointer_y=y;}
static void window_mark_moved_cursor_locked(uint32_t a,uint32_t b,uint32_t c,uint32_t d){assert(a!=c||b!=d);++cursors;}
static void window_coalesce_damage_locked(void){++coalesces;}
static void window_mark_rect_locked(int32_t x,int32_t y,uint32_t w,uint32_t h){(void)x;(void)y;assert(w&&h);++caption_marks;}
static uint32_t desktop_app_at_locked(uint32_t x,uint32_t y){return x<20&&y<20?7:0;}
static void desktop_mark_app_locked(uint32_t app){(void)app;}
static void desktop_set_hovered_app_locked(uint32_t app){g_window_server.desktop_hovered_app=app;}
static uint32_t window_input_resize_edges_locked(const window_server_window_t *w,uint32_t x,uint32_t y){(void)w;(void)x;(void)y;return 0;}
static uint32_t window_resize_edges_locked(const window_server_window_t *w,uint32_t x,uint32_t y){return window_input_resize_edges_locked(w,x,y);}
static bool desktop_route_key_locked(const input_event_t *e){return e->type==INPUT_EVENT_KEY&&g_window_server.desktop.menu_open;}
static bool desktop_route_button_locked(const input_event_t *e,bool over){if(e->type==INPUT_EVENT_BUTTON&&g_window_server.desktop.menu_open)assert(over);return e->type==INPUT_EVENT_BUTTON&&(g_window_server.desktop.menu_open||g_window_server.desktop.switch_open);}
static bool desktop_route_motion_locked(void){return g_window_server.desktop.menu_open||g_window_server.desktop.switch_open||g_window_server.desktop.pressed_app;}
static void window_input_clear_drag_locked(void){g_window_server.dragging_identifier=0;}
static void window_input_begin_drag_locked(uint32_t id,int32_t x,int32_t y,uint32_t edges){g_window_server.dragging_identifier=id;g_window_server.drag_offset_x=x;g_window_server.drag_offset_y=y;g_window_server.resize_edges=edges;}
static bool window_title_capture_moved_locked(void){return false;}
static bool window_resize_locked(window_server_window_t *w,int32_t x,int32_t y){(void)w;(void)x;(void)y;return false;}
static void window_drag_move_locked(window_server_window_t *w,int32_t x,int32_t y){w->x=x;w->y=y;}
static void focus_locked(window_server_window_t *w){g_window_server.focused_identifier=w->identifier;}
static bool window_server_init(void){return true;}
static void window_lock(void){}
static void window_unlock(void){}
static void window_server_notify_worker(void){}
static void window_scene_focus_locked(window_server_window_t *w){
 unsigned i=0;while(i<g_window_server.scene.count&&g_window_server.scene.children[i]!=w)++i;
 assert(i<g_window_server.scene.count);
 for(;i+1<g_window_server.scene.count;++i)g_window_server.scene.children[i]=g_window_server.scene.children[i+1];
 g_window_server.scene.children[i]=w;g_window_server.focused_identifier=w->identifier;
}

static void window_enqueue_close_request_locked(window_server_window_t *w){(void)w;}
static bool window_toggle_maximize_locked(window_server_window_t *w);
static void window_enqueue_resize_event_locked(window_server_window_t *w){assert(w->resize_pending);}
static bool window_minimize_locked(window_server_window_t *w);
static void window_animation_depart_locked(window_server_window_t *w,bool minimize){assert(w&&!w->minimized&&minimize);}
static void window_enqueue_minimized_event_locked(window_server_window_t *w){assert(w->minimized);}
static uint32_t desktop_number_shortcut_app(uint32_t code){(void)code;return DESKTOP_APP_NONE;}
static void input_route_diag_key(const input_event_t *e,window_server_window_t *w,bool delivered){(void)e;(void)w;(void)delivered;}
'''
lookup=r'''
static unsigned layout_hits,find_layouts;
static uint32_t counted_locate(const os_control_node_t *nodes,uint32_t n,Rect view,int32_t x,int32_t y,control_tree_box_t *box){
 ++layout_hits;return control_tree_locate(nodes,n,view,x,y,box);
}
#define control_tree_locate counted_locate
static uint32_t counted_find(const os_control_node_t *nodes,uint32_t n,Rect view,uint32_t id,control_tree_box_t *box){
 ++find_layouts;return control_tree_find(nodes,n,view,id,box);
}
#define control_tree_find counted_find
'''
bridge=r'''
static window_server_window_t *keyboard_window_locked(void){return window_scene_keyboard_locked();}
static void window_enqueue_event_locked(window_server_window_t *w,const input_event_t *event){
 os_input_event_t e={.timestamp=event->timestamp,.device_id=event->device_id,.type=event->type,.flags=event->flags,.code=event->code,.value=event->value};
 int32_t x=(int32_t)g_window_server.pointer_x-w->x-window_client_offset_x(w->flags);
 int32_t y=(int32_t)g_window_server.pointer_y-w->y-window_client_offset_y(w->flags);
 if(!window_control_route_locked(w,&e,x,y,e.type==OS_INPUT_EVENT_KEY||g_window_server.scene.hovered_window_identifier==w->identifier,true)){++raw_events;record(w,&e,0,0,x,y);}
}
'''
tests=r'''
static input_event_t event;
static void move(int axis,int delta){event=(input_event_t){.timestamp=event.timestamp+1,.device_id=9,.type=INPUT_EVENT_RELATIVE,.code=axis,.value=delta};unsigned before=layout_hits,hits=hit_calls;route_input_locked(&event);assert(layout_hits<=before+1);assert(hit_calls==hits+1);}
static void button(int value){event=(input_event_t){.timestamp=event.timestamp+1,.device_id=9,.type=INPUT_EVENT_BUTTON,.code=INPUT_BUTTON_LEFT,.value=value};unsigned before=layout_hits,hits=hit_calls;route_input_locked(&event);assert(layout_hits<=before+1);assert(hit_calls==hits+1);}
static void expect(unsigned i,unsigned w,unsigned target,unsigned action){assert(i<count&&records[i].window==w&&records[i].target==target&&records[i].action==action&&records[i].timestamp==event.timestamp);}
int main(void){
 os_control_node_t an[2]={{.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER},
  {.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,.width=100,.height=80}};
 os_control_node_t bn[2]={an[0],an[1]};bn[0].identifier=11;bn[1].identifier=12;
 uint16_t targets[2]={0,1};window_control_document_t ad={an,2,targets},bd={bn,2,targets};
 window_server_window_t a={.identifier=1,.flags=OS_WINDOW_VISIBLE|OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_EXPLICIT_MOVE_RESIZE,.x=40,.y=40,.width=200,.height=140,.present_sequence=1,.controls=&ad};
 window_server_window_t b=a;b.identifier=2;b.x=80;b.controls=&bd;
 g_window_server.scene.children[0]=&a;g_window_server.scene.children[1]=&b;g_window_server.scene.count=2;
 g_window_server.display_width=640;g_window_server.display_height=480;
 g_window_server.pointer_x=60;g_window_server.pointer_y=100;
 move(INPUT_REL_X,0);printf("POINTER_OPTIMIZED first-motion hit_calls=%u candidate_visits=%u\n",hit_calls,hit_visits);expect(0,1,2,OS_CONTROL_ENTER);expect(1,1,2,OS_CONTROL_MOVE);
 assert(!updates&&!cursors&&g_window_server.scene.hovered_window_identifier==1);
 button(INPUT_VALUE_PRESS);assert(a.control_pressed==2&&a.client_button_capture);
 count=0;move(INPUT_REL_X,40);
 /* 重叠处仍在 A 的按钮局部范围内，但实际命中 B。LEAVE A -> ENTER B -> MOVE A。 */
 assert(count==3);expect(0,1,2,OS_CONTROL_LEAVE);expect(1,2,12,OS_CONTROL_ENTER);expect(2,1,2,OS_CONTROL_MOVE);
 assert(records[0].x==59&&records[0].y==(int32_t)g_window_server.pointer_y-40-(int32_t)window_client_offset_y(a.flags));
 assert(!a.control_hover&&b.control_hover==12&&a.control_pressed==2&&updates==1&&cursors==1&&g_window_server.focused_identifier==1);
 count=0;button(INPUT_VALUE_RELEASE);assert(count==1);expect(0,1,2,OS_CONTROL_UP);
 assert(!a.control_pressed&&!a.client_button_capture&&!activations);
 count=0;move(INPUT_REL_X,1);assert(count==1);expect(0,2,12,OS_CONTROL_MOVE);
 /* 窗口 Z 序在指针不动时变化，仍由上次实际所有者发出 LEAVE。 */
 g_window_server.scene.children[0]=&b;g_window_server.scene.children[1]=&a;
 count=0;move(INPUT_REL_X,0);assert(count==3);expect(0,2,12,OS_CONTROL_LEAVE);expect(1,1,2,OS_CONTROL_ENTER);expect(2,1,2,OS_CONTROL_MOVE);
 unsigned before=updates;count=0;move(INPUT_REL_WHEEL,1);assert(updates==before&&count==1);expect(0,1,2,OS_CONTROL_WHEEL);
 /* 桌面菜单消费移动之前，必须清掉下方窗口悬停。 */
 g_window_server.desktop.menu_open=true;count=0;move(INPUT_REL_X,0);assert(count==1);expect(0,1,2,OS_CONTROL_LEAVE);
 assert(!a.control_hover&&!g_window_server.scene.hovered_window_identifier);
 button(INPUT_VALUE_PRESS);button(INPUT_VALUE_RELEASE);
 g_window_server.desktop.menu_open=false;count=0;move(INPUT_REL_X,0);assert(count==2);expect(0,1,2,OS_CONTROL_ENTER);expect(1,1,2,OS_CONTROL_MOVE);
 /* 显式菜单捕获也必须保留实际悬停切换，但点击继续交给捕获者。 */
 a.client_pointer_capture=true;g_window_server.scene.children[0]=&a;g_window_server.scene.children[1]=&b;
 count=0;move(INPUT_REL_X,0);assert(count==2);expect(0,1,2,OS_CONTROL_LEAVE);expect(1,2,12,OS_CONTROL_ENTER);
 count=0;button(INPUT_VALUE_PRESS);assert(!a.control_pressed&&!b.control_pressed&&!activations);
 button(INPUT_VALUE_RELEASE);a.client_pointer_capture=false;
 /* 指针跨到桌面根，旧窗口先离开，图标悬停与窗口命中互斥。 */
 count=0;move(INPUT_REL_X,-1000);move(INPUT_REL_Y,-1000);
 assert(!a.control_hover&&!b.control_hover&&!g_window_server.scene.hovered_window_identifier&&g_window_server.desktop_hovered_app==7);
 assert(g_window_server.pointer_x==0&&g_window_server.pointer_y==0);
 before=updates;unsigned old_cursor=cursors;
 for(unsigned i=0;i<1000;++i)move(INPUT_REL_X,INT32_MIN);
 assert(updates==before&&cursors==old_cursor);
 /* 键盘事件不改变指针命中和坐标。 */
 unsigned key_hits=hit_calls;event=(input_event_t){.timestamp=99999,.type=INPUT_EVENT_KEY,.code=0x04,.value=INPUT_VALUE_PRESS};route_input_locked(&event);
 assert(hit_calls==key_hits&&updates==before&&!g_window_server.scene.hovered_window_identifier);
 /* 范围控件使用真实计算路径：按下、越界拖动、移回和释放都不能重复查找几何。 */
 g_window_server.scene.count=1;g_window_server.scene.children[0]=&a;
 an[1].kind=OS_CONTROL_SLIDER;an[1].width=100;an[1].height=40;an[1].value=5000;
 ++a.control_revision;g_window_server.pointer_x=60;g_window_server.pointer_y=100;
 find_layouts=0;count=0;move(INPUT_REL_X,0);button(INPUT_VALUE_PRESS);
 assert(a.control_pressed==2&&a.client_button_capture&&!find_layouts);
 count=0;move(INPUT_REL_X,500);
 assert(records[count-2].action==OS_CONTROL_CHANGE&&records[count-2].value==10000);
 assert(records[count-1].action==OS_CONTROL_MOVE&&!find_layouts);
 count=0;move(INPUT_REL_X,-500);button(INPUT_VALUE_RELEASE);
 assert(!a.control_pressed&&!a.client_button_capture&&!find_layouts&&an[1].value==5000);
 /* 标题栏空白按下被拖动逻辑消费时仍清理旧文字选区，松开不重复清理。 */
 a.x=40;a.y=40;a.width=200;a.height=140;
 g_window_server.pointer_x=60;g_window_server.pointer_y=50;
 unsigned cleared=selection_clears;button(INPUT_VALUE_PRESS);
 assert(selection_clears==cleared+1&&g_window_server.dragging_identifier==a.identifier);
 button(INPUT_VALUE_RELEASE);assert(selection_clears==cleared+1&&!g_window_server.dragging_identifier);
 /* 真实最大化/还原：鼠标停在旧按钮，不追加任何物理移动事件。 */
 an[1].kind=OS_CONTROL_BUTTON;an[1].width=100;an[1].height=80;
 a.flags|=OS_WINDOW_RESIZABLE;
 int32_t bx,by;
 assert(window_caption_button_rect(a.flags,a.x,a.y,a.width,WINDOW_TITLE_BUTTON_MAXIMIZE,&bx,&by));
 g_window_server.pointer_x=(uint32_t)bx+10;g_window_server.pointer_y=(uint32_t)by+10;
 move(INPUT_REL_X,0);
 assert(g_window_server.scene.hovered_window_identifier==1&&g_window_server.scene.hovered_title_button==WINDOW_TITLE_BUTTON_MAXIMIZE);
 unsigned old_updates=updates,old_cursors=cursors;
 assert(window_toggle_maximize_locked(&a)&&a.maximized);
 assert(g_window_server.scene.hovered_title_button==WINDOW_TITLE_BUTTON_NONE);
 assert(updates==old_updates&&cursors==old_cursors);
 assert(window_caption_button_rect(a.flags,a.x,a.y,a.width,WINDOW_TITLE_BUTTON_MAXIMIZE,&bx,&by));
 g_window_server.pointer_x=(uint32_t)bx+10;g_window_server.pointer_y=(uint32_t)by+10;
 move(INPUT_REL_X,0);
 assert(g_window_server.scene.hovered_title_button==WINDOW_TITLE_BUTTON_MAXIMIZE);
 old_updates=updates;old_cursors=cursors;
 assert(window_toggle_maximize_locked(&a)&&!a.maximized);
 assert(!g_window_server.scene.hovered_window_identifier&&!g_window_server.scene.hovered_title_button&&!a.control_hover);
 assert(updates==old_updates&&cursors==old_cursors);
 /* 还原后露出另一窗口，其标题栏命中立即接管，但不得改变键盘焦点。 */
 b.x=0;b.y=0;b.width=638;b.height=448;
 g_window_server.scene.children[0]=&b;g_window_server.scene.children[1]=&a;g_window_server.scene.count=2;
 assert(window_toggle_maximize_locked(&a));
 assert(window_caption_button_rect(a.flags,a.x,a.y,a.width,WINDOW_TITLE_BUTTON_MAXIMIZE,&bx,&by));
 g_window_server.pointer_x=(uint32_t)bx+10;g_window_server.pointer_y=(uint32_t)by+10;move(INPUT_REL_X,0);
 g_window_server.focused_identifier=1;
 assert(window_toggle_maximize_locked(&a));
 assert(g_window_server.scene.hovered_window_identifier==2&&g_window_server.scene.hovered_title_button==WINDOW_TITLE_BUTTON_MAXIMIZE);
 assert(g_window_server.focused_identifier==1);
 puts("Stationary pointer: real maximize/restore clears the old title button and re-hits exposed windows without moving the cursor or focus PASS");
 /* 最小化前的旧控件必须先 LEAVE，下方控件再 ENTER；松开所有捕获。 */
 a.x=b.x=40;a.y=b.y=40;a.width=b.width=200;a.height=b.height=140;
 g_window_server.pointer_x=60;g_window_server.pointer_y=100;move(INPUT_REL_X,0);
 assert(a.control_hover==2&&!b.control_hover);
 a.client_pointer_capture=a.client_button_capture=true;g_window_server.focused_identifier=1;
 count=0;old_updates=updates;old_cursors=cursors;
 assert(window_minimize_locked(&a)&&a.minimized);
 assert(count==2&&records[0].window==1&&records[0].action==OS_CONTROL_LEAVE&&records[1].window==2&&records[1].action==OS_CONTROL_ENTER);
 assert(!a.control_hover&&!a.control_pressed&&!a.client_pointer_capture&&!a.client_button_capture);
 assert(b.control_hover==12&&g_window_server.scene.hovered_window_identifier==2);
 assert(updates==old_updates&&cursors==old_cursors);
 puts("Stationary minimize: ordered LEAVE/ENTER, exposed control hover and capture cleanup PASS");
 /* 主动激活底层窗口后，不移动鼠标也应有序切换控件悬停。 */
 a.minimized=false;g_window_server.focused_identifier=2;count=0;
 unsigned old_hits=hit_calls;old_updates=updates;old_cursors=cursors;
 assert(window_server_focus(1)==K_OK);
 assert(count==2&&records[0].window==2&&records[0].action==OS_CONTROL_LEAVE&&records[1].window==1&&records[1].action==OS_CONTROL_ENTER);
 assert(a.control_hover==2&&!b.control_hover&&g_window_server.scene.hovered_window_identifier==1);
 assert(hit_calls==old_hits+1&&updates==old_updates&&cursors==old_cursors);
 count=0;old_hits=hit_calls;
 assert(window_server_focus(999)==K_ENOENT&&!count&&hit_calls==old_hits);
 puts("Stationary focus: public activation updates ordered LEAVE/ENTER without cursor motion; missing target retains hover PASS");

 puts("Pointer scene: real full router/control layout, ordered cross-window events, overlap release, retained drag, z-order changes, desktop menu/root, explicit capture, wheel and saturation passed");
}
'''
range_code=function(control,'static uint32_t control_find_button(')+function(control,'static void control_change(')+function(control,'static void control_range_pointer(')
program=preamble+lookup+function(control,'static uint32_t control_target_index(')+geometry+range_code+hit+scene+pointer+function(control,'bool window_control_caption_hit_locked(')+bridge+title+mark+clear+common+function(router,'static bool window_toggle_maximize_locked(')+function(router,'void window_input_release_pointer_locked(')+function(router,'bool window_minimize_locked(')+function(window_source,'kstatus_t window_server_focus(')+tests
with tempfile.TemporaryDirectory(prefix='pointer-scene-') as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    args=['cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(root/'include'),str(path/'test.c'),str(root/'kernel/graphics/window_geometry.c'),str(root/'kernel/graphics/control_tree.c'),'-o',str(path/'test')]
    subprocess.run(args,check=True)
    subprocess.run([str(path/'test')],check=True)

    # 恢复旧捕获短路、遮挡误判和旧坐标推断，必须由同一真实事件序列检出。
    variants={
        'geometry-hover-not-refreshed':program.replace('    window_input_refresh_pointer_locked();', '    (void)0;'),
        'title-selection-not-cleared':program.replace('window_control_clear_text_selection_locked(target);',
            '(void)window_control_clear_text_selection_locked;'),
        'range-repeated-find': program.replace('if (window->control_range_target == target &&',
            'if (false && window->control_range_target == target &&'),
        'capture-before-scene': program.replace('    window_server_window_t *pointer_hit = window_route_pointer_scene_locked(event);',
            '    if (route_client_capture_locked(event)) return;\n    window_server_window_t *pointer_hit = window_route_pointer_scene_locked(event);'),
        'covered-window-hit': program.replace('e.type==OS_INPUT_EVENT_KEY||g_window_server.scene.hovered_window_identifier==w->identifier,true)', 'true,false)'),
        'old-coordinate-owner': program.replace('window_scene_find_locked(\n        g_window_server.scene.hovered_window_identifier)', 'window_scene_hit_test_locked(old_x, old_y)'),
    }
    for name,broken in variants.items():
        assert broken!=program,name
        (path/'test.c').write_text(broken)
        subprocess.run(args,check=True)
        result=subprocess.run([str(path/'test')],capture_output=True)
        assert result.returncode!=0, name+' 未触发回归断言'
        print('Negative control rejected:',name)
