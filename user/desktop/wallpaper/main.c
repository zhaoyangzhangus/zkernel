#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <uapi/all.h>
#include <uapi/desktop.h>
#include "../../runtime/text.h"
#include "../../runtime/caption.h"
#include "../../runtime/caption_tree.h"
#include "../../runtime/file_picker.h"
#include "../../runtime/modal_tree.h"

static ui_file_picker_t picker;
static os_window_create_t window;
static os_control_node_t nodes[128];
static char text[8192];
static os_window_control_tree_t tree;
static const char *dialog_title="更换壁纸";
static int64_t call(uint64_t number,uint64_t argument) {
    register uint64_t rax __asm__("rax")=number;
    register uint64_t rdi __asm__("rdi")=argument;
    __asm__ volatile("syscall":"+a"(rax),"+D"(rdi)::"rcx","r11","memory");return (int64_t)rax;
}
static bool render(void) {
    uint32_t count=1,used=0;
    nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
        .width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL,.background=OS_THEME_SURFACE,
        .foreground=OS_THEME_TEXT,.font_size=ui_text_size()};
    if (!user_caption_tree(nodes,&count,128,text,&used,sizeof(text),(user_caption_layout_t){
        .title=dialog_title,.width=window.width,.height=user_caption_height(),.font=ui_text_size()})) return false;
    ui_file_picker_document_t document;
    if (!ui_file_picker_build(&picker,window.identifier,window.width,window.height,&document) ||
        !ui_modal_tree_attach(nodes,&count,128,text,&used,sizeof(text),&document.request)) return false;
    tree=(os_window_control_tree_t){.hdr={sizeof(tree),OS_SYSCALL_ABI_VERSION},.identifier=window.identifier,
        .nodes=(uintptr_t)nodes,.count=count,.text=(uintptr_t)text,.text_size=used};
    if (call(OS_SYS_WINDOW_CONTROL_TREE,(uintptr_t)&tree)<0) {tree.count=0;return false;}
    return true;
}
static char key_to_ascii(uint32_t code, bool shift) {
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (code >= 'A' && code <= 'Z') return shift ? (char)code : (char)(code - 'A' + 'a');
    if (code >= 'a' && code <= 'z') return shift ? (char)(code - 'a' + 'A') : (char)code;
    if (code >= 0x04U && code <= 0x1DU) return shift ? upper[code - 0x04U] : lower[code - 0x04U];
    if (code >= 0x1EU && code <= 0x26U) {
        static const char numbers[] = "123456789";
        static const char symbols[] = "!@#$%^&*(";
        return shift ? symbols[code - 0x1EU] : numbers[code - 0x1EU];
    }
    if (code == 0x27U) return shift ? ')' : '0';
    if (code == 0x2CU) return ' ';
    if (code == 0x2DU) return shift ? '_' : '-';
    if (code == 0x2EU) return shift ? '+' : '=';
    if (code == 0x2FU) return shift ? '{' : '[';
    if (code == 0x30U) return shift ? '}' : ']';
    if (code == 0x31U) return shift ? '|' : '\\';
    if (code == 0x33U) return shift ? ':' : ';';
    if (code == 0x34U) return shift ? '"' : '\'';
    if (code == 0x35U) return shift ? '~' : '`';
    if (code == 0x36U) return shift ? '<' : ',';
    if (code == 0x37U) return shift ? '>' : '.';
    if (code == 0x38U) return shift ? '?' : '/';
    return '\0';
}

