/* 文件界面示例：应用只绑定行为，不绘制按钮、文字、边框或焦点。 */
#include "control_file.h"
#include <uapi/window.h>
#include <uapi/window_theme.h>
#include <uapi/syscall.h>
#include <stdio.h>

static int64_t ui_call(uint64_t number,void *argument) {
    int64_t result;
    __asm__ volatile("syscall":"=a"(result):"a"(OS_SYSCALL_NATIVE_FLAG|number),
        "D"((uintptr_t)argument):"rcx","r11","memory");
    return result;
}

int main(int argc,char **argv) {
    ui_file_document_t document={0};ui_file_error_t error;
    if(!ui_file_load(argc>1?argv[1]:"/usr/apps/ui-file-demo/controls.ui",&document,&error)){
        fprintf(stderr,"UI file: %s at byte %zu\n",error.message?error.message:"error",error.offset);return 1;
    }
    os_window_create_t window={.hdr={.size=sizeof(window),.version=1},
        .x=300,.y=180,.width=720,.height=520,.address=0x700000000000ULL,
        .flags=OS_WINDOW_VISIBLE|OS_WINDOW_RESIZABLE|OS_WINDOW_STANDARD_CAPTION_CONTROLS,
        .background=OS_THEME_SURFACE,.title="控件测试"};
    if(ui_call(OS_SYS_WINDOW_CREATE,&window)<0){ui_file_destroy(&document);return 2;}
    document.request.identifier=window.identifier;
    unsigned activations=0;
    os_control_node_t *apply=ui_file_find(&document,"apply");
    /* 指针只由本进程绑定；文件与合成器都不能构造或解引用它。 */
    if(apply)apply->user_data=(uintptr_t)&activations;
    int result=0;
    if(ui_call(OS_SYS_WINDOW_CONTROL_TREE,&document.request)<0){result=3;goto done;}
    for(;;){
        os_window_event_read_t request={.hdr={.size=sizeof(request),.version=1},
            .identifier=window.identifier,.timeout_ns=UINT64_MAX};
        int64_t status=ui_call(OS_SYS_WINDOW_EVENT_READ,&request);
        if(status==-4)continue;
        if(status<0){result=4;break;}
        os_window_event_t *event=&request.event;
        if(event->type==OS_WINDOW_EVENT_CLOSE_REQUEST)break;
        if(event->type!=OS_WINDOW_EVENT_CONTROL)continue;
        const os_control_node_t *target=os_control_event_node(&document.request,document.nodes,&event->control);
        if(!target)continue;
        os_control_node_t *node=document.nodes+event->control.node_index;
        bool changed=false;
        if(event->control.action==OS_CONTROL_CHANGE){
            if(os_control_is_choice(node->kind)){
                node->flags&=~OS_CONTROL_CHECKED;
                if(event->control.value)node->flags|=OS_CONTROL_CHECKED;
                changed=true;
            }else if(os_control_is_range(node->kind)){
                node->value=(uint32_t)event->control.value;changed=true;
                os_control_node_t *progress=ui_file_find(&document,"progress");
                if(progress&&progress->kind==OS_CONTROL_PROGRESS)progress->value=node->value;
            }
        }else if(event->control.action==OS_CONTROL_CLICK){
            if(node->identifier==ui_file_identifier("close"))break;
            if(node->kind==OS_CONTROL_LIST_ITEM||node->kind==OS_CONTROL_TAB){
                node->flags^=OS_CONTROL_CHECKED;changed=true;
            }else if(node==apply){
                ++*(unsigned *)(uintptr_t)event->control.user_data;
                node->flags&=~OS_CONTROL_INHERIT_BACKGROUND;
                node->background=activations&1U?OS_THEME_DANGER:OS_THEME_ACCENT;changed=true;
            }
        }
        if(changed&&ui_call(OS_SYS_WINDOW_CONTROL_TREE,&document.request)<0){result=3;break;}
    }
done:
    (void)ui_call(OS_SYS_HANDLE_CLOSE,(void *)(uintptr_t)window.window);
    ui_file_destroy(&document);return result;
}
