"""Production native minimize/dock restore, ownership and event queue semantics."""
from pathlib import Path
import subprocess
import resource
import tempfile

root = Path(__file__).resolve().parents[2]

def function(file, signature):
    text = (root/file).read_text()
    start = text.index(signature)
    end = text.index('{', start)+1
    depth = 1
    while depth:
        depth += (text[end] == '{')-(text[end] == '}')
        end += 1
    return text[start:end]

ioctl_source = (root/'kernel/syscall/linux.c').read_text()
ioctl_start = ioctl_source.index('static kstatus_t linux_window_ioctl(')
ioctl_switch = ioctl_source.index('    switch (command->operation)', ioctl_start)
branch_start = ioctl_source.index('    case LINUX_WINDOW_OPERATION_MINIMIZE:', ioctl_switch)
branch_end = ioctl_source.index('    case ', branch_start + len('    case LINUX_WINDOW_OPERATION_MINIMIZE:'))
dispatch = (ioctl_source[ioctl_start:ioctl_switch] +
            'switch(command->operation) {\n' + ioctl_source[branch_start:branch_end] +
            'default: return K_EINVAL;\n}\n}\n')

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "uapi/window.h"
#include "uapi/linux_abi.h"
#include "uapi/window_state.h"
#include "kernel/rect.h"
#define WINDOW_EVENT_CAPACITY 4U
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define LINUX_ENOTTY -25
#define DESKTOP_APP_CHROME 6U
typedef int kstatus_t;
typedef struct process {int id;} process_t;
typedef struct window_server_window {
 uint32_t identifier,flags,desktop_app;
 bool minimized,dirty,client_show_inactive,client_pointer_capture,client_button_capture;
 uint32_t control_hover,control_pressed,control_text_ctrl;bool control_text_drag;Rect control_hover_damage;
 uint64_t animation_start;bool animation_seen;
 process_t *owner;
 int x,y;uint32_t width,height;
 uint32_t event_read,event_write,event_count;
 os_window_event_t events[WINDOW_EVENT_CAPACITY];
} window_server_window_t;
static struct {
 uint32_t focused_identifier,dragging_identifier,title_pressed_identifier;
 struct {uint32_t count;window_server_window_t *children[3];} scene;int event_waitq;
} g_window_server;
static int locked,marks,wakes,notifies,drag_clears,title_clears,rehits,global_wakes;
static void window_input_refresh_pointer_locked(void){assert(locked);++rehits;}
static bool ready=true;
static void window_lock(void){assert(!locked);locked=1;}
static void window_unlock(void){assert(locked);locked=0;}
static bool window_server_kernel_ready(void){return ready;}
static bool window_server_owner_locked(process_t *p,window_server_window_t *w){assert(locked);return w->owner==p && w->identifier!=0;}
static void window_mark_window_locked(window_server_window_t *w){assert(locked);if(w)++marks;}
static void window_animation_restore_locked(window_server_window_t *w){assert(locked&&w);}
static void window_animation_depart_locked(window_server_window_t *w,bool minimize){assert(locked&&w&&!w->minimized&&minimize);}
static void window_input_clear_drag_locked(void){assert(locked);++drag_clears;g_window_server.dragging_identifier=0;}
static void window_clear_title_capture_locked(void){assert(locked);++title_clears;g_window_server.title_pressed_identifier=0;}
static void window_scene_set_focus_identifier_locked(uint32_t id){assert(locked);g_window_server.focused_identifier=id;}
static void window_scene_focus_locked(window_server_window_t *w){assert(locked);g_window_server.focused_identifier=w->identifier;}
static void window_event_schedule_wake_locked(window_server_window_t *w){assert(locked && w);++wakes;}
static void window_server_notify_worker(void){assert(!locked);++notifies;}
static const char *window_shell_app_title(uint32_t app){return app==DESKTOP_APP_CHROME?"Chrome":0;}
static int wake_all(void *queue){assert(!locked && queue);++global_wakes;return 0;}
''' + '\n'.join((
    function('kernel/graphics/input_events.c', 'static bool window_event_discard_motion_locked('),
    function('kernel/graphics/input_events.c', 'static os_window_event_t *window_event_reserve_locked('),
    function('kernel/graphics/input_events.c', 'static void window_event_commit_locked('),
    function('kernel/graphics/input_events.c', 'void window_enqueue_minimized_event_locked('),
    function('kernel/graphics/input_router.c', 'void window_input_release_pointer_locked('),
    function('kernel/graphics/input_router.c', 'bool window_minimize_locked('),
    function('kernel/graphics/window.c', 'kstatus_t window_server_set_minimized_owned('),
    function('kernel/graphics/launcher.c', 'bool desktop_restore_minimized_app('),
)) + dispatch + r'''
int main(void){
 _Static_assert(sizeof(os_window_event_t)==64,"event ABI");
 _Static_assert(sizeof(linux_window_command_t)==48,"command ABI unchanged");
 _Static_assert(offsetof(os_window_event_t,minimized)==8,"payload ABI");
 process_t owner={1},other={2};
 window_server_window_t normal={.identifier=1,.flags=OS_WINDOW_VISIBLE,.owner=&other};
 window_server_window_t chrome={.desktop_app=DESKTOP_APP_CHROME,.identifier=2,.flags=OS_WINDOW_VISIBLE,.owner=&owner,.x=10,.y=10,.width=945,.height=1060};
 window_server_window_t popup={.desktop_app=DESKTOP_APP_CHROME,.identifier=3,.flags=OS_WINDOW_VISIBLE,.owner=&other,.client_show_inactive=true};
 g_window_server.scene.count=3;g_window_server.scene.children[0]=&normal;g_window_server.scene.children[1]=&chrome;g_window_server.scene.children[2]=&popup;
 g_window_server.focused_identifier=2;g_window_server.dragging_identifier=2;g_window_server.title_pressed_identifier=2;
 /* 最小化打断按压后，恢复时不得继续显示旧控件的按压或悬停状态。 */
 chrome.client_pointer_capture=true;chrome.client_button_capture=true;
 chrome.control_hover=7;chrome.control_pressed=7;
 chrome.control_hover_damage=(Rect){1,2,30,40};
 assert(window_server_set_minimized_owned(&other,&chrome,1)==K_EPERM);
 assert(window_server_set_minimized_owned(&owner,&chrome,2)==K_EINVAL);
 assert(window_server_set_minimized_owned(0,&chrome,1)==K_EINVAL);
 ready=false;assert(window_server_set_minimized_owned(&owner,&chrome,1)==K_EINVAL);ready=true;
 assert(window_server_set_minimized_owned(&owner,&chrome,1)==0);
 assert(chrome.minimized && chrome.dirty && chrome.flags==OS_WINDOW_VISIBLE);
 assert(!chrome.client_pointer_capture && !chrome.client_button_capture);
 assert(!chrome.control_hover && !chrome.control_pressed && rect_is_empty(chrome.control_hover_damage));
 assert(chrome.x==10 && chrome.y==10 && chrome.width==945 && chrome.height==1060);
 assert(g_window_server.focused_identifier==1 && drag_clears==1 && title_clears==1);
 assert(chrome.event_count==1 && wakes==1 && notifies==1);
 os_window_event_t event=chrome.events[0];
 assert(event.identifier==2 && event.type==OS_WINDOW_EVENT_MINIMIZED && event.minimized.minimized==1);
 assert(event.minimized.reserved==0 && event.minimized.reserved2==0 && event.minimized.reserved3==0 && event.pointer_x==0 && event.pointer_y==0);
 assert(rehits==1);
 assert(window_server_set_minimized_owned(&owner,&chrome,1)==0 && chrome.event_count==1);
 assert(rehits==1);
 assert(desktop_restore_minimized_app(DESKTOP_APP_CHROME));
 assert(!chrome.minimized && g_window_server.focused_identifier==2 && chrome.event_count==2 && rehits==2);
 assert(chrome.events[1].minimized.minimized==0 && chrome.events[1].type==OS_WINDOW_EVENT_MINIMIZED);
 assert(window_server_set_minimized_owned(&owner,&chrome,1)==0);
 assert(window_server_set_minimized_owned(&owner,&chrome,0)==0);
 assert(!chrome.minimized && chrome.event_count==4 && rehits==4);
 // 队列饱和时保留最新状态，并将重用的输入负载清零。
 memset(&chrome.events[chrome.event_write],0xaa,sizeof(os_window_event_t));
 uint32_t slot=chrome.event_write,read=chrome.event_read;
 assert(window_server_set_minimized_owned(&owner,&chrome,1)==0);
 assert(chrome.event_count==4 && chrome.event_read==(read+1)%4);
 event=chrome.events[slot];assert(event.minimized.minimized==1 && event.minimized.reserved3==0 && event.pointer_y==0);
 assert(window_server_set_minimized_owned(&owner,&chrome,0)==0);
 chrome.flags=0;assert(window_server_set_minimized_owned(&owner,&chrome,1)==K_EINVAL);
 chrome.flags=OS_WINDOW_VISIBLE;
 linux_window_command_t command={.operation=LINUX_WINDOW_OPERATION_MINIMIZE,.width=1};
 assert(linux_window_ioctl(&owner,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command))==0 && chrome.minimized);
 command.width=0;
 assert(linux_window_ioctl(&owner,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command))==0 && !chrome.minimized);
 command.width=2;assert(linux_window_ioctl(&owner,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command))==K_EINVAL);command.width=1;
 command.height=1;assert(linux_window_ioctl(&owner,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command))==K_EINVAL);command.height=0;
 command.x=1;assert(linux_window_ioctl(&owner,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command))==K_EINVAL);command.x=0;
 command.y=1;assert(linux_window_ioctl(&owner,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command))==K_EINVAL);command.y=0;
 command.stride=1;assert(linux_window_ioctl(&owner,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command))==K_EINVAL);command.stride=0;
 command.address=1;assert(linux_window_ioctl(&owner,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command))==K_EINVAL);command.address=0;
 command.length=1;assert(linux_window_ioctl(&owner,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command))==K_EINVAL);command.length=0;
 command.reserved=1;assert(linux_window_ioctl(&owner,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command))==LINUX_ENOTTY);command.reserved=0;
 command.reserved2=1;assert(linux_window_ioctl(&owner,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command))==LINUX_ENOTTY);command.reserved2=0;
 assert(linux_window_ioctl(&owner,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command)-1)==LINUX_ENOTTY);
 assert(linux_window_ioctl(&owner,&chrome,0,&command,sizeof(command))==LINUX_ENOTTY);
 assert(linux_window_ioctl(&other,&chrome,LINUX_WINDOW_IOCTL_COMMIT,&command,sizeof(command))==K_EPERM);
 assert(!chrome.minimized);
 assert(!desktop_restore_minimized_app(99));
 /* 只剩同应用弹层时不能恢复它；隐藏主窗口和同进程其他应用也不能冒充候选。 */
 g_window_server.scene.count=3;
 g_window_server.scene.children[0]=NULL;
 g_window_server.scene.children[1]=&normal;
 g_window_server.scene.children[2]=&popup;
 normal.owner=&owner;normal.desktop_app=1;
 uint32_t focused=g_window_server.focused_identifier;
 int prior_marks=marks,prior_hits=rehits;
 assert(!desktop_restore_minimized_app(DESKTOP_APP_CHROME));
 assert(g_window_server.focused_identifier==focused&&marks==prior_marks&&rehits==prior_hits);
 normal.desktop_app=DESKTOP_APP_CHROME;normal.flags=0;
 assert(!desktop_restore_minimized_app(DESKTOP_APP_CHROME));
 normal.flags=OS_WINDOW_VISIBLE;normal.minimized=true;
 assert(desktop_restore_minimized_app(DESKTOP_APP_CHROME));
 assert(!normal.minimized&&g_window_server.focused_identifier==normal.identifier);
 assert(!popup.dirty&&!popup.event_count);
 /* 已在最前且聚焦的窗口，重复点击不能制造重绘、重新命中或唤醒。 */
 g_window_server.scene.children[1]=&popup;
 g_window_server.scene.children[2]=&normal;
 normal.dirty=false;
 int saved_marks=marks,saved_hits=rehits,saved_wakes=global_wakes;
 uint32_t saved_events=normal.event_count;
 for(unsigned i=0;i<10000;++i)assert(desktop_restore_minimized_app(DESKTOP_APP_CHROME));
 assert(!normal.dirty&&marks==saved_marks&&rehits==saved_hits&&global_wakes==saved_wakes);
 assert(normal.event_count==saved_events&&!locked);
 /* 层级、焦点或最小化任一发生变化，仍须恢复和重绘。 */
 g_window_server.scene.children[1]=&normal;
 g_window_server.scene.children[2]=&popup;
 assert(desktop_restore_minimized_app(DESKTOP_APP_CHROME));
 assert(normal.dirty&&marks>saved_marks&&rehits==saved_hits+1&&global_wakes==saved_wakes+1);
 g_window_server.scene.children[1]=&popup;g_window_server.scene.children[2]=&normal;
 normal.dirty=false;g_window_server.focused_identifier=0;
 assert(desktop_restore_minimized_app(DESKTOP_APP_CHROME)&&normal.dirty);
 assert(g_window_server.focused_identifier==normal.identifier);
 normal.minimized=true;normal.dirty=false;
 saved_events=normal.event_count;
 assert(desktop_restore_minimized_app(DESKTOP_APP_CHROME)&&normal.dirty&&!normal.minimized);
 assert(normal.event_count==saved_events+1);
 assert(locked==0 && marks>0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-minimize-') as directory:
    path = Path(directory)
    command=['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-fno-pie','-no-pie','-I',str(root/'include'),
             '-x','c','-','-o',str(path/'test')]
    def run(code):
        subprocess.run(command,input=code,text=True,check=True)
        return subprocess.run([str(path/'test')],capture_output=True,text=True,
            timeout=20,preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    result=run(program)
    assert result.returncode==0,result.stderr
    for label,old in (
        ('重复点击仍整窗重绘', 'if (!window->minimized &&\n        g_window_server.scene.children[g_window_server.scene.count - 1U] == window &&'),
        ('恢复了同应用弹出窗口','candidate->client_show_inactive || '),
        ('错误恢复其他应用',' || candidate->desktop_app != app'),
        ('错误恢复隐藏窗口','(candidate->flags & OS_WINDOW_VISIBLE) == 0U ||'),
    ):
        broken=program.replace(old,old.replace('if (','if (false && ',1) if old.startswith('if (') else '')
        assert broken!=program
        result=run(broken)
        assert result.returncode!=0 and 'Assertion' in result.stderr,result.stderr
        print('反例被拒绝：',label)
print('Native minimize: ownership, focus/capture, retained geometry, dock restore, inactive popups, hidden/foreign apps, queue saturation and stable ABI passed')
print('Dock restore: 10000 focused/front-window requests cause zero redraws, pointer re-hits or global wakes; stacking/focus/minimize changes retain restoration passed')