int main(int argc,char **argv) {
    bool select_only=argc==5;
    enum ui_file_picker_mode mode=UI_PICKER_OPEN_FILE;
    if (argc!=1 && !select_only) return 1;
    if (select_only) {
        if (strcmp(argv[3],"--result-file") || argv[4][0]!='/') return 1;
        if (!strcmp(argv[1],"--save")) {mode=UI_PICKER_SAVE_FILE;dialog_title="保存文件";}
        else if (!strcmp(argv[1],"--open")) dialog_title="打开文件";
        else if (!strcmp(argv[1],"--directory")) {mode=UI_PICKER_DIRECTORY;dialog_title="选择目录";}
        else return 1;
    }
    int exit_code=1;
    bool window_created=false;
    if (!ui_text_init(OS_TEXT_DEFAULT_SIZE)) return 1;
    os_display_info_t display={.hdr={sizeof(display),OS_SYSCALL_ABI_VERSION}};
    if (call(OS_SYS_DISPLAY_GET_INFO,(uintptr_t)&display)<0) goto cleanup;
    window=(os_window_create_t){.hdr={sizeof(window),OS_SYSCALL_ABI_VERSION},
        .width=display.width<800?display.width:800,.height=display.height<620?display.height:620,
        .flags=OS_WINDOW_VISIBLE|OS_WINDOW_RESIZABLE|OS_WINDOW_STANDARD_CAPTION_CONTROLS|
            OS_WINDOW_CAPTION_HEIGHT(user_caption_height()),
        .background=OS_THEME_SURFACE,.address=0x12000000ULL};
    window.x=(int32_t)((display.width-window.width)/2U);window.y=(int32_t)((display.height-window.height)/2U);
    user_caption_format_title(window.title,dialog_title,false);
    if (call(OS_SYS_WINDOW_CREATE,(uintptr_t)&window)<0) goto cleanup;
    window_created=true;
    if (!ui_file_picker_show(&picker,mode,select_only?argv[2]:"/usr/apps/desktop/wall.png",call) ||
        !render()) goto cleanup;
    while (picker.active) {
        os_window_event_read_t request={.hdr={sizeof(request),OS_SYSCALL_ABI_VERSION},
            .identifier=window.identifier,.timeout_ns=UINT64_MAX};
        int64_t read_status=call(OS_SYS_WINDOW_EVENT_READ,(uintptr_t)&request);
        if (read_status==-EINTR) continue;
        /* 窗口失效或事件通道关闭后退出，避免空转并让浏览器收到取消结果。 */
        if (read_status<0) break;
        os_window_event_t *event=&request.event;
        if (event->type==OS_WINDOW_EVENT_RESIZE) {
            window.width=event->resize.width;window.height=event->resize.height;
            if (!render()) break;
            continue;
        }
        if (event->type==OS_WINDOW_EVENT_CONTROL && !os_control_event_node(&tree,nodes,&event->control)) continue;
        bool raw=event->type==OS_WINDOW_EVENT_INPUT && event->input.type==OS_INPUT_EVENT_KEY;
        bool key=event->type==OS_WINDOW_EVENT_CONTROL && event->control.action==OS_CONTROL_KEY;
        enum ui_file_picker_event_result result=ui_file_picker_event(&picker,event,
            raw||key?key_to_ascii(raw?event->input.code:event->control.code,picker.shift):0);
        if (result==UI_PICKER_SELECTED) {
            os_desktop_wallpaper_t change={.hdr={sizeof(change),OS_SYSCALL_ABI_VERSION}};
            if (ui_file_picker_result(&picker,change.path,sizeof(change.path))) {
                if (select_only) {
                    /* 只返回原始完整路径，不添加分隔符，也不经过命令解释器。 */
                    /* 浏览器预先创建结果文件，助手只写完整路径，不创建任意目标。 */
                    int descriptor=open(argv[4],O_WRONLY|O_TRUNC);
                    if (descriptor<0) break;
                    size_t size=strlen(change.path),written=0;
                    while (written<size) {
                        ssize_t result=write(descriptor,change.path+written,size-written);
                        if (result<0 && errno==EINTR) continue;
                        if (result<=0) break;
                        written+=(size_t)result;
                    }
                    int close_status=close(descriptor);
                    if (written==size && close_status==0) exit_code=0;
                    break;
                }
                int64_t status=call(OS_SYS_DESKTOP_WALLPAPER,(uintptr_t)&change);
                if (status==0) {exit_code=0;break;}
                picker.active=true;picker.accepted=false;
                ui_message_copy(picker.error,sizeof(picker.error),
                    status==-16?"正在更新壁纸，请稍后重试":"无法设置壁纸：请选择有效 PNG 图片并确认系统盘可写");
            }
        }
        if (picker.active && result!=UI_PICKER_IDLE && !render()) break;
    }
cleanup:
    ui_file_picker_close(&picker);
    if (window_created) call(OS_SYS_HANDLE_CLOSE,window.window);
    ui_text_shutdown();
    return exit_code;
}
