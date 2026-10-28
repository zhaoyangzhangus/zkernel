"""真实提交实现的失败回滚、所有权和快照生命周期回归。"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/control_document.c').read_text()
body = source[source.index('typedef struct control_font_cache {'):source.index('typedef struct control_paint_context {')]
zorder = (root / 'kernel/graphics/zorder.c').read_text()
body = body.replace('memset(', 'counted_memset(').replace('os_utf8_next(', 'counted_utf8_next(')
body += zorder[zorder.index('static void window_scene_change_focus_locked('):zorder.index('void window_scene_focus_locked(')]
prefix = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <kernel/control_tree.h>
#include <kernel/control_selection.h>
#include <kernel/control_text.h>
#include <uapi/utf8.h>
#include <uapi/control_validation.h>
#include <ascii_font.h>
#include <uapi/window.h>
#include "user/runtime/message_box.h"
uint32_t ui_text_line_height(void){return 20;}
uint32_t ui_text_size(void){return 20;}
uint32_t ui_text_measure_range(const char *text,size_t n){(void)text;return (uint32_t)n*10;}
#define __user
typedef int kstatus_t;
enum {K_OK=0,K_EINVAL=-1,K_EPERM=-2,K_ENOMEM=-3,K_EFAULT=-4,K_EIO=-5};
typedef struct window_control_copy_request { struct window_control_document *document;uint32_t first,size; } window_control_copy_request_t;
static int locked;
static char clipboard[1024];static size_t clipboard_size;static bool clipboard_fail;
static kstatus_t clipboard_set_text(const char *text,size_t size){assert(!locked);if(clipboard_fail)return K_ENOMEM;assert(size<=sizeof(clipboard));memcpy(clipboard,text,size);clipboard_size=size;return K_OK;}
typedef struct process {int id;} process_t;
typedef struct window_server_window {
 process_t *owner;uint32_t width,height;bool dirty;struct window_control_document *controls;
 uint32_t control_hover,control_pressed,control_focus;bool client_button_capture;
 uint32_t control_text_target,control_text_anchor,control_text_end,control_text_ctrl,control_text_font;
 uint8_t control_text_drag;Rect control_text_bounds;
 uint64_t control_revision;
 uint32_t control_key_target,control_key_code,control_shift;
 Rect control_hover_damage;uint32_t control_input_width,control_input_height;
 Rect control_range_bounds;uint32_t control_range_target,control_range_width,control_range_height,control_range_flags;
 uint64_t control_range_revision;
 int32_t x,y;uint32_t flags;
} window_server_window_t;
static process_t owner={1},other={2};
static window_server_window_t window;
static bool pointer_over_window=true;
static window_server_window_t *window_scene_pointer_window_locked(void){return pointer_over_window?&window:0;}
static struct {uint32_t pointer_x,pointer_y,focused_identifier,display_width,display_height;}
 g_window_server={.display_width=1920,.display_height=1080};
static uint32_t window_client_offset_x(uint32_t flags){(void)flags;return 1;}
static uint32_t window_client_offset_y(uint32_t flags){(void)flags;return 41;}
static int locked,allocations,fail_alloc,live,copies,fail_copy,lookups,close_at,notifies,marked;
static bool transfer_owner,resize_on_copy,fail_visit;
static size_t allocated_bytes,zeroed_bytes,decodes;
static uint32_t counted_utf8_next(const char *text,size_t length,size_t *used){++decodes;return os_utf8_next(text,length,used);}
static void *counted_memset(void *p,int value,size_t size){if(!value)zeroed_bytes+=size;return memset(p,value,size);}
static void *kmalloc(size_t size,int flags){
 (void)flags;assert(!locked);if(++allocations==fail_alloc)return 0;
 void *p=malloc(size);assert(p);memset(p,0xa5,size);++live;allocated_bytes+=size;return p;
}
static void *kzalloc(size_t size,int flags){void *p=kmalloc(size,flags);if(p)counted_memset(p,0,size);return p;}
static void kfree(void *p){assert(!locked);if(p){--live;free(p);}}
static unsigned font_builds,glyph_builds;static bool fail_font;
bool ui_font_copy_glyph(uint32_t code,uint32_t w,uint32_t h,uint8_t *out,size_t size){
 assert(!locked&&code&&w&&h&&size==(size_t)w*h);++glyph_builds;if(fail_font)return false;
 memset(out,(uint8_t)(code%255+1),size);return true;
}
uint32_t ascii_font_width(void){return 8;}
uint32_t ascii_font_height(void){return 16;}
bool ascii_font_copy_cache(uint32_t w,uint32_t h,uint8_t *out,size_t size){
 assert(!locked&&w==h/2&&h<=96&&size==(size_t)w*h*95);++font_builds;
 if(fail_font)return false;
 memset(out,117,size);return true;
}
static void window_lock(void){assert(!locked);locked=1;}
static void window_unlock(void){assert(locked);locked=0;}
static window_server_window_t *window_scene_find_locked(uint32_t id){
 assert(locked);++lookups;return id==7 && lookups!=close_at?&window:0;
}
static kstatus_t copy_from_user(void *dst,const void *src,size_t size){
 assert(!locked);if(++copies==fail_copy)return K_EFAULT;memcpy(dst,src,size);
 if(transfer_owner)window.owner=&other;
 if(resize_on_copy){window.width=640;resize_on_copy=false;}
 return K_OK;
}
static void window_mark_window_locked(window_server_window_t *w){assert(locked&&w==&window);++marked;}
static unsigned caption_marks;
static void window_mark_rect_locked(int32_t x,int32_t y,uint32_t width,uint32_t height){
 assert(locked&&x>=0&&y>=0&&width&&height);++caption_marks;
}
static unsigned small_marks;static uint64_t damage_pixels;static Rect last_damage;
static unsigned indexed_validations,revalidations,find_walks,locate_walks,locked_node_reads,visit_walks;
static unsigned text_bounds_calls,target_queries;
static unsigned focus_walks;static bool fail_focus;
static bool counted_focus_query(const os_control_node_t *n,uint32_t count,Rect view,
 uint32_t current,bool reverse,uint32_t *next,control_tree_box_t *box,
 control_tree_visit_fn visit,void *context){
 ++focus_walks;
 bool valid=control_tree_focus_next_query(n,count,view,current,reverse,next,box,visit,context);
 return valid&&!fail_focus;
}
static bool counted_visit(const os_control_node_t *n,uint32_t count,Rect view,
 control_tree_visit_fn visit,void *context){
 ++visit_walks;bool valid=control_tree_visit_validated(n,count,view,visit,context);return valid&&!fail_visit;
}
static bool counted_locate_query(const os_control_node_t *n,uint32_t count,Rect view,
 int32_t x,int32_t y,uint32_t *index,control_tree_box_t *box,control_tree_visit_fn visit,void *context){
 ++visit_walks;return control_tree_locate_query(n,count,view,x,y,index,box,visit,context);
}
static uint32_t counted_find(const os_control_node_t *n,uint32_t count,Rect view,
 uint32_t id,control_tree_box_t *box){
 ++find_walks;return control_tree_find(n,count,view,id,box);
}
static uint32_t counted_locate(const os_control_node_t *n,uint32_t count,Rect view,
 int32_t x,int32_t y,control_tree_box_t *box){
 ++locate_walks;return control_tree_locate(n,count,view,x,y,box);
}
static bool counted_indexed(const os_control_node_t *n,uint32_t count,uint32_t text,uint16_t *order){
 ++indexed_validations;return os_control_tree_validate_indexed(n,count,text,order);
}
bool counted_layout(const os_control_node_t *n,uint32_t count,uint32_t text,Rect view,control_tree_box_t *boxes){
 ++revalidations;return control_tree_layout(n,count,text,view,boxes);
}
bool counted_validate(const os_control_node_t *n,uint32_t count,uint32_t text){
 ++revalidations;return control_tree_validate(n,count,text);
}
#define os_control_tree_validate_indexed counted_indexed
#define control_tree_layout counted_layout
#define control_tree_validate counted_validate
#define control_tree_visit_validated counted_visit
#define control_tree_find counted_find
#define control_tree_locate_query counted_locate_query
#define control_tree_locate counted_locate
#define control_tree_focus_next_query counted_focus_query
static void window_mark_surface_locked(const window_server_window_t *w,int32_t x,int32_t y,uint32_t width,uint32_t height){
 assert(locked&&w==&window&&x>=0&&y>=0&&width&&height);
 assert((uint64_t)x+width<=w->width&&(uint64_t)y+height<=w->height);
 ++small_marks;damage_pixels+=(uint64_t)width*height;last_damage=(Rect){x,y,x+(int32_t)width,y+(int32_t)height};
}
static void window_server_notify_worker(void){assert(!locked);++notifies;}
static unsigned caption_actions;
static void window_control_caption_action_locked(window_server_window_t *w,uint32_t action){assert(locked&&w==&window);caption_actions=action;}
static os_control_event_t events[128];static unsigned event_count;
bool window_control_event_metadata_locked(const window_server_window_t *,uint32_t,os_control_event_t *);
static void window_enqueue_control_event_locked(window_server_window_t *w,const os_input_event_t *in,
 uint32_t target,uint16_t action,int32_t x,int32_t y){
 (void)x;(void)y;assert(locked&&w==&window&&event_count<128);
 os_control_event_t *e=events+event_count++;
 *e=(os_control_event_t){.timestamp=in->timestamp,.target=target,.action=action,.code=in->code,.value=in->value};
 assert(window_control_event_metadata_locked(w,target,e));
}
'''
main = r'''
int main(void){
 _Static_assert(sizeof(os_window_control_tree_t)==48,"tree ABI");
 window=(window_server_window_t){.owner=&owner,.width=640,.height=360};
 os_control_node_t nodes[3]={
  {.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER},
  {.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,.width=100,.height=40,.text_length=2,.flags=OS_CONTROL_NO_HOVER}};
 char text[]="OK";
 os_window_control_tree_t request={.identifier=7,.count=2,.nodes=(uintptr_t)nodes,
  .text=(uintptr_t)text,.text_size=2};
 assert(window_server_control_tree(&other,&request)==K_EPERM&&!live&&!allocations);
 assert(window_server_control_tree(&owner,&request)==K_OK&&live==4&&window.controls);
 /* 同文档刷新必须保留已排队事件、鼠标捕获和按键配对，且不制造新损坏。 */
 window_control_document_t *identical=window.controls;uint64_t same_revision=request.revision;
 window.dirty=false;window.control_hover=window.control_pressed=window.control_focus=2;
 window.control_key_target=2;window.control_key_code=0x2c;window.control_shift=1;
 window.client_button_capture=true;
 for(unsigned i=0;i<32;++i){
  assert(window_server_control_tree(&owner,&request)==K_OK&&window.controls==identical&&live==4);
  assert(request.revision==same_revision&&!window.dirty&&marked==1&&notifies==1);
  assert(window.control_hover==2&&window.control_pressed==2&&window.control_focus==2);
  assert(window.control_key_target==2&&window.control_key_code==0x2c&&window.control_shift==1);
  assert(window.client_button_capture);
 }
 window.control_hover=window.control_pressed=window.control_focus=window.control_key_target=0;
 window.control_key_code=window.control_shift=0;window.client_button_capture=false;
 window_control_document_t *old=window.controls;
 assert(old->layout_valid&&old->layout_width==640);
 _Static_assert(sizeof(os_control_event_t)==48,"control event ABI");
 os_input_event_t input={.type=OS_INPUT_EVENT_RELATIVE};
 window_lock();
 assert(window_control_route_locked(&window,&input,20,20,true,false));
 assert(window.control_hover==2&&events[0].action==OS_CONTROL_ENTER&&events[0].target==2);
 assert(marked==1&&small_marks==1&&damage_pixels==4000);
 assert(last_damage.x0==0&&last_damage.y0==0&&last_damage.x1==100&&last_damage.y1==40);
 unsigned stationary=small_marks;
 window_control_route_locked(&window,&input,21,21,true,false);
 assert(small_marks==stationary&&marked==1);
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
 assert(window_control_route_locked(&window,&input,20,20,true,false)&&window.control_pressed==2&&window.client_button_capture);
 input.value=OS_INPUT_VALUE_RELEASE;
 assert(window_control_route_locked(&window,&input,20,20,true,false)&&!window.control_pressed&&!window.client_button_capture);
 assert(events[event_count-1].action==OS_CONTROL_CLICK&&events[event_count-1].target==2);
 /* 捕获被弹窗接管时，只重绘原按钮，不制造释放或激活事件。 */
 input.value=OS_INPUT_VALUE_PRESS;window_control_route_locked(&window,&input,20,20,true,false);
 int cancel_full=marked;unsigned cancel_small=small_marks,cancel_events=event_count,cancel_finds=find_walks;
 uint64_t cancel_pixels=damage_pixels;
 window.control_key_target=2;window.control_key_code=0x2c;window.control_shift=1;
 assert(window_control_cancel_pointer_press_locked(&window));
 assert(!window.control_pressed&&!window.client_button_capture);
 assert(window.control_hover==2&&window.control_focus==2&&window.control_key_target==2&&window.control_key_code==0x2c&&window.control_shift==1);
 assert(marked==cancel_full&&small_marks==cancel_small+1&&damage_pixels-cancel_pixels==4000);
 assert(find_walks==cancel_finds+1&&event_count==cancel_events);
 assert(!window_control_cancel_pointer_press_locked(&window));
 assert(small_marks==cancel_small+1&&find_walks==cancel_finds+1);
 window.control_pressed=99;window.client_button_capture=true;
 assert(window_control_cancel_pointer_press_locked(&window));
 assert(!window.control_pressed&&!window.client_button_capture);
 assert(small_marks==cancel_small+1&&find_walks==cancel_finds+1);
 assert(!window_control_cancel_pointer_press_locked(0));
 window.control_key_target=window.control_key_code=window.control_shift=0;
 input.value=OS_INPUT_VALUE_RELEASE;window_control_route_locked(&window,&input,20,20,true,false);
 assert(event_count==cancel_events);
 input.value=OS_INPUT_VALUE_PRESS;window_control_route_locked(&window,&input,20,20,true,false);
 input.value=OS_INPUT_VALUE_RELEASE;window_control_route_locked(&window,&input,-10,-10,true,false);
 assert(events[event_count-1].action==OS_CONTROL_UP&&!window.control_hover&&!window.control_pressed);
 input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=42};
 window_control_route_locked(&window,&input,0,0,true,false);
 assert(events[event_count-1].action==OS_CONTROL_KEY&&events[event_count-1].target==2);
 input=(os_input_event_t){.type=OS_INPUT_EVENT_RELATIVE};
 window_control_route_locked(&window,&input,20,20,true,false);window_control_pointer_hover_locked(&window,&input,false);
 assert(!window.control_hover&&events[event_count-1].action==OS_CONTROL_LEAVE);
 window_unlock();marked=notifies=1;
 window_control_document_get(old);
 /* 新文档的四处基础分配均须覆盖，不能被幂等快速路径绕过。 */
 ++nodes[1].foreground;
 for(int i=1;i<=4;++i){
  fail_alloc=allocations+i;
  assert(window_server_control_tree(&owner,&request)==K_ENOMEM&&live==4&&window.controls==old);
  fail_alloc=0;
 }
 for(int i=1;i<=2;++i){
  fail_copy=copies+i;
  assert(window_server_control_tree(&owner,&request)==K_EFAULT&&live==4&&window.controls==old);
  fail_copy=0;
 }
 nodes[1].surface=2;nodes[1].kind=OS_CONTROL_SURFACE;
 assert(window_server_control_tree(&owner,&request)==K_EINVAL&&window.controls==old&&live==4);
 nodes[1].kind=OS_CONTROL_BUTTON;nodes[1].font_size=97;
 assert(window_server_control_tree(&owner,&request)==K_EINVAL&&window.controls==old&&live==4);
 nodes[1].font_size=0;nodes[1].x=INT32_MAX;
 assert(window_server_control_tree(&owner,&request)==K_EINVAL&&window.controls==old&&live==4);
 nodes[1].x=0;
 close_at=lookups+2;
 assert(window_server_control_tree(&owner,&request)==K_EPERM&&window.controls==old&&live==4);
 close_at=0;
 assert(window_server_control_tree(&owner,&request)==K_OK&&live==8&&window.controls!=old);
 assert(window.control_focus==2&&!window.control_pressed);
 text[0]='N';nodes[1].width=12;
 assert(old->text[0]=='O'&&old->nodes[1].width==100);
 window_control_document_put(old);assert(live==4);
 request.count=request.text_size=0;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!window.controls&&!live);
 assert(notifies==3&&marked==3&&!locked);
 request.reserved=1;assert(window_server_control_tree(&owner,&request)==K_EINVAL);
 request.reserved=0;request.count=2;request.text_size=2;nodes[1].flags=OS_CONTROL_DISABLED;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 window_lock();input=(os_input_event_t){.type=OS_INPUT_EVENT_RELATIVE};
 window_control_route_locked(&window,&input,5,5,true,false);
 assert(window.control_hover==1);
 assert(marked==4);
 window_unlock();nodes[1].flags=0;nodes[1].kind=OS_CONTROL_SURFACE;nodes[1].surface=1;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 window_lock();input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
 window_control_route_locked(&window,&input,5,5,true,false);
 assert(window.control_pressed==2&&window.client_button_capture);window_unlock();
 nodes[1].flags=OS_CONTROL_HIDDEN;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!window.client_button_capture&&!window.control_pressed&&!window.control_focus);
 window_lock();unsigned before=event_count;input.value=OS_INPUT_VALUE_RELEASE;
 window_control_route_locked(&window,&input,5,5,true,false);
 for(unsigned i=before;i<event_count;++i)assert(events[i].action!=OS_CONTROL_CLICK);
 window_unlock();request.count=request.text_size=0;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 /* 局部损坏必须与当前裁剪一致，非按钮不触发额外绘制，几何变化不能复用旧范围。 */
 nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,
  .x=-30,.y=-10,.width=100,.height=40};
 request.count=2;request.text_size=2;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 int full_before=marked;unsigned small_before=small_marks;uint64_t pixels_before=damage_pixels;
 window_lock();input=(os_input_event_t){.type=OS_INPUT_EVENT_RELATIVE};
 window_control_route_locked(&window,&input,5,5,true,false);
 assert(marked==full_before&&small_marks==small_before+1&&damage_pixels-pixels_before==70*30);
 assert(last_damage.x0==0&&last_damage.y0==0&&last_damage.x1==70&&last_damage.y1==30);
 window.width=40;window.height=20;window_control_pointer_hover_locked(&window,&input,false);
 assert(marked==full_before+1);
 window_control_route_locked(&window,&input,5,5,true,false);
 assert(last_damage.x1==40&&last_damage.y1==20);
 /* 捕获取消前再次缩小窗口，损坏范围必须使用新视口，而不是旧悬停缓存。 */
 window.width=30;window.height=15;window.control_pressed=2;window.client_button_capture=true;
 small_before=small_marks;
 assert(window_control_cancel_pointer_press_locked(&window));
 assert(marked==full_before+1&&small_marks==small_before+1);
 assert(last_damage.x1==30&&last_damage.y1==15);
 window.width=640;window.height=360;
 window_control_route_locked(&window,&input,200,200,true,false);assert(marked==full_before+2);
 small_before=small_marks;window_control_route_locked(&window,&input,210,210,true,false);
 assert(marked==full_before+2&&small_marks==small_before);
 window_unlock();
 nodes[1].x=nodes[1].y=20;
 nodes[2]=(os_control_node_t){.identifier=3,.parent=0,.kind=OS_CONTROL_BUTTON,
  .x=150,.y=20,.width=100,.height=40};request.count=3;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 window_lock();window_control_route_locked(&window,&input,50,30,true,false);
 full_before=marked;small_before=small_marks;pixels_before=damage_pixels;
 window_control_route_locked(&window,&input,200,30,true,false);
 assert(marked==full_before&&small_marks==small_before+2&&damage_pixels-pixels_before==8000);
 window_unlock();request.count=request.text_size=0;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 /* 托管焦点复用真实布局；Tab/Shift+Tab、按键配对和焦点丢失不能误激活。 */
 nodes[0].flags=OS_CONTROL_MANAGED_FOCUS;request.count=3;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 window_lock();event_count=0;
 input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=0x2b,.value=OS_INPUT_VALUE_PRESS};
 window_control_route_locked(&window,&input,0,0,true,false);assert(window.control_focus==2);
 assert(events[event_count-1].action==OS_CONTROL_FOCUS);
 window_control_route_locked(&window,&input,0,0,true,false);assert(window.control_focus==3);
 input.code=0xe1;window_control_route_locked(&window,&input,0,0,true,false);
 input.code=0x2b;window_control_route_locked(&window,&input,0,0,true,false);assert(window.control_focus==2);
 input.code=0xe1;input.value=OS_INPUT_VALUE_RELEASE;window_control_route_locked(&window,&input,0,0,true,false);
 assert(!window.control_shift);
 for(unsigned key=0;key<3;++key){
  input.code=key==0?0x28:key==1?0x58:0x2c;input.value=OS_INPUT_VALUE_PRESS;
  window_control_route_locked(&window,&input,0,0,true,false);assert(window.control_key_target==2);
  unsigned held=event_count;input.value=OS_INPUT_VALUE_REPEAT;
  window_control_route_locked(&window,&input,0,0,true,false);assert(event_count==held);
  input.value=OS_INPUT_VALUE_RELEASE;window_control_route_locked(&window,&input,0,0,true,false);
  assert(!window.control_key_target&&!window.control_key_code);
  assert(event_count==held+1&&events[held].action==OS_CONTROL_CLICK&&events[held].target==2);
  window_control_route_locked(&window,&input,0,0,true,false);assert(event_count==held+1);
 }
 input.code=0x2c;input.value=OS_INPUT_VALUE_PRESS;window_control_route_locked(&window,&input,0,0,true,false);
 input.code=0x2b;window_control_route_locked(&window,&input,0,0,true,false);assert(window.control_focus==3&&!window.control_key_target);
 unsigned cancelled=event_count;input.code=0x2c;input.value=OS_INPUT_VALUE_RELEASE;
 window_control_route_locked(&window,&input,0,0,true,false);assert(event_count==cancelled);
 input.value=OS_INPUT_VALUE_PRESS;window_control_route_locked(&window,&input,0,0,true,false);
 input.code=0xe5;window_control_route_locked(&window,&input,0,0,true,false);assert(window.control_shift);
 g_window_server.focused_identifier=7;
 window_scene_set_focus_identifier_locked(7);assert(window.control_shift&&window.control_key_target);
 window_scene_set_focus_identifier_locked(8);assert(!window.control_shift&&!window.control_key_target);
 cancelled=event_count;input.code=0x2c;input.value=OS_INPUT_VALUE_RELEASE;
 window_control_route_locked(&window,&input,0,0,true,false);assert(event_count==cancelled);
 window_unlock();
 /* 在按键释放前替换文档，不能把上一份控件的激活带进新文档。 */
 window_lock();input.code=0x28;input.value=OS_INPUT_VALUE_PRESS;
 window_control_route_locked(&window,&input,0,0,true,false);assert(window.control_key_target==3);
 window_unlock();assert(window_server_control_tree(&owner,&request)==K_OK&&window.control_key_target==3);
 /* 样式和其他控件变化保留配对，实际关联对象变化才取消激活。 */
 nodes[2].foreground=0x123456;
 assert(window_server_control_tree(&owner,&request)==K_OK&&window.control_key_target==3);
 ++nodes[1].foreground;
 assert(window_server_control_tree(&owner,&request)==K_OK&&window.control_key_target==3);
 ++nodes[2].user_data;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!window.control_key_target);
 window_lock();cancelled=event_count;input.value=OS_INPUT_VALUE_RELEASE;
 window_control_route_locked(&window,&input,0,0,true,false);assert(event_count==cancelled);
 window_unlock();
 nodes[2].flags=OS_CONTROL_DISABLED;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!window.control_focus);
 window_lock();input.code=0x2b;input.value=OS_INPUT_VALUE_PRESS;
 window_control_route_locked(&window,&input,0,0,true,false);assert(window.control_focus==2);
 window_control_route_locked(&window,&input,0,0,true,false);assert(window.control_focus==2);
 window_unlock();nodes[1].flags=OS_CONTROL_HIDDEN;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!window.control_focus);
 window_lock();window_control_route_locked(&window,&input,0,0,true,false);assert(!window.control_focus);
 input.code=0xe5;window_control_route_locked(&window,&input,0,0,true,false);assert(window.control_shift);
 window_unlock();request.count=0;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!live&&!window.control_shift);
 /* 真实消息框构造器与真实服务器联测，不能仅用手工树证明应用已经接入。 */
 ui_message_box_t message={0};ui_message_document_t dialog;
 for(unsigned mode=0;mode<=3;++mode){
  ui_message_show(&message,"Title","Message",mode);
  assert(ui_message_build(&message,7,window.width,window.height,&dialog));
  assert(window_server_control_tree(&owner,&dialog.request)==K_OK);
  uint32_t count=ui_message_count(mode),initial=ui_message_target(&message,count+1);
  assert(window.control_focus==initial);
  window_lock();event_count=0;
  input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=0x2b,.value=OS_INPUT_VALUE_PRESS};
  for(unsigned i=0;i<count;++i){
   window_control_route_locked(&window,&input,0,0,true,false);
   assert(window.control_focus==ui_message_target(&message,2+i));
  }
  assert(window.control_focus==initial);
  input.code=0x4f;window_control_route_locked(&window,&input,0,0,true,false);
  assert(window.control_focus==ui_message_target(&message,2));
  input.code=0x50;window_control_route_locked(&window,&input,0,0,true,false);
  assert(window.control_focus==initial);
  input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
  window_control_route_locked(&window,&input,0,0,true,false);
  input.value=OS_INPUT_VALUE_RELEASE;window_control_route_locked(&window,&input,0,0,true,false);
  assert(window.control_focus==initial);
  input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=0x2b,.value=OS_INPUT_VALUE_PRESS};
  window_control_route_locked(&window,&input,0,0,true,false);
  uint32_t selected=ui_message_target(&message,2);assert(window.control_focus==selected);
  window_unlock();
  /* 重建同一代几何时保留焦点，不恢复应用自有的默认选择。 */
  assert(window_server_control_tree(&owner,&dialog.request)==K_OK&&window.control_focus==selected);
  window_lock();input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=0x2c,.value=OS_INPUT_VALUE_PRESS};
  window_control_route_locked(&window,&input,0,0,true,false);
  input.value=OS_INPUT_VALUE_RELEASE;window_control_route_locked(&window,&input,0,0,true,false);
  assert(events[event_count-1].action==OS_CONTROL_CLICK&&events[event_count-1].target==selected);
  os_window_event_t semantic={.type=OS_WINDOW_EVENT_CONTROL,.control=events[event_count-1]};
  assert(ui_message_event(&message,&semantic)==ui_message_choice(mode,0)&&!message.active);
  window_unlock();
 }
 request.count=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 /* 原始 Surface 的局部更新必须平移到控件，而不是使用源坐标或整窗重绘。 */
 nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER};
 nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_SURFACE,
  .surface=1,.x=30,.y=50,.width=100,.height=100,.flags=OS_CONTROL_DISABLED};
 request.count=2;request.text_size=0;window.x=window.y=0;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 unsigned damage_before=small_marks;int whole=marked,allocated=allocations;
 window_lock();window_control_surface_damage_locked(&window,2,3,4,5);window_unlock();
 assert(small_marks==damage_before+1&&marked==whole&&allocations==allocated);
 assert(last_damage.x0==32&&last_damage.y0==53&&last_damage.x1==36&&last_damage.y1==58);
 /* 完全不可见的源更新仍进入快照，但只提交一个实际可见像素。 */
 window.x=-101;window.y=-141;
 damage_before=small_marks;
 window_lock();window_control_surface_damage_locked(&window,500,300,2,2);window_unlock();
 assert(small_marks==damage_before+1&&marked==whole);
 assert(last_damage.x0==100&&last_damage.y0==100&&last_damage.x1==101&&last_damage.y1==101);
 window.x=INT32_MAX;
 window_lock();window_control_surface_damage_locked(&window,0,0,1,1);window_unlock();
 assert(marked==whole+1);
 request.count=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 /* 字号来自计算样式，每种尺寸只生成一次；失败和快照引用都不能泄漏缓存。 */
 window.x=window.y=0;request.count=3;request.text_size=2;
 nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,.font_size=24};
 nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,.width=100,.height=40,
  .flags=OS_CONTROL_INHERIT_FONT,.text_length=1};
 nodes[2]=(os_control_node_t){.identifier=3,.parent=0,.kind=OS_CONTROL_TEXT,.width=100,.height=40,
  .flags=OS_CONTROL_INHERIT_FONT,.text_offset=1,.text_length=1};
 unsigned before_fonts=font_builds;
 assert(window_server_control_tree(&owner,&request)==K_OK&&live==5&&font_builds==before_fonts+1);
 old=window.controls;window_control_document_get(old);
 for(unsigned i=0;i<100;++i){
  nodes[2].foreground=i+1U;
  assert(window_server_control_tree(&owner,&request)==K_OK&&live==9);
  assert(font_builds==before_fonts+1&&window.controls->fonts[24]==old->fonts[24]);
 }
 window_control_document_put(old);old=window.controls;window_control_document_get(old);
 assert(live==5);
 nodes[0].font_size=28;
 fail_alloc=allocations+5;
 assert(window_server_control_tree(&owner,&request)==K_ENOMEM&&live==5&&window.controls==old);fail_alloc=0;
 fail_font=true;assert(window_server_control_tree(&owner,&request)==K_EIO&&live==5&&window.controls==old);fail_font=false;
 nodes[0].font_size=96;
 assert(window_server_control_tree(&owner,&request)==K_OK&&live==10&&window.controls->fonts[96]);
 window_control_document_put(old);assert(live==5);
 request.count=request.text_size=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 /* 混合 UTF-8 文字：重复字形只存一份，纯 ASCII 保持原快速路径。 */
 {
 const char unicode[]="中A中…";
 nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,.font_size=24};
 nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,.width=100,.height=40,
  .flags=OS_CONTROL_INHERIT_FONT,.text_length=sizeof(unicode)-1};
 request.count=2;request.text=(uintptr_t)unicode;request.text_size=sizeof(unicode)-1;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 control_text_run_t *run=window.controls->runs[1];
 assert(run&&run->count==4&&run->glyph_count==3&&run->width==84&&run->height==24);
 assert(run->cells[0].glyph==run->cells[2].glyph&&run->cells[0].end==24&&run->cells[1].end==36);
 assert(!window.controls->fonts[24]);
 unsigned builds=allocations;
 for(unsigned i=0;i<20;++i){
  assert(window_server_control_tree(&owner,&request)==K_OK&&window.controls->runs[1]==run);
  assert((unsigned)allocations==builds+(i+1)*3);
 }
 /* 文字控件前插入无关节点，再恢复原顺序；保留旧快照检查字形缓存生命周期。 */
 old=window.controls;window_control_document_get(old);unsigned before_glyphs=glyph_builds;
 nodes[2]=nodes[1];nodes[1]=(os_control_node_t){.identifier=3,.parent=0,
  .kind=OS_CONTROL_TEXT,.width=10,.height=10};request.count=3;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 bool moved_run_reused=window.controls->runs[2]==run;
 nodes[1]=nodes[2];request.count=2;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 printf("UNICODE_REORDER_OPTIMIZED glyph_builds=%u reused=%u\n",glyph_builds-before_glyphs,moved_run_reused);
 assert(moved_run_reused&&window.controls->runs[1]==run&&glyph_builds==before_glyphs);
 window_control_document_put(old);run=window.controls->runs[1];
 /* 与逐屏幕像素参考比较，包含负原点、混合字宽和局部裁剪。 */
 uint32_t frame[2+40*28];
 for(unsigned sample=0;sample<500;++sample){
  for(unsigned i=0;i<2+40*28;++i)frame[i]=0x12345678;
  int64_t x=(int)(sample%101)-80,y=(int)(sample%51)-30;
  Rect clip={(int)(sample%7),(int)(sample%5),30+(int)(sample%11),21+(int)(sample%8)};
  control_raster_target_t target={frame+1,40,clip};
  control_text_draw(&target,run,x,y,0xe36291);
  for(int py=0;py<28;++py)for(int px=0;px<40;++px){
   uint32_t expected=0x12345678;
   if(px>=clip.x0&&px<clip.x1&&py>=clip.y0&&py<clip.y1&&py>=y&&py<y+24){
    const uint32_t ends[]={24,36,60,84},codes[]={0x4e2d,'A',0x4e2d,0x2026};
    unsigned start=0;
    for(unsigned i=0;i<4;++i){
     if(px>=x+start&&px<x+ends[i])expected=control_blend_argb(expected,0xe36291|((codes[i]%255+1)<<24));
     start=ends[i];
    }
   }
   assert(frame[1+py*40+px]==expected);
  }
  assert(frame[0]==0x12345678&&frame[1+40*28]==0x12345678);
 }
 old=window.controls;int retained_live=live;nodes[0].font_size=28;
 for(unsigned i=1;i<=7;++i){
  fail_alloc=allocations+i;
  assert(window_server_control_tree(&owner,&request)==K_ENOMEM&&window.controls==old&&live==retained_live);
  fail_alloc=0;
 }
 fail_font=true;
 assert(window_server_control_tree(&owner,&request)==K_EIO&&window.controls==old&&live==retained_live);
 fail_font=false;assert(window_server_control_tree(&owner,&request)==K_OK&&window.controls->runs[1]->height==28);
 /* 大段重复中文按唯一字形计费，不按 UTF-8 字节数复制像素。 */
 char repeated[3000];for(unsigned i=0;i<1000;++i)memcpy(repeated+i*3,"中",3);
 request.text=(uintptr_t)repeated;request.text_size=sizeof(repeated);nodes[1].text_length=sizeof(repeated);
 nodes[0].font_size=96;
 size_t before_zero=zeroed_bytes;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 assert(zeroed_bytes-before_zero==sizeof(window_control_document_t)+2*sizeof(control_text_run_t*)+2048*sizeof(uint32_t));
 run=window.controls->runs[1];assert(run->count==1000&&run->glyph_count==1&&run->bytes<20000);
 old=window.controls;retained_live=live;
 /* 不同字形超出文档预算时必须原子拒绝，不能丢失已显示的旧树。 */
 for(unsigned i=0;i<1000;++i){unsigned c=0x4e00+i;
  repeated[i*3]=(char)(0xe0|(c>>12));repeated[i*3+1]=(char)(0x80|((c>>6)&63));repeated[i*3+2]=(char)(0x80|(c&63));}
 assert(window_server_control_tree(&owner,&request)==K_ENOMEM&&window.controls==old&&live==retained_live);
 const char malformed[]="中\xff\xe4\xb8";
 request.text=(uintptr_t)malformed;request.text_size=sizeof(malformed)-1;nodes[1].text_length=request.text_size;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 run=window.controls->runs[1];assert(run->count==4&&run->glyph_count==2);
 /* 完整 64 KiB 文本覆盖最大字节起点；临时记录带毒填充，只清空查重索引。 */
 char *maximum=malloc(OS_CONTROL_TREE_MAX_TEXT);assert(maximum);
 for(unsigned i=0;i<OS_CONTROL_TREE_MAX_TEXT-1;i+=3)memcpy(maximum+i,"中",3);
 maximum[OS_CONTROL_TREE_MAX_TEXT-1]='X';
 nodes[0].font_size=16;nodes[1].text_length=OS_CONTROL_TREE_MAX_TEXT;
 request.text=(uintptr_t)maximum;request.text_size=OS_CONTROL_TREE_MAX_TEXT;
 before_zero=zeroed_bytes;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 run=window.controls->runs[1];
 assert(run->count==21846&&run->glyph_count==2&&run->width==21845U*16U+8U);
 assert(run->cells[21845].byte_offset==65535&&run->cells[21845].end==run->width);
 for(unsigned i=0;i<21845;++i) {
  assert(run->cells[i].byte_offset==i*3&&run->cells[i].end==(i+1)*16U&&run->cells[i].glyph==0);
 }
 assert(run->cells[21845].glyph==1&&run->glyphs[0].code==0x4e2d&&run->glyphs[1].code=='X');
 size_t cleared=zeroed_bytes-before_zero;
 assert(cleared==sizeof(window_control_document_t)+2*sizeof(control_text_run_t*)+65536*sizeof(uint32_t));
 printf("Unicode temporary records: 436920 fewer cleared bytes for 64 KiB text; indexed offsets and glyphs verified\n");
 /* 长 ASCII 前缀不应在统计字符数时重复解码，包含嵌入零字节的原有逐字节行为。 */
 memset(maximum,'A',OS_CONTROL_TREE_MAX_TEXT-3);
 maximum[0]=maximum[32768]=maximum[65532]=0;
 memcpy(maximum+OS_CONTROL_TREE_MAX_TEXT-3,"中",3);
 decodes=0;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 assert(decodes==65535);
 run=window.controls->runs[1];
 unsigned reference_count=0,reference_width=0;
 for(size_t offset=0;offset<request.text_size;++reference_count) {
  size_t used;uint32_t code=os_utf8_next(maximum+offset,request.text_size-offset,&used);
  if(!used){used=1;code=0xfffd;}
  const control_text_cell_t *cell=run->cells+reference_count;
  reference_width+=os_control_text_cell_width(code,16);
  assert(cell->byte_offset==offset&&cell->end==reference_width);
  assert(run->glyphs[cell->glyph].code==code);
  offset+=used;
 }
 assert(run->count==reference_count&&run->width==reference_width);
 printf("Unicode ASCII prefix: 65535 decodes instead of 131068; complete 64 KiB cells match the previous decoding rule\n");
 /* 随机混合字节覆盖首个非 ASCII 位置、无效序列和文本字段里的零字节。 */
 unsigned random=37;
 for(unsigned sample=0;sample<200;++sample) {
  unsigned length=1+sample*127U%512U,prefix=sample%length;
  for(unsigned i=0;i<length;++i) {
   random=random*1664525U+1013904223U;
   maximum[i]=(char)(i<prefix?random%128U:random>>24);
  }
  maximum[prefix]=(char)0xff;
  nodes[1].identifier=1000+sample;nodes[1].text_length=length;request.text_size=length;
  decodes=0;
  assert(window_server_control_tree(&owner,&request)==K_OK);
  run=window.controls->runs[1];reference_count=reference_width=0;
  for(size_t offset=0;offset<length;++reference_count) {
   size_t used;uint32_t code=os_utf8_next(maximum+offset,length-offset,&used);
   if(!used){used=1;code=0xfffd;}
   const control_text_cell_t *cell=run->cells+reference_count;
   reference_width+=os_control_text_cell_width(code,16);
   assert(cell->byte_offset==offset&&cell->end==reference_width&&run->glyphs[cell->glyph].code==code);
   offset+=used;
  }
  assert(run->count==reference_count&&run->width==reference_width);
  assert(decodes==2U*reference_count-prefix);
 }
 free(maximum);
 request.count=request.text_size=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 request.text=(uintptr_t)text;
 }
 /* 上次点击的文字更新可与下一次按下交错，稳定按钮不能因此丢失松开后的点击。 */
 for(unsigned mode=0;mode<9;++mode){
  event_count=0;
  nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER};
  nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,
   .width=100,.height=40,.text_length=2};
  request.count=2;request.text_size=2;
  assert(window_server_control_tree(&owner,&request)==K_OK);
  input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
  window_lock();window_control_route_locked(&window,&input,10,10,true,false);window_unlock();
  assert(window.control_pressed==2&&window.client_button_capture);
  text[0]='X';
  switch(mode){
   case 1:nodes[1].flags=OS_CONTROL_HIDDEN;break;
   case 2:nodes[1].flags=OS_CONTROL_DISABLED;break;
   case 3:nodes[1].kind=OS_CONTROL_TEXT;break;
   case 4:request.count=1;break;
   case 5:nodes[0].identifier=9;break;
   case 6:nodes[1].identifier=3;break;
   case 7:nodes[1].x=200;break;
   case 8:nodes[0].flags=OS_CONTROL_MANAGED_FOCUS;break;
  }
  assert(window_server_control_tree(&owner,&request)==K_OK);
  assert(window.control_pressed==((mode==0||mode==7)?2U:0U));
  assert(window.client_button_capture==(mode==0||mode==7));
  event_count=0;input.value=OS_INPUT_VALUE_RELEASE;
  window_lock();window_control_route_locked(&window,&input,10,10,true,false);window_unlock();
  unsigned clicks=0;for(unsigned i=0;i<event_count;++i)clicks+=events[i].action==OS_CONTROL_CLICK;
  assert(clicks==(mode==0?1U:0U)&&!window.control_pressed&&!window.client_button_capture);
  request.count=request.text_size=0;
  assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 }
 /* 所有新交互控件走真实提交、命中、焦点、鼠标与键盘，不在测试里重写路由。 */
 _Static_assert(sizeof(os_control_node_t)==104,"node ABI");
 for(unsigned kind=OS_CONTROL_CHECKBOX;kind<=OS_CONTROL_SEPARATOR;++kind){
  for(unsigned vertical=0;vertical<2;++vertical){
   if(vertical&&!os_control_is_range(kind)&&kind!=OS_CONTROL_PROGRESS&&kind!=OS_CONTROL_SEPARATOR)continue;
   window.width=640;window.height=360;
   nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,
    .kind=OS_CONTROL_CONTAINER,.flags=OS_CONTROL_MANAGED_FOCUS|OS_CONTROL_FOCUS_ARROWS};
   nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=kind,.width=100,.height=40,
    .value=5000,.flags=vertical?OS_CONTROL_VERTICAL:0};
   if(vertical){nodes[1].width=40;nodes[1].height=100;}
   int cx=vertical?20:50,cy=vertical?50:20;
   if(os_control_is_interactive(kind))nodes[1].flags|=OS_CONTROL_DEFAULT_FOCUS;
   request.count=2;request.text_size=0;
   assert(window_server_control_tree(&owner,&request)==K_OK);
   assert(window.control_focus==(os_control_is_interactive(kind)?2U:0U));
   event_count=0;window_lock();
   input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
   window_control_route_locked(&window,&input,cx,cy,true,false);
   input.value=OS_INPUT_VALUE_RELEASE;window_control_route_locked(&window,&input,cx,cy,true,false);
   window_unlock();
   unsigned changes=0;
   for(unsigned e=0;e<event_count;++e)if(events[e].action==OS_CONTROL_CHANGE){
    ++changes;assert(events[e].target==2&&events[e].value==(os_control_is_range(kind)?5000:1));
   }
   assert(changes==(os_control_is_choice(kind)?1U:os_control_is_range(kind)?2U:0U));
   assert(!(window.controls->nodes[1].flags&OS_CONTROL_CHECKED));
   if(os_control_is_choice(kind)){
    nodes[1].flags|=OS_CONTROL_CHECKED;assert(window_server_control_tree(&owner,&request)==K_OK);
    event_count=0;window_lock();
    input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=0x2c,.value=OS_INPUT_VALUE_PRESS};
    window_control_route_locked(&window,&input,cx,cy,true,false);
    input.value=OS_INPUT_VALUE_RELEASE;window_control_route_locked(&window,&input,50,20,true,false);
    window_unlock();changes=0;
    for(unsigned e=0;e<event_count;++e)if(events[e].action==OS_CONTROL_CHANGE){++changes;assert(events[e].value==0);}
    assert(changes==(kind==OS_CONTROL_RADIO?0U:1U));
   }
   if(os_control_is_range(kind)){
    event_count=0;window_lock();
    input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
    window_control_route_locked(&window,&input,cx,cy,true,false);
    input=(os_input_event_t){.type=OS_INPUT_EVENT_ABSOLUTE};
    window_control_route_locked(&window,&input,-100,-100,true,false);
    assert(event_count>=2&&events[event_count-2].action==OS_CONTROL_CHANGE&&events[event_count-2].value==0);
    assert(events[event_count-1].action==OS_CONTROL_MOVE&&events[event_count-1].target==2);
    window_control_route_locked(&window,&input,1000,1000,true,false);
    assert(event_count>=2&&events[event_count-2].action==OS_CONTROL_CHANGE&&events[event_count-2].value==10000);
    assert(events[event_count-1].action==OS_CONTROL_MOVE&&events[event_count-1].target==2);
    input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_RELEASE};
    window_control_route_locked(&window,&input,1000,1000,true,false);
    assert(!window.client_button_capture);
    const unsigned codes[]={0x4a,0x4d,0x4f,0x50,0x51,0x52};
    const int values[]={0,10000,5100,4900,5100,4900};
    for(unsigned k=0;k<6;++k){
     event_count=0;input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=codes[k],.value=OS_INPUT_VALUE_PRESS};
     window_control_route_locked(&window,&input,50,20,true,false);
     assert(event_count==1&&events[0].action==OS_CONTROL_CHANGE&&events[0].value==values[k]);
     input.value=OS_INPUT_VALUE_RELEASE;window_control_route_locked(&window,&input,50,20,true,false);
     assert(event_count==1);
    }
    window_unlock();assert(window.controls->nodes[1].value==5000);
    nodes[1].value=10001;assert(window_server_control_tree(&owner,&request)==K_EINVAL);
    nodes[1].value=5000;
    if(kind==OS_CONTROL_SCROLLBAR){
     const unsigned pages[]={0,2500,5000,10000};
     for(unsigned p=0;p<4;++p){
      nodes[1].page_size=pages[p];
      assert(window_server_control_tree(&owner,&request)==K_OK);
      unsigned thumb=100*pages[p]/10000;if(thumb<12)thumb=12;
      unsigned travel=100-thumb,point=thumb/2+travel/4;
      event_count=0;window_lock();
      input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
      window_control_route_locked(&window,&input,vertical?20:(int)point,vertical?(int)point:20,true,false);
      assert(events[event_count-1].action==OS_CONTROL_CHANGE);
      assert(events[event_count-1].value==(int)(travel?(travel/4)*10000/travel:0));
      input.value=OS_INPUT_VALUE_RELEASE;
      window_control_route_locked(&window,&input,cx,cy,true,false);window_unlock();
      assert(!window.client_button_capture);
     }
     nodes[1].page_size=10001;assert(window_server_control_tree(&owner,&request)==K_EINVAL);
     nodes[1].page_size=0;
    }
   }
   nodes[1].flags|=OS_CONTROL_DISABLED;
   assert(window_server_control_tree(&owner,&request)==K_OK&&!window.control_focus);
   event_count=0;window_lock();
   input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
   window_control_route_locked(&window,&input,50,20,true,false);input.value=OS_INPUT_VALUE_RELEASE;
   window_control_route_locked(&window,&input,50,20,true,false);window_unlock();
   for(unsigned e=0;e<event_count;++e)assert(events[e].action!=OS_CONTROL_CHANGE);
   request.count=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
  }
 }
 /* 物理悬停准备与范围控件按下共用命中几何，不再单独查找同一控件。 */
 window=(window_server_window_t){.owner=&owner,.width=640,.height=360};
 nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
  .flags=OS_CONTROL_MANAGED_FOCUS};
 nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_SLIDER,
  .width=100,.height=40,.value=5000,.flags=OS_CONTROL_DEFAULT_FOCUS};
 request=(os_window_control_tree_t){.identifier=7,.count=2,.nodes=(uintptr_t)nodes};
 assert(window_server_control_tree(&owner,&request)==K_OK);
 g_window_server.pointer_x=51;g_window_server.pointer_y=61;
 event_count=find_walks=locate_walks=0;window_lock();
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
 window_control_pointer_hover_locked(&window,&input,true);
 assert(window_control_route_locked(&window,&input,50,20,true,true));
 fprintf(stderr,"RANGE_PRESS_LAYOUTS locate=%u find=%u\n",locate_walks,find_walks);
 assert(locate_walks==1&&!find_walks&&window.control_pressed==2&&window.client_button_capture);
 /* 捕获拖动移出窗口后继续复用原始范围，不把裁剪矩形当成数值坐标系。 */
 input=(os_input_event_t){.type=OS_INPUT_EVENT_ABSOLUTE};event_count=0;
 window_control_pointer_hover_locked(&window,&input,false);
 window_control_route_locked(&window,&input,1000,1000,false,true);
 assert(!find_walks&&events[event_count-2].action==OS_CONTROL_CHANGE&&events[event_count-2].value==10000);
 window_unlock();
 /* 应用在拖动中替换快照时，使用尚未发布的候选布局刷新几何。 */
 nodes[1].x=100;nodes[1].width=200;nodes[1].value=6000;pointer_over_window=false;
 assert(window_server_control_tree(&owner,&request)==K_OK&&window.control_pressed==2&&window.client_button_capture);
 event_count=find_walks=0;window_lock();
 window_control_route_locked(&window,&input,200,20,false,true);
 assert(!find_walks&&events[event_count-2].value==5000);
 window_unlock();nodes[1].width=OS_CONTROL_FILL;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 window_lock();window.width=840;event_count=find_walks=0;
 window_control_route_locked(&window,&input,470,20,false,true);
 assert(find_walks==1&&events[event_count-2].value==5000);
 window_control_route_locked(&window,&input,470,20,false,true);assert(find_walks==1);
 window.flags=OS_WINDOW_STANDARD_CAPTION_CONTROLS;
 window_control_route_locked(&window,&input,470,20,false,true);assert(find_walks==2);
 --window.control_range_revision;
 window_control_route_locked(&window,&input,470,20,false,true);assert(find_walks==3);
 window_control_route_locked(&window,&input,470,20,false,true);assert(find_walks==3);
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT};
 input.value=OS_INPUT_VALUE_RELEASE;
 window_control_route_locked(&window,&input,470,20,false,true);window_unlock();
 assert(!window.control_pressed&&!window.client_button_capture&&find_walks==3);
 pointer_over_window=true;window.flags=0;window.width=640;
 request.count=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 /* 无序的完整节点表验证排序索引、64 位关联值、直接定位和版本隔离。 */
 os_control_node_t *many=calloc(OS_CONTROL_TREE_MAX_NODES,sizeof(*many));assert(many);
 for(unsigned i=0;i<OS_CONTROL_TREE_MAX_NODES;++i)many[i]=(os_control_node_t){
  .identifier=UINT32_MAX-i*104729U,.parent=i?0:OS_CONTROL_NO_PARENT,
  .kind=i?OS_CONTROL_BUTTON:OS_CONTROL_CONTAINER,.width=10,.height=10,
  .user_data=UINT64_MAX-i};
 request.nodes=(uintptr_t)many;request.count=OS_CONTROL_TREE_MAX_NODES;
 unsigned indexed_before=indexed_validations;
 assert(window_server_control_tree(&owner,&request)==K_OK&&request.revision);
 assert(indexed_validations==indexed_before+1&&!revalidations);
 assert(sizeof(*window.controls->targets)==2);
 for(unsigned i=1;i<request.count;++i)
  assert(window.controls->nodes[window.controls->targets[i-1]].identifier<
         window.controls->nodes[window.controls->targets[i]].identifier);
 uint64_t first_revision=request.revision;os_control_event_t stale={0};
 window_lock();
 for(unsigned i=0;i<OS_CONTROL_TREE_MAX_NODES;++i){
  os_control_event_t e={.target=many[i].identifier};
  assert(window_control_event_metadata_locked(&window,e.target,&e));
  assert(e.node_index==i&&e.node_kind==many[i].kind&&e.user_data==many[i].user_data);
  assert(e.tree_revision==request.revision&&os_control_event_node(&request,many,&e)==many+i);
  e.node_index=OS_CONTROL_TREE_MAX_NODES;assert(!os_control_event_node(&request,many,&e));
  e.node_index=i;e.user_data^=1;assert(!os_control_event_node(&request,many,&e));
 }
 stale.target=many[1].identifier;assert(window_control_event_metadata_locked(&window,stale.target,&stale));
 assert(!window_control_event_metadata_locked(&window,0,&stale));window_unlock();
 window_control_document_t *retained_doc=window.controls;
 uint32_t last_id=many[request.count-1].identifier;
 many[request.count-1].identifier=many[0].identifier;
 int retained_allocations=live;
 assert(window_server_control_tree(&owner,&request)==K_EINVAL&&window.controls==retained_doc&&live==retained_allocations);
 many[request.count-1].identifier=last_id;
 os_control_node_t swap=many[1];many[1]=many[2];many[2]=swap;
 assert(window_server_control_tree(&owner,&request)==K_OK&&request.revision>first_revision);
 assert(!os_control_event_node(&request,many,&stale));
 window_lock();assert(window_control_event_metadata_locked(&window,stale.target,&stale));window_unlock();
 assert(stale.node_index==2&&os_control_event_node(&request,many,&stale)==many+2);
 uint64_t retained=request.revision;fail_alloc=allocations+1;
 assert(window_server_control_tree(&owner,&request)==K_ENOMEM&&request.revision==retained);
 fail_alloc=0;request.count=0;
 assert(window_server_control_tree(&owner,&request)==K_OK&&request.revision>retained&&!live);
 request.count=OS_CONTROL_TREE_MAX_NODES;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!os_control_event_node(&request,many,&stale));
 request.count=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);free(many);
 /* 同一棵树跨越标题栏和客户区：负坐标事件、Surface 投影、系统按钮保留区。 */
 window=(window_server_window_t){.owner=&owner,.width=640,.height=480,
  .flags=OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_CAPTION_HEIGHT(40)};
 os_control_node_t frame[]={
  {.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
   .flags=OS_CONTROL_WINDOW_FRAME|OS_CONTROL_POINTER_PASSTHROUGH,.width=-1,.height=-1},
  {.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,.x=200,.y=4,.width=40,.height=32},
  {.identifier=3,.parent=0,.kind=OS_CONTROL_SURFACE,.flags=OS_CONTROL_POINTER_PASSTHROUGH,
   .y=40,.width=-1,.height=-1,.surface=1}};
 request=(os_window_control_tree_t){.identifier=7,.count=3,.nodes=(uintptr_t)frame};
 assert(window_server_control_tree(&owner,&request)==K_OK);
 assert(window.controls->boxes[1].bounds.y0==-36);
 assert(window.controls->boxes[2].bounds.y0==0&&window.controls->boxes[2].bounds.y1==480);
 window_lock();
 assert(window_control_caption_hit_locked(&window,211,15));
 assert(!window_control_caption_hit_locked(&window,100,15));
 input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=4,.value=1};
 assert(!window_control_route_locked(&window,&input,210,-26,true,false));
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
 int frame_full_marks=marked;
 event_count=0;assert(window_control_route_locked(&window,&input,210,-26,true,false));
 assert(window.control_pressed==2&&window.client_button_capture);
 input.value=OS_INPUT_VALUE_RELEASE;assert(window_control_route_locked(&window,&input,210,-26,true,false));
 assert(!window.control_pressed&&!window.client_button_capture);
 assert(events[event_count-1].action==OS_CONTROL_CLICK&&events[event_count-1].target==2);
 assert(caption_marks&&marked==frame_full_marks);
 input.value=OS_INPUT_VALUE_PRESS;window_control_route_locked(&window,&input,210,-26,true,false);
 unsigned cancel_caption=caption_marks;
 assert(window_control_cancel_pointer_press_locked(&window));
 assert(caption_marks==cancel_caption+1&&marked==frame_full_marks);
 input.value=OS_INPUT_VALUE_RELEASE;
 assert(!window_control_route_locked(&window,&input,210,20,true,false));
 small_marks=0;window_control_surface_damage_locked(&window,10,20,30,40);
 assert(small_marks==1&&last_damage.x0==10&&last_damage.y0==20&&last_damage.y1==60);
 window_unlock();
 frame[1].x=550;assert(window_server_control_tree(&owner,&request)==K_OK);
 window_lock();assert(window_control_caption_hit_locked(&window,561,15));
 assert(window_control_route_locked(&window,&input,560,-26,true,false));window_unlock();
 frame[1].flags=OS_CONTROL_WINDOW_FRAME;assert(window_server_control_tree(&owner,&request)==K_EINVAL);
 frame[1].flags=0;window.flags|=OS_WINDOW_CLIENT_DECORATIONS;
 assert(window_server_control_tree(&owner,&request)==K_EINVAL);
 request.count=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 /* 真实按下/刷新/释放：普通数值刷新保留激活，目标换义或失效必须取消。 */
 for(unsigned mode=0;mode<15;++mode){
  window=(window_server_window_t){.owner=&owner,.width=640,.height=360};
  char key_text[]="AB";
  nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
   .flags=OS_CONTROL_MANAGED_FOCUS};
  nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,.width=100,.height=40,
   .flags=OS_CONTROL_DEFAULT_FOCUS,.text_length=1};
  nodes[2]=(os_control_node_t){.identifier=3,.parent=0,.kind=OS_CONTROL_TEXT,.y=50,.width=100,.height=40,
   .text_offset=1,.text_length=1};
  if(mode==13)nodes[1].kind=OS_CONTROL_CHECKBOX;
  if(mode==14){nodes[1].flags|=OS_CONTROL_SYMBOL;nodes[1].value=OS_CONTROL_SYMBOL_MENU;nodes[1].text_length=0;}
  request=(os_window_control_tree_t){.identifier=7,.count=3,.nodes=(uintptr_t)nodes,
   .text=(uintptr_t)key_text,.text_size=2};
  assert(window_server_control_tree(&owner,&request)==K_OK&&window.control_focus==2);
  window_lock();event_count=0;
  input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=0x28,.value=OS_INPUT_VALUE_PRESS};
  window_control_route_locked(&window,&input,0,0,true,false);window_unlock();assert(window.control_key_target==2);
  switch(mode){
   case 0:key_text[1]='C';break;
   case 1:key_text[0]='C';break;
   case 2:++nodes[1].user_data;break;
   case 3:nodes[1].kind=OS_CONTROL_CHECKBOX;break;
   case 4:nodes[1].flags|=OS_CONTROL_DISABLED;break;
   case 5:nodes[1].flags|=OS_CONTROL_HIDDEN;break;
   case 6:request.count=1;break;
   case 7:nodes[0].identifier=9;break;
   case 8:nodes[1].identifier=4;break;
   case 9:nodes[0].flags=0;nodes[1].flags=0;break;
   case 10:nodes[1].kind=OS_CONTROL_CHECKBOX;nodes[1].flags|=OS_CONTROL_CHECKED;break;
   case 11:nodes[1].x=60;nodes[1].foreground=0x123456;break;
   case 12:key_text[0]='C';key_text[1]='A';nodes[1].text_offset=1;nodes[2].text_offset=0;break;
   case 13:nodes[1].flags|=OS_CONTROL_CHECKED;break;
   case 14:nodes[1].value=OS_CONTROL_SYMBOL_HOME;break;
  }
  assert(window_server_control_tree(&owner,&request)==K_OK);
  bool preserve=mode==0||mode==11||mode==12;
  assert(window.control_key_target==(preserve?2U:0U));
  window_lock();event_count=0;input.value=OS_INPUT_VALUE_RELEASE;
  window_control_route_locked(&window,&input,0,0,true,false);
  unsigned activated=0;
  for(unsigned i=0;i<event_count;++i)if(events[i].action==OS_CONTROL_CLICK)++activated;
  assert(activated==(unsigned)preserve&&!window.control_key_target);
  unsigned delivered=event_count;window_control_route_locked(&window,&input,0,0,true,false);
  if(preserve)assert(event_count==delivered);
  window_unlock();request.count=request.text_size=0;
  assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 }
 /* 幂等判断不能跨越视口、样式、文字或所有权变化。 */
 window=(window_server_window_t){.owner=&owner,.width=640,.height=360};
 nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER};
 nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_TEXT,.width=100,.height=40,.text_length=2};
 request=(os_window_control_tree_t){.identifier=7,.count=2,.nodes=(uintptr_t)nodes,.text=(uintptr_t)text,.text_size=2};
 assert(window_server_control_tree(&owner,&request)==K_OK);
 for(unsigned change=0;change<4;++change){
  uint64_t before=request.revision;int damage=marked;
  assert(window_server_control_tree(&owner,&request)==K_OK&&request.revision==before&&marked==damage);
  if(change==0)++window.width;
  if(change==1)++window.height;
  if(change==2)++nodes[1].foreground;
  if(change==3)text[0]^=1;
  assert(window_server_control_tree(&owner,&request)==K_OK&&request.revision>before&&marked==damage+1);
  assert(window.controls->layout_width==window.width&&window.controls->layout_height==window.height);
 }
 /* 合成器已按新视口更新缓存，不能把旧提交误认作按新视口准备过的文档。 */
 old=window.controls;uint64_t cache_revision=request.revision;++window.width;
 assert(control_tree_layout_validated(old->nodes,old->count,
  control_tree_window_viewport(old->nodes,old->count,window.width,window.height,window.flags),old->boxes));
 old->layout_width=window.width;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 printf("PAINT_CACHE_VIEWPORT_OPTIMIZED reused_old=%u revision=%llu\n",window.controls==old,
  (unsigned long long)request.revision);
 assert(window.controls!=old&&request.revision>cache_revision);
 /* 旧帧留下不同或失效的绘制缓存，也不能改变相同提交的身份。 */
 old=window.controls;cache_revision=request.revision;
 old->layout_width=1;old->layout_height=1;old->layout_flags=~window.flags;old->layout_valid=false;
 assert(window_server_control_tree(&owner,&request)==K_OK&&window.controls==old&&request.revision==cache_revision);
 old=window.controls;int retained_live=live;transfer_owner=true;
 assert(window_server_control_tree(&owner,&request)==K_EPERM&&window.controls==old&&live==retained_live);
 transfer_owner=false;window.owner=&owner;
 request.count=request.text_size=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 /* 发布时复用候选几何；准备期间调整大小时必须查询新视口。 */
 window=(window_server_window_t){.owner=&owner,.width=320,.height=180};
 nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER};
 nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,
  .x=10,.width=OS_CONTROL_FILL,.height=40};
 request=(os_window_control_tree_t){.identifier=7,.count=2,.nodes=(uintptr_t)nodes};
 g_window_server.pointer_x=501;g_window_server.pointer_y=61;
 locate_walks=0;resize_on_copy=true;
 assert(window_server_control_tree(&owner,&request)==K_OK&&window.width==640);
 assert(window.controls->layout_width==320&&window.control_hover==2&&locate_walks==1);
 assert(window.control_hover_damage.x1==640);
 request.count=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 /* 文本容器拖选会立即重绘；重发同一根不能取消释放事件所需的捕获。 */
 window=(window_server_window_t){.owner=&owner,.width=320,.height=180};
 nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,
  .kind=OS_CONTROL_CONTAINER};
 nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,
  .x=20,.y=20,.width=50,.height=30};
 request=(os_window_control_tree_t){.identifier=7,.count=2,.nodes=(uintptr_t)nodes};
 assert(window_server_control_tree(&owner,&request)==K_OK);
 window_lock();
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,
  .value=OS_INPUT_VALUE_PRESS};
 assert(window_control_route_locked(&window,&input,10,10,true,false));window_unlock();
 assert(window.control_pressed==1&&window.client_button_capture);
 ++nodes[0].background;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 assert(window.control_pressed==1&&window.client_button_capture);
 /* 捕获影响拖动目标，但不改变真实悬停和滚轮目标。 */
 event_count=0;window_lock();
 input=(os_input_event_t){.type=OS_INPUT_EVENT_RELATIVE,.code=OS_INPUT_REL_X,.value=1};
 assert(window_control_route_locked(&window,&input,25,25,true,false));
 assert(window.control_hover==2&&events[event_count-1].action==OS_CONTROL_MOVE&&events[event_count-1].target==1);
 input.code=OS_INPUT_REL_WHEEL;
 assert(window_control_route_locked(&window,&input,25,25,true,false));
 assert(events[event_count-1].action==OS_CONTROL_WHEEL&&events[event_count-1].target==2);
 input.code=OS_INPUT_REL_Y;
 assert(window_control_route_locked(&window,&input,-10,-10,true,false));
 assert(!window.control_hover&&events[event_count-1].action==OS_CONTROL_MOVE&&events[event_count-1].target==1);
 window_unlock();
 event_count=0;window_lock();
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,
  .value=OS_INPUT_VALUE_RELEASE};
 assert(window_control_route_locked(&window,&input,-10,-10,true,false));window_unlock();
 assert(event_count&&events[event_count-1].action==OS_CONTROL_UP&&events[event_count-1].target==1);
 assert(!window.control_pressed&&!window.client_button_capture);
 window_lock();
 input=(os_input_event_t){.type=OS_INPUT_EVENT_RELATIVE,.code=OS_INPUT_REL_X,.value=1};
 assert(window_control_route_locked(&window,&input,25,25,true,false));window_unlock();
 assert(events[event_count-1].action==OS_CONTROL_MOVE&&events[event_count-1].target==2);
 request.count=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 /* 4096 节点的真实事件路径：不能为普通键、非滑块和已命中的激活重复遍历。 */
 window=(window_server_window_t){.owner=&owner,.width=640,.height=360};
 os_control_node_t *cost_nodes=calloc(OS_CONTROL_TREE_MAX_NODES,sizeof(*cost_nodes));assert(cost_nodes);
 for(unsigned i=0;i<OS_CONTROL_TREE_MAX_NODES;++i)cost_nodes[i]=(os_control_node_t){
  .identifier=UINT32_MAX-i*104729U,.parent=i?0:OS_CONTROL_NO_PARENT,
  .kind=i?OS_CONTROL_BUTTON:OS_CONTROL_CONTAINER,.width=10,.height=10};
 cost_nodes[0].flags=OS_CONTROL_MANAGED_FOCUS;
 cost_nodes[OS_CONTROL_TREE_MAX_NODES-1].flags=OS_CONTROL_DEFAULT_FOCUS;
 request=(os_window_control_tree_t){.identifier=7,.count=OS_CONTROL_TREE_MAX_NODES,.nodes=(uintptr_t)cost_nodes};
 g_window_server.pointer_x=6;g_window_server.pointer_y=46;
 locked_node_reads=locate_walks=0;
 assert(window_server_control_tree(&owner,&request)==K_OK&&locked_node_reads<128&&!locate_walks);
 uint32_t cost_target=cost_nodes[OS_CONTROL_TREE_MAX_NODES-1].identifier;
 size_t duplicate_bytes=allocated_bytes;unsigned duplicate_calls=allocations;
 /* 不需要的第三次分配即使会失败，也不能妨碍相同文档刷新。 */
 fail_alloc=allocations+3;
 assert(window_server_control_tree(&owner,&request)==K_OK);fail_alloc=0;
 printf("DUPLICATE_OPTIMIZED calls=%u bytes=%zu layout_bytes=%zu\n",(unsigned)allocations-duplicate_calls,
  allocated_bytes-duplicate_bytes,(size_t)request.count*sizeof(control_tree_box_t));
 assert((unsigned)allocations==duplicate_calls+2U);
 assert(allocated_bytes-duplicate_bytes==sizeof(window_control_document_t)+
  (size_t)request.count*(sizeof(os_control_node_t)+sizeof(uint16_t)));
 /* 非法节点也在几何分配之前被拒绝，并完整保留当前快照。 */
 uint32_t final_identifier=cost_nodes[request.count-1].identifier;
 cost_nodes[request.count-1].identifier=cost_nodes[0].identifier;
 duplicate_calls=allocations;duplicate_bytes=allocated_bytes;
 window_control_document_t *before_invalid=window.controls;uint64_t before_revision=request.revision;
 assert(window_server_control_tree(&owner,&request)==K_EINVAL&&window.controls==before_invalid);
 assert(request.revision==before_revision&&(unsigned)allocations==duplicate_calls+2U);
 assert(allocated_bytes-duplicate_bytes==sizeof(window_control_document_t)+
  (size_t)request.count*(sizeof(os_control_node_t)+sizeof(uint16_t)));
 cost_nodes[request.count-1].identifier=final_identifier;


 assert(window.control_focus==cost_target&&window.control_hover==cost_target);
 /* 保留焦点和捕获按压只查稳定 ID，不能在发布锁内扫描新旧节点表。 */
 window.control_pressed=cost_target;window.client_button_capture=true;
 ++cost_nodes[1].foreground;locked_node_reads=locate_walks=0;
 assert(window_server_control_tree(&owner,&request)==K_OK&&locked_node_reads<128&&!locate_walks);
 assert(window.control_focus==cost_target&&window.control_pressed==cost_target&&window.client_button_capture);
 printf("Control publish work: 4096 nodes, locked state reads=%u, hover relayouts=%u; resize race passed\n",
  locked_node_reads,locate_walks);
 window.control_pressed=0;window.client_button_capture=false;
 window.control_focus=cost_target;window_lock();
 event_count=find_walks=locate_walks=0;
 input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=4,.value=OS_INPUT_VALUE_PRESS};
 assert(window_control_route_locked(&window,&input,0,0,true,false));
 assert(find_walks==0&&locate_walks==0&&events[event_count-1].action==OS_CONTROL_KEY);
 /* 普通输入只查询一次事件元数据，不额外查焦点；未配对的释放也不能遍历控件树。 */
 for(unsigned sample=0;sample<1024;++sample) {
  event_count=find_walks=locate_walks=target_queries=0;
  input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=4+sample%26,
   .value=sample%3};
  assert(window_control_route_locked(&window,&input,0,0,true,false));
  assert(target_queries==1U&&!find_walks&&!locate_walks&&event_count==1);
 }
 for(unsigned code=0;code<3;++code) {
  const uint32_t codes[]={0x28,0x58,0x2c};
  event_count=find_walks=locate_walks=target_queries=0;
  input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=codes[code],.value=OS_INPUT_VALUE_RELEASE};
  assert(window_control_route_locked(&window,&input,0,0,true,false));
  assert(!target_queries&&!find_walks&&!locate_walks&&!event_count);
 }
 for(unsigned key=0;key<2;++key){
  event_count=find_walks=locate_walks=0;input.code=0x28;
  input.value=key?OS_INPUT_VALUE_RELEASE:OS_INPUT_VALUE_PRESS;
  assert(window_control_route_locked(&window,&input,0,0,true,false));
  assert(find_walks==1&&locate_walks==0);
  if(key)assert(events[event_count-1].action==OS_CONTROL_CLICK&&events[event_count-1].target==cost_target);
  else {
   assert(window.control_key_target==cost_target);
   for(unsigned sample=0;sample<3;++sample) {
    os_input_event_t ignored={.type=OS_INPUT_EVENT_KEY,
     .code=sample==2?0x2c:0x28,
     .value=sample==0?OS_INPUT_VALUE_PRESS:sample==1?OS_INPUT_VALUE_REPEAT:OS_INPUT_VALUE_RELEASE};
    event_count=find_walks=locate_walks=target_queries=0;
    assert(window_control_route_locked(&window,&ignored,0,0,true,false));
    assert(!target_queries&&!find_walks&&!locate_walks&&!event_count);
    assert(window.control_key_target==cost_target&&window.control_key_code==0x28);
   }
  }
 }
 puts("Keyboard work: 1024 plain keys with only one metadata query, unmatched releases/repeats/duplicate presses without layout PASS");
 for(unsigned button=0;button<2;++button){
  event_count=find_walks=locate_walks=0;
  input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,
   .value=button?OS_INPUT_VALUE_RELEASE:OS_INPUT_VALUE_PRESS};
  assert(window_control_route_locked(&window,&input,5,5,true,false));
  assert(find_walks==0&&locate_walks==1);
  if(button)assert(events[event_count-1].action==OS_CONTROL_CLICK&&events[event_count-1].target==cost_target);
  else assert(window.control_pressed==cost_target&&window.client_button_capture);
 }
 /* 一次焦点切换同时取消旧键盘按压，并恢复旧、新焦点外观。 */
 window.control_focus=cost_target;window.control_key_target=cost_nodes[1].identifier;
 window.control_key_code=0x28;event_count=find_walks=locate_walks=visit_walks=0;small_marks=0;
 control_set_focus(&window,&input,cost_nodes[2].identifier,5,5);
 printf("FOCUS_OPTIMIZED find_walks=%u shared_layouts=%u marks=%u\n",find_walks,visit_walks,small_marks);
 assert(!find_walks&&visit_walks==1&&small_marks==3&&event_count==2);
 assert(events[0].action==OS_CONTROL_BLUR&&events[0].target==cost_target);
 assert(events[1].action==OS_CONTROL_FOCUS&&events[1].target==cost_nodes[2].identifier);
 assert(!window.control_key_target&&!window.control_key_code);
 /* Tab 的目标选择和旧、新外观恢复必须共用一次布局。 */
 window.control_focus=cost_nodes[2].identifier;window.control_key_target=cost_nodes[1].identifier;
 window.control_key_code=0x28;event_count=focus_walks=visit_walks=find_walks=small_marks=0;
 input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=0x2b,.value=OS_INPUT_VALUE_PRESS};
 assert(window_control_route_locked(&window,&input,0,0,true,false));
 printf("TAB_LAYOUTS focus=%u damage=%u marks=%u\n",focus_walks,visit_walks,small_marks);
 assert(focus_walks+visit_walks==1&&small_marks==3&&event_count==2&&!find_walks);
 assert(window.control_focus==cost_nodes[3].identifier&&!window.control_key_target&&!window.control_key_code);
 assert(events[0].action==OS_CONTROL_BLUR&&events[0].target==cost_nodes[2].identifier);
 assert(events[1].action==OS_CONTROL_FOCUS&&events[1].target==cost_nodes[3].identifier);
 window.control_shift=1;event_count=focus_walks=visit_walks=small_marks=0;
 assert(window_control_route_locked(&window,&input,0,0,true,false));
 assert(window.control_focus==cost_nodes[2].identifier&&focus_walks==1&&!visit_walks&&small_marks==2);
 window.control_shift=0;window.control_key_target=cost_nodes[1].identifier;window.control_key_code=0x28;
 event_count=focus_walks=visit_walks=small_marks=0;fail_focus=true;
 assert(window_control_route_locked(&window,&input,0,0,true,false));
 assert(window.control_focus==cost_nodes[2].identifier&&window.control_key_target==cost_nodes[1].identifier&&window.control_key_code==0x28);
 assert(focus_walks==1&&!visit_walks&&!small_marks&&!event_count);fail_focus=false;
 input.value=OS_INPUT_VALUE_RELEASE;focus_walks=0;
 assert(window_control_route_locked(&window,&input,0,0,true,false)&&!focus_walks);
 /* 同一节点兼任键盘按压和旧焦点时，只恢复一次；无效目标不启动布局。 */
 window.control_key_target=window.control_focus;small_marks=visit_walks=find_walks=0;
 control_mark_focus_change_locked(&window,cost_target);
 assert(visit_walks==1&&!find_walks&&small_marks==2);
 /* 布局失败时不得发布已收集的半份范围；焦点事件和状态另由调用方维护。 */
 small_marks=visit_walks=0;fail_visit=true;
 control_mark_focus_change_locked(&window,cost_target);
 assert(visit_walks==1&&!small_marks);fail_visit=false;
 window.control_key_target=2;window.control_focus=3;small_marks=visit_walks=0;
 control_mark_focus_change_locked(&window,4);
 assert(!visit_walks&&!small_marks);
 window.control_key_target=0;
 /* 不存在的焦点不能为确定无效的目标再走一次布局。 */
 window.control_focus=2;find_walks=locate_walks=0;
 input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=0x28,.value=OS_INPUT_VALUE_PRESS};
 assert(window_control_route_locked(&window,&input,0,0,true,false)&&!find_walks&&!locate_walks);
 window_unlock();request.count=0;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!live);free(cost_nodes);
 /* 自绘终端文字、空标签和符号不能触发通用文字命中的第二次整树布局。 */
 {
 os_control_node_t *plain=calloc(OS_CONTROL_TREE_MAX_NODES,sizeof(*plain));assert(plain);
 for(unsigned sample=0;sample<4;++sample) {
  window=(window_server_window_t){.owner=&owner,.width=640,.height=360};
  for(unsigned i=0;i<OS_CONTROL_TREE_MAX_NODES;++i)plain[i]=(os_control_node_t){
   .identifier=i+1U,.parent=i?0:OS_CONTROL_NO_PARENT,.kind=i?OS_CONTROL_TEXT:OS_CONTROL_CONTAINER,
   .flags=i?OS_CONTROL_POINTER_PASSTHROUGH:0,.width=i?100:640,.height=i?40:360,.font_size=16,
   .text_length=i?1U:0U,.value=i?OS_CONTROL_TEXT_APP_SELECTION:0};
  if(sample==1 || sample==2)for(unsigned i=1;i<OS_CONTROL_TREE_MAX_NODES;++i) {
   plain[i].kind=OS_CONTROL_BUTTON;plain[i].flags=sample==2?OS_CONTROL_SYMBOL:0;
   plain[i].value=sample==2?OS_CONTROL_SYMBOL_BACK:0;plain[i].text_length=0U;
  }
  if(sample==3)plain[OS_CONTROL_TREE_MAX_NODES-1].value=0;
  request=(os_window_control_tree_t){.identifier=7,.count=OS_CONTROL_TREE_MAX_NODES,
   .nodes=(uintptr_t)plain,.text=(uintptr_t)"a",.text_size=1};
  assert(window_server_control_tree(&owner,&request)==K_OK);
  window_lock();event_count=visit_walks=locate_walks=0;
  input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
  assert(window_control_route_locked(&window,&input,5,5,true,false));
  printf("TEXT_ELIGIBILITY sample=%u visits=%u locate=%u\n",sample,visit_walks,locate_walks);
  assert(visit_walks==(sample==3?1U:0U) && locate_walks==(sample==3?0U:1U));
  input.value=OS_INPUT_VALUE_RELEASE;
  assert(window_control_route_locked(&window,&input,5,5,true,false));
  window_unlock();request.count=0;request.text_size=0;
  assert(window_server_control_tree(&owner,&request)==K_OK && !live);
 }
 free(plain);
 }
 /* 重绘被遮挡窗口不能凭客户区局部坐标重新激活悬停；离开保留真实事件时间戳。 */
 window=(window_server_window_t){.owner=&owner,.width=640,.height=360};
 nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER};
 nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,.width=100,.height=40};
 request=(os_window_control_tree_t){.identifier=7,.count=2,.nodes=(uintptr_t)nodes};
 g_window_server.pointer_x=21;g_window_server.pointer_y=61;pointer_over_window=false;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!window.control_hover);
 pointer_over_window=true;nodes[0].background=1;
 assert(window_server_control_tree(&owner,&request)==K_OK&&window.control_hover==2);
 event_count=0;input=(os_input_event_t){.timestamp=12345,.type=OS_INPUT_EVENT_RELATIVE};
 window_lock();window_control_pointer_hover_locked(&window,&input,false);window_unlock();
 assert(!window.control_hover&&event_count==1&&events[0].action==OS_CONTROL_LEAVE&&events[0].timestamp==12345);
 request.count=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);

 /* 真实输入路由：透明标签可拖选，UTF-8 边界复制，按钮单击仍可激活。 */
 window=(window_server_window_t){.owner=&owner,.width=640,.height=360};
 nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER};
 nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_TEXT,
  .flags=OS_CONTROL_POINTER_PASSTHROUGH,
  .x=10,.y=10,.width=160,.height=40,.font_size=20,.text_length=5};
 const char selected_utf8[]="A中B";
 request=(os_window_control_tree_t){.identifier=7,.count=2,.nodes=(uintptr_t)nodes,
  .text=(uintptr_t)selected_utf8,.text_size=5};
 assert(window_server_control_tree(&owner,&request)==K_OK);
 window_lock();event_count=0;
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
 assert(window_control_route_locked(&window,&input,11,11,true,false));
 assert(window.control_text_drag && window.control_text_anchor==0 && window.client_button_capture);
 input=(os_input_event_t){.type=OS_INPUT_EVENT_RELATIVE};target_queries=0;
 assert(window_control_route_locked(&window,&input,39,11,true,false));
 assert(target_queries==1U);
 assert(window.control_text_end==4);
 assert(last_damage.x0==10 && last_damage.x1==40 && last_damage.y0==10 && last_damage.y1==30);
 /* 反复跨过中英文字节边界，拖动每次只定位一次目标，含选区回退到空的情况。 */
 for(unsigned sample=0;sample<1024;++sample) {
  const int32_t positions[]={11,21,49,39};
  const uint32_t offsets[]={0,1,5,4};
  target_queries=0;
  assert(window_control_route_locked(&window,&input,positions[sample%4],11,true,false));
  assert(target_queries==1U && window.control_text_end==offsets[sample%4]);
 }
 puts("Selection motion: 1024 forward/reverse UTF-8 drags, one target query per move PASS");
 /* 独立字节/像素边界表验证正向、反向、清空及跨起点的损坏矩形。 */
 const uint32_t byte_edges[]={0,1,4,5},pixel_edges[]={0,10,30,40};
 for(unsigned a=0;a<4;++a)for(unsigned b=0;b<4;++b) {
  unsigned previous_marks=small_marks;
  control_text_damage_selection(&window,1U,byte_edges[a],byte_edges[b]);
  if(a==b)assert(small_marks==previous_marks);
  else {
   unsigned left=a<b?a:b,right=a<b?b:a;
   assert(small_marks==previous_marks+1);
   assert(last_damage.x0==10+(int32_t)pixel_edges[left] && last_damage.x1==10+(int32_t)pixel_edges[right]);
   assert(last_damage.y0==10&&last_damage.y1==30);
  }
 }

 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_RELEASE};
 assert(window_control_route_locked(&window,&input,51,11,true,false));
 assert(!window.control_text_drag && !window.client_button_capture && window.control_text_end==5);
 window_unlock();nodes[0].background++;
 assert(window_server_control_tree(&owner,&request)==K_OK);
 assert(window.control_text_target==2 && window.control_text_anchor==0 && window.control_text_end==5);
 window_lock();event_count=0;
 input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=0xe0,.value=OS_INPUT_VALUE_PRESS};
 assert(window_control_route_locked(&window,&input,51,11,true,false));
 input.code=0x06;assert(window_control_route_locked(&window,&input,51,11,true,false));
 g_window_server.focused_identifier=7;
 window_control_copy_request_t copy=window_control_copy_selection_locked(&input);
 assert(copy.document==window.controls && copy.size==5);
 window_unlock();window_control_copy_complete(&copy);window_lock();
 assert(!copy.document);
 assert(clipboard_size==5 && !memcmp(clipboard,selected_utf8,5));
 window_control_keyboard_blur_locked(&window);assert(!window.control_text_ctrl);
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
 assert(window_control_route_locked(&window,&input,11,11,true,false));
 input.value=OS_INPUT_VALUE_RELEASE;assert(window_control_route_locked(&window,&input,11,11,true,false));
 assert(!window.control_text_drag && !window.client_button_capture);
 window_unlock();
 /* 更换文本必须取消旧字节索引；旧选区不能复制新文档的同一位置。 */
 nodes[1].kind=OS_CONTROL_BUTTON;nodes[1].flags=0;nodes[1].value=0;
 nodes[1].x=0;nodes[1].y=0;nodes[1].width=100;nodes[1].height=40;nodes[1].text_length=4;
 request.text=(uintptr_t)"Open";request.text_size=4;
 assert(window_server_control_tree(&owner,&request)==K_OK && !window.control_text_target);
 window_lock();event_count=0;
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
 assert(window_control_route_locked(&window,&input,31,11,true,false));
 input.value=OS_INPUT_VALUE_RELEASE;assert(window_control_route_locked(&window,&input,31,11,true,false));
 unsigned clicks=0;for(unsigned i=0;i<event_count;++i)clicks+=events[i].action==OS_CONTROL_CLICK;
 assert(clicks==1);
 event_count=0;input.value=OS_INPUT_VALUE_PRESS;
 assert(window_control_route_locked(&window,&input,31,11,true,false));
 input=(os_input_event_t){.type=OS_INPUT_EVENT_RELATIVE};
 assert(window_control_route_locked(&window,&input,69,11,true,false));
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_RELEASE};
 assert(window_control_route_locked(&window,&input,69,11,true,false));
 assert(!window.control_pressed && !window.client_button_capture);
 for(unsigned i=0;i<event_count;++i)assert(events[i].action!=OS_CONTROL_CLICK);
 /* 拖选后回到起点也不能重新变成单击；下一次真实单击仍可激活。 */
 event_count=0;
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
 assert(window_control_route_locked(&window,&input,31,11,true,false));
 input=(os_input_event_t){.type=OS_INPUT_EVENT_RELATIVE};
 assert(window_control_route_locked(&window,&input,69,11,true,false));
 assert(window_control_route_locked(&window,&input,31,11,true,false));
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_RELEASE};
 assert(window_control_route_locked(&window,&input,31,11,true,false));
 assert(!window.control_pressed && !window.client_button_capture);
 for(unsigned i=0;i<event_count;++i)assert(events[i].action!=OS_CONTROL_CLICK);
 event_count=0;input.value=OS_INPUT_VALUE_PRESS;
 assert(window_control_route_locked(&window,&input,31,11,true,false));
 input.value=OS_INPUT_VALUE_RELEASE;
 assert(window_control_route_locked(&window,&input,31,11,true,false));
 clicks=0;for(unsigned i=0;i<event_count;++i)clicks+=events[i].action==OS_CONTROL_CLICK;
 assert(clicks==1);
 window_unlock();request.count=0;request.text_size=0;
 assert(window_server_control_tree(&owner,&request)==K_OK && !live);
 /* 非客户区按下清理实际选区，仅损坏旧高亮；重复清理不再产生损坏。 */
 window=(window_server_window_t){.owner=&owner,.width=640,.height=360};
 nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER};
 nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,
  .width=120,.height=40,.font_size=20,.text_length=5};
 request=(os_window_control_tree_t){.identifier=7,.count=2,.nodes=(uintptr_t)nodes,
  .text=(uintptr_t)"A中B",.text_size=5};
 assert(window_server_control_tree(&owner,&request)==K_OK);
 window_lock();window.control_text_target=2;window.control_text_anchor=1;window.control_text_end=4;
 window.control_text_font=20;window.control_text_bounds=(Rect){0,0,40,20};
 window.control_text_drag=CONTROL_TEXT_DRAGGED;window.control_text_ctrl=1;
 unsigned cleared_marks=small_marks;
 window_control_clear_text_selection_locked(&window);
 assert(!window.control_text_target&&!window.control_text_anchor&&!window.control_text_end&&!window.control_text_drag);
 assert(window.control_text_ctrl==1&&small_marks==cleared_marks+1&&last_damage.x0==10&&last_damage.x1==30);
 window_control_clear_text_selection_locked(&window);assert(small_marks==cleared_marks+1);
 window_control_clear_text_selection_locked(0);
 window_unlock();request.count=request.text_size=0;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 puts("Selection clear: non-client action cancels target/drag, restores old highlight once and retains modifier state PASS");
 /* 大量重叠文字只为最终命中的一项计算字形位置，遮挡和穿透规则保持不变。 */
 os_control_node_t overlaps[128];
 for(unsigned top=0;top<5;++top) {
  window=(window_server_window_t){.owner=&owner,.width=640,.height=360};
  for(unsigned i=0;i<128;++i)overlaps[i]=(os_control_node_t){.identifier=i+1,
   .parent=i?0:OS_CONTROL_NO_PARENT,.kind=i?OS_CONTROL_TEXT:OS_CONTROL_CONTAINER,
   .width=120,.height=40,.font_size=20,.text_length=5};
  if(top==1)overlaps[127].flags=OS_CONTROL_DISABLED;
  if(top==2)overlaps[127].text_length=0;
  if(top==3){overlaps[127].text_length=0;overlaps[127].flags=OS_CONTROL_POINTER_PASSTHROUGH;}
  if(top==4){overlaps[127].kind=OS_CONTROL_CONTAINER;overlaps[127].flags=OS_CONTROL_INERT;}
  request=(os_window_control_tree_t){.identifier=7,.count=128,.nodes=(uintptr_t)overlaps,
   .text=(uintptr_t)"A中B",.text_size=5};
  assert(window_server_control_tree(&owner,&request)==K_OK);
  window_lock();text_bounds_calls=0;
  input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
  window_control_route_locked(&window,&input,1,1,true,false);
  bool selectable=top!=2;
  assert(text_bounds_calls==(unsigned)selectable);
  assert(window.control_text_target==(selectable?(top>=3?127U:128U):0U));
  window_unlock();request.count=request.text_size=0;
  assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 }
 puts("Selection hit: 128 overlapping labels, one final bounds calculation, disabled/opaque/passthrough/inert semantics PASS");
 /* 每一种带文字的控件默认可选，禁用文字仍可复制，留白起拖不得激活动作。 */
 for(unsigned kind=OS_CONTROL_CONTAINER;kind<=OS_CONTROL_SEPARATOR;++kind)for(unsigned disabled=0;disabled<2;++disabled) {
  window=(window_server_window_t){.owner=&owner,.width=640,.height=360};
  nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER};
  nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=kind,.surface=1,
   .flags=disabled?OS_CONTROL_DISABLED:0,.width=120,.height=40,.font_size=20,.text_length=5};
  request=(os_window_control_tree_t){.identifier=7,.count=2,.nodes=(uintptr_t)nodes,
   .text=(uintptr_t)"A中B",.text_size=5};
  assert(window_server_control_tree(&owner,&request)==K_OK);
  window_lock();event_count=0;
  input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
  window_control_route_locked(&window,&input,1,1,true,false);
  assert(window.control_text_target==2 && window.control_text_anchor==0 && window.client_button_capture);
  input=(os_input_event_t){.type=OS_INPUT_EVENT_RELATIVE};
  assert(window_control_route_locked(&window,&input,160,1,false,false));
  input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_RELEASE};
  assert(window_control_route_locked(&window,&input,160,1,false,false));
  assert(window.control_text_end==5 && !window.client_button_capture);
  for(unsigned i=0;i<event_count;++i)assert(events[i].action!=OS_CONTROL_CLICK);
  input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=0xe0,.value=OS_INPUT_VALUE_PRESS};
  window_control_route_locked(&window,&input,0,0,true,false);input.code=6;
  assert(window_control_route_locked(&window,&input,0,0,true,false));
  g_window_server.focused_identifier=7;
  window_control_copy_request_t copy=window_control_copy_selection_locked(&input);
  assert(copy.document==window.controls && copy.size==5);
  window_unlock();window_control_copy_complete(&copy);window_lock();
  assert(clipboard_size==5&&!memcmp(clipboard,"A中B",5));
  window_unlock();request.count=request.text_size=0;
  assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 }
 /* 解锁后即使窗口清空文档，选区快照仍持有引用；发布失败也必须释放。 */
 for(unsigned fail=0;fail<2;++fail) {
  window=(window_server_window_t){.owner=&owner,.width=640,.height=360};
  nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER};
  nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_TEXT,
   .width=120,.height=40,.font_size=20,.text_offset=4,.text_length=5};
  request=(os_window_control_tree_t){.identifier=7,.count=2,.nodes=(uintptr_t)nodes,
   .text=(uintptr_t)"old:A中B",.text_size=9};
  assert(window_server_control_tree(&owner,&request)==K_OK);
  window_lock();window.control_text_target=2;window.control_text_ctrl=1;
  window.control_text_anchor=4;window.control_text_end=1;
  g_window_server.focused_identifier=7;
  input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,.code=6,.value=OS_INPUT_VALUE_RELEASE};
  assert(!window_control_copy_selection_locked(&input).document);
  input.value=OS_INPUT_VALUE_PRESS;
  window_control_copy_request_t copy=window_control_copy_selection_locked(&input);
  assert(copy.document==window.controls && copy.first==5 && copy.size==3);
  window_unlock();request.count=request.text_size=0;
  assert(window_server_control_tree(&owner,&request)==K_OK && live);
  memcpy(clipboard,"prior",5);clipboard_size=5;clipboard_fail=fail;
  window_control_copy_complete(&copy);assert(!copy.document && !live);
  window_control_copy_complete(&copy);assert(!live);
  if(fail)assert(clipboard_size==5&&!memcmp(clipboard,"prior",5));
  else assert(clipboard_size==3&&!memcmp(clipboard,"中",3));
 }
 /* 左右放置都必须使用真实指针状态机，拖离、禁用和越界不能激活。 */
 for(unsigned left=0;left<2;++left) for(unsigned action=1;action<=3;++action) {
  window=(window_server_window_t){.owner=&owner,.width=640,.height=360,
   .flags=OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_CAPTION_HEIGHT(40)};
  nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
   .flags=OS_CONTROL_WINDOW_FRAME|OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_POINTER_PASSTHROUGH,
   .width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL};
  nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_WINDOW_BUTTON,
   .x=left?0:608,.width=32,.height=40,.value=action};
  request=(os_window_control_tree_t){.identifier=7,.count=2,.nodes=(uintptr_t)nodes};
  assert(window_server_control_tree(&owner,&request)==K_OK);
  assert(window_control_has_caption(window.controls));
  int x=nodes[1].x+16,y=-20;
  assert(window_control_caption_hit_locked(&window,x+window_client_offset_x(window.flags),
   y+window_client_offset_y(window.flags)));
  window_lock();event_count=caption_actions=0;
  input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
  assert(window_control_route_locked(&window,&input,x,y,true,false));assert(!caption_actions);
  input.value=OS_INPUT_VALUE_RELEASE;
  assert(window_control_route_locked(&window,&input,x,y,true,false));assert(caption_actions==action);
  caption_actions=0;input.value=OS_INPUT_VALUE_PRESS;
  assert(window_control_route_locked(&window,&input,x,y,true,false));
  input.value=OS_INPUT_VALUE_RELEASE;(void)window_control_route_locked(&window,&input,300,100,true,false);
  assert(!caption_actions);window_unlock();
  nodes[1].flags=OS_CONTROL_DISABLED;assert(window_server_control_tree(&owner,&request)==K_OK);
  window_lock();input.value=OS_INPUT_VALUE_PRESS;(void)window_control_route_locked(&window,&input,x,y,true,false);
  input.value=OS_INPUT_VALUE_RELEASE;(void)window_control_route_locked(&window,&input,x,y,true,false);
  assert(!caption_actions);window_unlock();
  request.count=0;assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 }
 /* 带标题栏的客户区透明菜单遮罩必须收到真实按下事件；标题空白仍可拖动。 */
 window=(window_server_window_t){.owner=&owner,.width=640,.height=360,
  .flags=OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_CAPTION_HEIGHT(40)};
 nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
  .flags=OS_CONTROL_WINDOW_FRAME|OS_CONTROL_POINTER_PASSTHROUGH,.width=-1,.height=-1};
 nodes[1]=(os_control_node_t){.identifier=77,.parent=0,.kind=OS_CONTROL_CONTAINER,
  .flags=OS_CONTROL_BACKGROUND_ALPHA,.y=40,.width=-1,.height=-1};
 request=(os_window_control_tree_t){.identifier=7,.count=2,.nodes=(uintptr_t)nodes};
 assert(window_server_control_tree(&owner,&request)==K_OK);
 window_lock();event_count=0;
 input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS};
 assert(window_control_route_locked(&window,&input,100,100,true,false));
 assert(event_count && events[event_count-1].target==77 && events[event_count-1].action==OS_CONTROL_DOWN);
 input.value=OS_INPUT_VALUE_RELEASE;
 assert(window_control_route_locked(&window,&input,100,100,true,false));
 assert(!window.client_button_capture);
 event_count=0;input.value=OS_INPUT_VALUE_PRESS;
 assert(!window_control_route_locked(&window,&input,100,-20,true,false));
 window_unlock();request.count=0;
 assert(window_server_control_tree(&owner,&request)==K_OK&&!live);
 puts("Caption actions: left/right, all operations, release/cancel and disabled routing PASS");
 /* 桌面原生弹层与应用 Unicode 文档使用同一套字形缓存，悬停和移动不重建。 */
 os_control_node_t overlay_nodes[2]={
  {.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER},
  {.identifier=2,.parent=0,.kind=OS_CONTROL_TEXT,.text_length=6}};
 control_tree_box_t overlay_boxes[2]={0};overlay_boxes[1].style.font_size=16;
 window_control_document_t *overlay_doc=window_control_prepare_overlay(0,overlay_nodes,2,"打开",6,overlay_boxes);
 assert(overlay_doc && overlay_doc->runs && overlay_doc->runs[1]);
 assert(overlay_doc->runs[1]->count==2 && overlay_doc->runs[1]->width==32);
 assert(overlay_doc->runs[1]->glyphs[overlay_doc->runs[1]->cells[0].glyph].code==0x6253U);
 unsigned before_overlay=glyph_builds;
 overlay_nodes[1].x=100;overlay_nodes[1].flags=OS_CONTROL_CHECKED;
 window_control_document_t *same_overlay=window_control_prepare_overlay(overlay_doc,overlay_nodes,2,"打开",6,overlay_boxes);
 assert(same_overlay==overlay_doc && glyph_builds==before_overlay);
 window_control_document_put(same_overlay);
 fail_font=true;
 assert(!window_control_prepare_overlay(overlay_doc,overlay_nodes,2,"关闭",6,overlay_boxes));
 assert(overlay_doc->runs[1]->width==32);fail_font=false;
 overlay_nodes[1].text_length=7;
 assert(!window_control_prepare_overlay(overlay_doc,overlay_nodes,2,"打开",6,overlay_boxes));
 window_control_document_put(overlay_doc);assert(!live);
 puts("Desktop overlay text: Unicode glyph identity, shared cache, motion/hover reuse, malformed ranges and failure cleanup PASS");
 clipboard_fail=false;
 puts("Clipboard copy: unlocked publication, replaced document lifetime, reversed UTF-8 offsets and failed publication cleanup PASS");
 puts("All control kinds: default text selection, disabled copy, padding drag, outside release and no accidental click PASS");
 puts("Control selection: UTF-8 drag/copy, release/blur cleanup, document invalidation, click versus drag PASS");
 puts("Control input work: 4096 nodes, text key=0 layouts, activation key=1 find, pointer button=1 locate and 0 finds PASS");
 puts("Control document: idempotent snapshots/input preservation, geometry/style/text/ownership invalidation, rollback/lifetime, common widgets and stale-event isolation PASS");
 assert(!revalidations);
 puts("Control document: one indexed validation per submission, uint16 targets, 4096 metadata lookups, duplicate rollback PASS");
}
'''
def instrument_nodes(code):
    code = code.replace("uint32_t target) {\n    if (!document || !target)",
        "uint32_t target) {\n    ++target_queries;\n    if (!document || !target)")
    code = code.replace('uint32_t index,const control_tree_box_t *box,uint32_t font) {',
        'uint32_t index,const control_tree_box_t *box,uint32_t font) {\n ++text_bounds_calls;')
    # 统计真实发布锁内的节点表读取，候选准备阶段的线性遍历不计入临界区。
    code = re.sub(r'(document|previous)->nodes\[([^\[\]]+)\]',
                  r'(*counted_document_node(\1,\2))', code)
    code = code.replace('const os_control_node_t *node=document->nodes+index;',
                        'const os_control_node_t *node=counted_document_node(document,index);')
    helper = '''static const os_control_node_t *counted_document_node(
 const window_control_document_t *document,uint32_t index){
 if(locked)++locked_node_reads;
 return document->nodes+index;
}
'''
    marker = '/* 事件生产复用提交校验'
    assert marker in code
    return code.replace(marker, helper + marker, 1)


with tempfile.TemporaryDirectory(prefix='control-document-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + instrument_nodes(body) + main)
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root), '-I' + str(root / 'include'), str(path / 'test.c'),
                    str(root / 'kernel/graphics/control_tree.c'), '-o', str(path / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'test')], check=True)
    validated = 'control_tree_layout_validated(document->nodes, document->count,'
    assert validated in body
    negative = body.replace(validated,
                            'control_tree_layout(document->nodes, document->count, document->text_size,')
    (path / 'test.c').write_text(prefix + instrument_nodes(negative) + main)
    subprocess.run(command, check=True)
    result = subprocess.run([str(path / 'test')], capture_output=True, timeout=30)
    assert result.returncode != 0 and b'revalidations' in result.stderr, result.stderr
    print('Negative control rejected: repeated validation during document layout')
    # 像素和事件可能仍然正确，但重复查询必须被真实事件成本约束拒绝。
    focus_start = body.index('        uint32_t focus=control_target_index(document,window->control_focus);')
    focus_end = body.index('        if (!next_focus)', focus_start)
    legacy_focus = '''        for(uint32_t i=0;i<document->count;++i)
            if(document->nodes[i].identifier==window->control_focus &&
               document->boxes[i].visible && document->boxes[i].enabled &&
               (!(document->nodes[0].flags & OS_CONTROL_MANAGED_FOCUS) ||
                (os_control_is_interactive(document->nodes[i].kind) &&
                 !rect_is_empty(control_button_damage(document->nodes+i,document->boxes+i)))))
                next_focus=window->control_focus;
'''
    negatives = {
        'ordinary key queries focus target': body.replace(
            'uint32_t focused=range_key?control_target_index(document,window->control_focus):OS_CONTROL_NO_PARENT;',
            'uint32_t focused=control_target_index(document,window->control_focus);'),
        'ignored activation key queries layout': body.replace(
            '    /* 重复按下、自动重复和未配对的释放不改变按钮状态，不为它们重新查询布局。 */',
            '    control_tree_box_t redundant; (void)control_find_button(window,window->control_focus,&redundant);\n'
            '    /* 重复按下、自动重复和未配对的释放不改变按钮状态，不为它们重新查询布局。 */'),
        'selection repeats resolved target lookup': body.replace(
            'control_text_damage_selection(window,index,window->control_text_end,end);',
            'control_text_damage_selection(window,control_target_index(document,window->control_text_target),window->control_text_end,end);'),
        'repeated text pointer layout': body.replace(
            'query && query->valid ? query->index : control_tree_locate',
            'false ? query->index : control_tree_locate'),
        'repeated navigation damage layout': body.replace('    if (identifier == window->control_focus) return;',
            '    if (identifier == window->control_focus) return;\n    control_mark_focus_change_locked(window, identifier);'),
        'cancel uses whole-window damage': body.replace('    control_damage_identifier(window, pressed);',
            '    window_mark_window_locked(window);'),
        'text-key layout': body.replace('control_tree_box_t focus_box;',
            'control_tree_box_t focus_box; (void)control_find_button(window,window->control_focus,&focus_box);'),
        'non-range pointer layout': body.replace(
            '    uint32_t candidate=control_target_index(window->controls,target);',
            '    uint32_t candidate=control_target_index(window->controls,target);\n'
            '    control_tree_box_t redundant; (void)control_find_button(window,target,&redundant);'),
        'range ignores resized viewport': body.replace(
            'window->control_range_width == window->width && window->control_range_height == window->height',
            'true'),
        'submission reads mutable paint viewport': body.replace('font_source->prepared_width==window->width && font_source->prepared_height==window->height &&\n'
            '                font_source->prepared_flags==window->flags',
            'font_source->layout_width==window->width && font_source->layout_height==window->height &&\n'
            '                font_source->layout_flags==window->flags'),
        'text cache follows array index': body.replace('uint32_t old_index=control_target_index(previous,node->identifier);',
            'uint32_t old_index=previous && index<previous->count ? index : OS_CONTROL_NO_PARENT;'),
        'repeated focus damage layouts': body.replace('    control_mark_focus_change_locked(window, identifier);',
            '    control_mark_focus_change_locked(window, identifier);\n    control_damage_identifier(window,window->control_focus);'),
        'locked focus scan': body[:focus_start] + legacy_focus + body[focus_end:],
        'stale candidate geometry': body.replace(
            'document->layout_width==window->width && document->layout_height==window->height &&\n'
            '                document->layout_flags==window->flags', 'true'),
    }
    for label, negative in negatives.items():
        assert negative != body
        (path / 'test.c').write_text(prefix + instrument_nodes(negative) + main)
        subprocess.run(command, check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True, timeout=30)
        assert result.returncode != 0 and any(message in result.stderr for message in
            (b'target_queries', b'locate_walks', b'find_walks', b'locked_node_reads', b'window.control_hover==2', b'moved_run_reused', b'window.controls!=old', b'marked==cancel_full', b'focus_walks+visit_walks')), (label, result.stderr)
        print('Negative control rejected:', label)
