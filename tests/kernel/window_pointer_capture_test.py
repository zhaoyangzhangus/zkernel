"""Compile production capture ownership/routing with boundary and lifecycle tests."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
window=(root/'kernel/graphics/window.c').read_text()
router=(root/'kernel/graphics/input_router.c').read_text()
owned=window[window.index('kstatus_t window_server_capture_owned('):window.index('kstatus_t window_server_set_minimized_owned(')]
route=router[router.index('static bool route_client_capture_locked('):router.index('void route_input_locked(')]
controls=(root/'kernel/graphics/control_document.c').read_text()
cancel_start=controls.index('bool window_control_cancel_pointer_press_locked(')
cancel=controls[cancel_start:controls.index('\n}',cancel_start)+2]
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
enum {K_OK=0,K_EINVAL=-22,K_EPERM=-1,K_EBUSY=-16,
 INPUT_EVENT_RELATIVE=1,INPUT_EVENT_BUTTON=2,INPUT_EVENT_KEY=3,
 INPUT_REL_X=0,INPUT_REL_Y=1,INPUT_REL_WHEEL=8,OS_WINDOW_VISIBLE=1,
 OS_WINDOW_EXPLICIT_MOVE_RESIZE=64,INPUT_BUTTON_LEFT=1,INPUT_VALUE_RELEASE=0,INPUT_VALUE_PRESS=1};
typedef int kstatus_t;
typedef struct {int id;} process_t;
typedef struct {int type,code,value;} input_event_t;
typedef struct {process_t* owner; uint32_t flags,control_pressed,control_hover; bool client_pointer_capture,client_button_capture,client_hidden,minimized;uint64_t present_sequence;} window_server_window_t;
static struct {uint32_t pointer_x,pointer_y,display_width,display_height;struct {uint32_t count;window_server_window_t* children[4];} scene;} g_window_server;
static int locks, delivered, marks, notifications; static bool ready=true;
static window_server_window_t* last;
static void window_lock(void){assert(locks==0);++locks;}
static void window_unlock(void){assert(locks==1);--locks;}
static bool window_server_kernel_ready(void){return ready;}
static bool window_server_owner_locked(process_t* p,window_server_window_t* w){assert(locks==1);return w->owner==p;}
static void window_enqueue_event_locked(window_server_window_t* w,const input_event_t* e){(void)e;last=w;++delivered;}
static void control_damage_identifier(window_server_window_t* w,uint32_t id){assert(locks==1&&w&&id);++marks;}
void window_server_notify_worker(void){assert(!locks);++notifications;}
'''+cancel+owned+route+r'''
int main(void){
 process_t p={1},foreign={2};
 window_server_window_t a={.owner=&p,.flags=OS_WINDOW_VISIBLE},b={.owner=&p,.flags=OS_WINDOW_VISIBLE};
 g_window_server.scene.count=2;g_window_server.scene.children[0]=&a;g_window_server.scene.children[1]=&b;
 g_window_server.display_width=1920;g_window_server.display_height=1080;
 assert(window_server_capture_owned(0,&a,1)==K_EINVAL);
 assert(window_server_capture_owned(&foreign,&a,1)==K_EPERM && !a.client_pointer_capture);
 assert(window_server_capture_owned(&p,&a,2)==K_EINVAL);
 assert(window_server_capture_owned(&p,&a,1)==K_OK);
 assert(window_server_capture_owned(&p,&a,1)==K_OK);
 assert(window_server_capture_owned(&p,&b,1)==K_EBUSY);
 assert(window_server_capture_owned(&p,&b,0)==K_OK && a.client_pointer_capture);
 input_event_t e={INPUT_EVENT_RELATIVE,INPUT_REL_X,2500};
 assert(route_client_capture_locked(&e) && last==&a && g_window_server.pointer_x==0);
 e.code=INPUT_REL_Y;e.value=-500;assert(route_client_capture_locked(&e) && g_window_server.pointer_y==0);
 e.code=INPUT_REL_Y;e.value=1500;assert(route_client_capture_locked(&e) && g_window_server.pointer_y==0);
 e.type=INPUT_EVENT_BUTTON;assert(route_client_capture_locked(&e) && delivered==4);

 e.type=INPUT_EVENT_RELATIVE;e.code=INPUT_REL_X;e.value=INT32_MAX;
 for(unsigned i=0;i<10000;++i)assert(route_client_capture_locked(&e));
 assert(delivered==10004);
 e.code=INPUT_REL_WHEEL;assert(route_client_capture_locked(&e));
 assert(delivered==10005);
 e.code=7;assert(route_client_capture_locked(&e)); // unknown axis still delivered
 assert(delivered==10006);
 e.type=INPUT_EVENT_KEY;assert(!route_client_capture_locked(&e));
 e.type=INPUT_EVENT_BUTTON;
 a.client_hidden=true;assert(!route_client_capture_locked(&e));
 assert(window_server_capture_owned(&p,&a,1)==K_EINVAL);
 a.client_hidden=false;a.minimized=true;assert(!route_client_capture_locked(&e));
 a.minimized=false;a.flags=0;a.present_sequence=1;assert(!route_client_capture_locked(&e));
 assert(window_server_capture_owned(&p,&a,1)==K_EINVAL);
 assert(window_server_capture_owned(&p,&a,0)==K_OK);
 // Show defers exposing the first surface, not capture ownership/routing.
 a.present_sequence=0;
 assert(window_server_capture_owned(&p,&a,1)==K_OK && a.client_pointer_capture);
 assert(route_client_capture_locked(&e) && last==&a);
 assert(window_server_capture_owned(&p,&b,1)==K_EBUSY);
 assert(window_server_capture_owned(&p,&a,0)==K_OK);
 a.flags=OS_WINDOW_VISIBLE;
 assert(window_server_capture_owned(&p,&a,0)==K_OK && !route_client_capture_locked(&e));
 a.client_button_capture=true;e.code=INPUT_BUTTON_LEFT;e.value=1;
 assert(route_client_capture_locked(&e) && last==&a && a.client_button_capture);
 e.value=INPUT_VALUE_RELEASE;
 assert(route_client_capture_locked(&e) && !a.client_button_capture);
 assert(!route_client_capture_locked(&e));
 // A same-process popup can take over the parent's implicit press grab.
 // No explicit or foreign capture may be stolen, and a later array conflict
 // must not clear an earlier same-owner grab before returning failure.
 a.client_button_capture=true;
 a.control_pressed=7;a.control_hover=7;
 assert(window_server_capture_owned(&p,&b,0)==K_OK&&a.client_button_capture);
 b.client_hidden=true;
 assert(window_server_capture_owned(&p,&b,1)==K_EINVAL&&a.client_button_capture);
 b.client_hidden=false;a.client_pointer_capture=true;
 assert(window_server_capture_owned(&p,&b,1)==K_EBUSY&&a.client_button_capture);
 a.client_pointer_capture=false;a.owner=&foreign;
 assert(window_server_capture_owned(&p,&b,1)==K_EBUSY&&a.client_button_capture);
 a.owner=&p;
 window_server_window_t c={.owner=&foreign,.flags=OS_WINDOW_VISIBLE,.client_pointer_capture=true};
 g_window_server.scene.children[2]=&c;g_window_server.scene.count=3;
 assert(window_server_capture_owned(&p,&b,1)==K_EBUSY&&a.client_button_capture&&!b.client_pointer_capture);
 c.client_pointer_capture=false;c.client_button_capture=true;
 assert(window_server_capture_owned(&p,&b,1)==K_EBUSY&&a.client_button_capture&&c.client_button_capture);
 assert(a.control_pressed==7 && a.control_hover==7 && marks==0 && notifications==0);
 c.owner=&p;
 c.control_pressed=8;c.control_hover=8;
 assert(window_server_capture_owned(&p,&b,1)==K_OK&&b.client_pointer_capture);
 assert(!a.client_button_capture&&!c.client_button_capture);
 /* 原窗口收不到释放事件，捕获转移必须取消按压；物理悬停仍由场景路由管理。 */
 assert(!a.control_pressed && !c.control_pressed);
 assert(a.control_hover==7 && c.control_hover==8 && marks==2);
 assert(notifications==1);
 assert(window_server_capture_owned(&p,&b,1)==K_OK && marks==2);
 assert(notifications==1);
 assert(route_client_capture_locked(&e)&&last==&b); // The popup receives release.
 assert(window_server_capture_owned(&p,&b,0)==K_OK);
 g_window_server.scene.count=2;g_window_server.scene.children[2]=0;
 g_window_server.scene.children[0]=0;
 assert(window_server_capture_owned(&p,&b,1)==K_OK && route_client_capture_locked(&e) && last==&b);
 g_window_server.scene.count=1;assert(!route_client_capture_locked(&e)); // removed window
 assert(locks==0);
}
'''
assert router.index('window_route_pointer_scene_locked(event);') < router.index('if (route_client_capture_locked(event))')
assert 'window->client_pointer_capture = false;' in router
assert 'if (visible == 0U && !defer_first_surface) {\n        window_input_release_pointer_locked(window);' in window
with tempfile.TemporaryDirectory() as directory:
    cpp=Path(directory)/'capture.c';exe=Path(directory)/'capture'
    cpp.write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    # 移除按压取消后，同一测试必须复现捕获转移留下的旧状态。
    previous=program.replace('window->control_pressed = 0U;', '(void)window->control_pressed;')
    assert previous!=program
    cpp.write_text(previous)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe)],capture_output=True)
    assert result.returncode!=0 and b'!a.control_pressed && !c.control_pressed' in result.stderr
print('Pointer capture: ownership, exclusive/idempotent acquire, release, recipient-only routing, keyboard exclusion and lifecycle passed')
