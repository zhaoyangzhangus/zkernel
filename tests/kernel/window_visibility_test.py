"""Run the production visibility helper against ownership/first-frame races."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/window.c').read_text()
start = source.index('static kstatus_t window_server_set_internal(')
end = source.index('\nkstatus_t window_server_set(', start)
helper = source[start:end]
router = (root / 'kernel/graphics/input_router.c').read_text()
start = router.index('void window_input_release_pointer_locked(')
end = router.index('\n}', start) + 2
pointer_release = router[start:end]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "kernel/rect.h"
typedef int kstatus_t;
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define OS_WINDOW_VISIBLE 1U
#define WINDOW_SERVER_SHOW_INACTIVE 2U
typedef struct { int id; } process_t;
typedef struct window {
 process_t *owner; uint32_t identifier, flags, width, height;
 int32_t x,y; bool client_hidden, client_show_inactive, client_pointer_capture, client_button_capture; uint64_t present_sequence;
 uint32_t control_hover,control_pressed,control_text_ctrl;bool control_text_drag;Rect control_hover_damage;
} window_server_window_t;
static struct { uint32_t focused_identifier;struct {uint32_t count;window_server_window_t *children[4];} scene; } g_window_server;
static unsigned rehits;
static window_server_window_t *expect_leave;
static void window_input_refresh_pointer_locked(void) {
 ++rehits;
 if(expect_leave) {assert(expect_leave->control_hover && expect_leave->client_pointer_capture);expect_leave=0;}
}
static void window_lock(void) {}
static void window_unlock(void) {}
static bool window_server_owner_locked(process_t *p,window_server_window_t *w) {return p==w->owner;}
static window_server_window_t *window_scene_find_locked(uint32_t id) {(void)id;return 0;}
static uint32_t window_outer_width(uint32_t v,uint32_t flags) {(void)flags;return v;}
static uint32_t window_outer_height(uint32_t v,uint32_t flags) {(void)flags;return v;}
static void window_mark_moved_rect_locked(int32_t a,int32_t b,int32_t c,int32_t d,uint32_t e,uint32_t f,bool g) {(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;}
static void window_scene_set_focus_identifier_locked(uint32_t id) {g_window_server.focused_identifier=id;}
static void window_mark_window_locked(window_server_window_t *w) {(void)w;}
static void window_server_notify_worker(void) {}
'''
program += pointer_release + '\n' + helper
program += r'''
int main(void) {
 process_t owner={1},other={2};
 window_server_window_t w={.owner=&owner,.identifier=3,.x=10,.y=20};
 g_window_server.scene.count=1;g_window_server.scene.children[0]=&w;
 assert(window_server_set_internal(&other,&w,0,0,0,false,true)==K_EPERM);
 assert(window_server_set_internal(&owner,&w,0,0,1,false,true)==0);
 assert(!(w.flags&OS_WINDOW_VISIBLE) && !w.client_hidden);
 w.client_pointer_capture=true;
 w.client_button_capture=true;w.control_hover=7;w.control_pressed=7;
 w.control_hover_damage=(Rect){1,2,30,40};
 assert(window_server_set_internal(&owner,&w,0,0,1,false,true)==0);
 assert(rehits==0);
 assert(w.client_pointer_capture && !(w.flags&OS_WINDOW_VISIBLE));
 assert(w.client_button_capture && w.control_hover==7 && w.control_pressed==7 && w.control_hover_damage.x1==30);
 assert(window_server_set_internal(&other,&w,0,0,0,false,true)==K_EPERM);
 assert(w.client_pointer_capture && w.client_button_capture && w.control_pressed==7);
 expect_leave=&w;
 assert(window_server_set_internal(&owner,&w,0,0,0,false,true)==0);
 assert(!expect_leave && rehits==1);
 assert(!w.client_pointer_capture && w.client_hidden);
 assert(!w.client_button_capture && !w.control_hover && !w.control_pressed && rect_is_empty(w.control_hover_damage));
 assert(window_server_set_internal(&owner,&w,0,0,1,false,true)==0);
 w.present_sequence=1;
 assert(window_server_set_internal(0,&w,10,20,1,true,false)==0);
 assert(w.flags&OS_WINDOW_VISIBLE);
 w.client_pointer_capture=true;w.client_button_capture=true;
 assert(window_server_set_internal(&owner,&w,0,0,0,false,true)==0);
 assert(!w.client_pointer_capture && !w.client_button_capture);
 assert(w.client_hidden && !(w.flags&OS_WINDOW_VISIBLE));
 assert(w.x==10 && w.y==20 && g_window_server.focused_identifier==0);
 assert(window_server_set_internal(0,&w,10,20,1,true,false)==0);
 assert(!(w.flags&OS_WINDOW_VISIBLE));
 assert(window_server_set_internal(&owner,&w,0,0,1,false,true)==0);
 assert((w.flags&OS_WINDOW_VISIBLE) && !w.client_hidden);
 /* The generic hide path must relinquish capture too, without marking the
    client hidden or changing its position. */
 w.client_pointer_capture=true;w.client_button_capture=true;
 w.control_hover=8;w.control_pressed=8;w.control_hover_damage=(Rect){1,2,30,40};
 assert(window_server_set_internal(0,&w,10,20,0,true,false)==0);
 assert(!w.client_pointer_capture && !w.client_button_capture);
 assert(!w.control_hover && !w.control_pressed && rect_is_empty(w.control_hover_damage));
 assert(!w.client_hidden && !(w.flags&OS_WINDOW_VISIBLE));
 assert(window_server_set_internal(0,&w,10,20,1,true,false)==0);
 assert(w.flags&OS_WINDOW_VISIBLE);
 assert(window_server_set_internal(&owner,&w,0,0,2,false,true)==K_EINVAL);
 assert(window_server_set_internal(&owner,&w,0,0,4,false,true)==K_EINVAL);
 /* An inactive first frame must preserve focus, including the later PRESENT. */
 w.present_sequence=0; g_window_server.focused_identifier=7;
 assert(window_server_set_internal(&owner,&w,0,0,3,false,true)==0);
 assert(w.client_show_inactive && !(w.flags&OS_WINDOW_VISIBLE));
 w.present_sequence=1;
 assert(window_server_set_internal(0,&w,10,20,1,true,false)==0);
 assert((w.flags&OS_WINDOW_VISIBLE) && g_window_server.focused_identifier==7);
 assert(window_server_set_internal(&owner,&w,0,0,3,false,true)==0);
 assert(g_window_server.focused_identifier==7);
 assert(window_server_set_internal(&owner,&w,0,0,1,false,true)==0);
 assert(!w.client_show_inactive && g_window_server.focused_identifier==3);
 /* Hiding an active window must not fall back to an inactive popup. */
 window_server_window_t popup={.owner=&owner,.identifier=8,
   .flags=OS_WINDOW_VISIBLE,.client_show_inactive=true};
 g_window_server.scene.count=2;g_window_server.scene.children[1]=&popup;
 assert(window_server_set_internal(&owner,&w,0,0,0,false,true)==0);
 assert(g_window_server.focused_identifier==0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-window-visibility-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root/'include'), str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Visibility: ownership, first frame, hide/late PRESENT, restore and validation passed')
