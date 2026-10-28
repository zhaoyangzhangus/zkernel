/* 声明式界面验证：应用仅填写节点与原始内容，不绘制按钮和文本。 */
#include <stdint.h>
#include <uapi/linux_abi.h>
#include <uapi/window.h>
#include <uapi/window_state.h>
#include <uapi/control_tree.h>
#include <uapi/window_theme.h>
#include <uapi/syscall.h>

void *memset(void *target, int value, size_t count) {
    volatile unsigned char *bytes = target;
    for (size_t i=0; i<count; ++i) bytes[i]=(unsigned char)value;
    return target;
}

static int64_t call(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    int64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return result;
}
static int demo_repaint(int64_t fd, linux_window_create_t *window, unsigned patches) {
    os_surface_command_t command={.operation=OS_SURFACE_BEGIN};
    if(call(LINUX_SYS_IOCTL,fd,LINUX_WINDOW_IOCTL_SURFACE,(uintptr_t)&command)<0)return -1;
    os_surface_frame_t *frame=&command.frame;
    if(!frame->width||!frame->height||frame->stride!=(uint64_t)frame->width*4U||
        (uint64_t)frame->stride*frame->height>frame->capacity) {
        command.operation=OS_SURFACE_CANCEL;
        (void)call(LINUX_SYS_IOCTL,fd,LINUX_WINDOW_IOCTL_SURFACE,(uintptr_t)&command);
        return -1;
    }
    /* 只写 BEGIN 授予的后台槽位；完整提交前，合成器继续显示旧的前台帧。 */
    uint32_t *pixels=(void*)(uintptr_t)frame->address;
    for(uint32_t y=0;y<frame->height;++y)for(uint32_t x=0;x<frame->width;++x) {
        uint32_t color=((x/24+y/24)&1)?0x286fa0:0x438fc0;
        if(patches&&y>=8U&&y<20U&&x>=8U&&x<24U)color=patches&1U?0xe85d3f:0x43cf7b;
        pixels[(uint64_t)y*frame->width+x]=color;
    }
    command.operation=OS_SURFACE_COMMIT;
    int64_t result=call(LINUX_SYS_IOCTL,fd,LINUX_WINDOW_IOCTL_SURFACE,(uintptr_t)&command);
    if(result>=0){window->width=frame->width;window->height=frame->height;return 0;}
    /* 尺寸竞态已由服务器中止后台帧；其他失败必须显式释放写租约。 */
    if(result==-11)return 1;
    command.operation=OS_SURFACE_CANCEL;
    (void)call(LINUX_SYS_IOCTL,fd,LINUX_WINDOW_IOCTL_SURFACE,(uintptr_t)&command);
    return -1;
}
__attribute__((used)) static int demo_main(void) {
    linux_window_create_t window = {.x=520,.y=270,.width=720,.height=420,
        .flags=OS_WINDOW_VISIBLE|OS_WINDOW_RESIZABLE|OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_SURFACE_EXCHANGE,
        .title="CONTROL TREE"};
    int64_t fd = call(LINUX_SYS_WINDOW_CREATE, (uintptr_t)&window, 0, 0);
    if (fd < 0) return 1;
    char text[] = "Compositor controlsSurface is a childApplyResetCheckRadioSwitchItemTab";
    unsigned clicks=0,patches=0;
    os_control_node_t nodes[] = {
        {.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
         .flags=OS_CONTROL_MANAGED_FOCUS,
         .layout=OS_CONTROL_COLUMN,.padding=24,.gap=16,.background=OS_THEME_SURFACE,
         .foreground=OS_THEME_TEXT,.font_size=20},
        {.identifier=2,.parent=0,.kind=OS_CONTROL_TEXT,.width=OS_CONTROL_FILL,.height=40,
         .flags=OS_CONTROL_INHERIT_FOREGROUND|OS_CONTROL_INHERIT_BACKGROUND,
         .font_size=24,.text_length=19},
        {.identifier=3,.parent=0,.kind=OS_CONTROL_CONTAINER,.width=OS_CONTROL_FILL,.height=44,
         .layout=OS_CONTROL_ROW,.gap=16,
         .flags=OS_CONTROL_INHERIT_STYLE|OS_CONTROL_ALIGN_CENTER},
        {.identifier=4,.parent=2,.kind=OS_CONTROL_BUTTON,.width=140,.height=44,
         .flags=OS_CONTROL_INHERIT_FONT,
         .background=OS_THEME_ACCENT,.foreground=OS_THEME_ON_ACCENT,.radius=OS_THEME_RADIUS,
         .user_data=(uintptr_t)&clicks,
         .text_offset=37,.text_length=5},
        {.identifier=7,.parent=2,.kind=OS_CONTROL_BUTTON,.width=100,.height=44,
         .flags=OS_CONTROL_INHERIT_TEXT,
         .background=OS_THEME_FIELD,.radius=OS_THEME_RADIUS,
         .user_data=(uintptr_t)&patches,
         .text_offset=42,.text_length=5},
        {.identifier=5,.parent=2,.kind=OS_CONTROL_TEXT,.width=OS_CONTROL_FILL,.height=44,
         .flags=OS_CONTROL_INHERIT_STYLE|OS_CONTROL_FLEX_GROW,
         .padding=10,
         .text_offset=19,.text_length=18},
        {.identifier=9,.parent=0,.kind=OS_CONTROL_CONTAINER,.width=OS_CONTROL_FILL,.height=36,
         .layout=OS_CONTROL_ROW,.gap=16,.flags=OS_CONTROL_INHERIT_STYLE},
        {.identifier=10,.parent=6,.kind=OS_CONTROL_CHECKBOX,.width=160,.height=36,
         .flags=OS_CONTROL_INHERIT_STYLE,.text_offset=47,.text_length=5},
        {.identifier=11,.parent=6,.kind=OS_CONTROL_RADIO,.width=160,.height=36,
         .flags=OS_CONTROL_INHERIT_STYLE,.text_offset=52,.text_length=5},
        {.identifier=12,.parent=6,.kind=OS_CONTROL_SWITCH,.width=180,.height=36,
         .flags=OS_CONTROL_INHERIT_STYLE,.text_offset=57,.text_length=6},
        {.identifier=13,.parent=0,.kind=OS_CONTROL_CONTAINER,.width=OS_CONTROL_FILL,.height=24,
         .layout=OS_CONTROL_ROW,.gap=20,.flags=OS_CONTROL_INHERIT_STYLE},
        {.identifier=14,.parent=10,.kind=OS_CONTROL_PROGRESS,.width=180,.height=24,
         .flags=OS_CONTROL_INHERIT_STYLE,.value=5000},
        {.identifier=15,.parent=10,.kind=OS_CONTROL_SLIDER,.width=180,.height=24,
         .flags=OS_CONTROL_INHERIT_STYLE,.value=5000},
        {.identifier=16,.parent=10,.kind=OS_CONTROL_SCROLLBAR,.width=180,.height=24,
         .flags=OS_CONTROL_INHERIT_STYLE,.value=5000},
        {.identifier=17,.parent=0,.kind=OS_CONTROL_CONTAINER,.width=OS_CONTROL_FILL,.height=32,
         .layout=OS_CONTROL_ROW,.gap=16,.flags=OS_CONTROL_INHERIT_STYLE},
        {.identifier=18,.parent=14,.kind=OS_CONTROL_LIST_ITEM,.width=160,.height=32,
         .flags=OS_CONTROL_INHERIT_STYLE,.padding=8,.text_offset=63,.text_length=4},
        {.identifier=19,.parent=14,.kind=OS_CONTROL_TAB,.width=160,.height=32,
         .flags=OS_CONTROL_INHERIT_STYLE,.text_offset=67,.text_length=3},
        {.identifier=20,.parent=0,.kind=OS_CONTROL_SEPARATOR,.width=OS_CONTROL_FILL,.height=2,
         .flags=OS_CONTROL_INHERIT_STYLE},
        {.identifier=6,.parent=0,.kind=OS_CONTROL_CONTAINER,.width=OS_CONTROL_FILL,
         .height=OS_CONTROL_FILL,.flags=OS_CONTROL_CLIP_CHILDREN,.background=0x333333},
        {.identifier=8,.parent=18,.kind=OS_CONTROL_SURFACE,.width=OS_CONTROL_FILL,
         .height=OS_CONTROL_FILL,.surface=1},
    };
    os_window_control_tree_t request = {.hdr={.size=sizeof(request),.version=1},
        .identifier=window.identifier,.count=sizeof(nodes)/sizeof(nodes[0]),
        .nodes=(uintptr_t)nodes,.text=(uintptr_t)text,.text_size=sizeof(text)-1};
    if (call(OS_SYSCALL_NATIVE_FLAG|OS_SYS_WINDOW_CONTROL_TREE,(uintptr_t)&request,0,0)<0) return 2;
    /* 无效更新必须失败，屏幕仍保留上一次成功发布的控件树。 */
    nodes[3].parent=3;
    if (call(OS_SYSCALL_NATIVE_FLAG|OS_SYS_WINDOW_CONTROL_TREE,(uintptr_t)&request,0,0)>=0) return 3;
    nodes[3].parent=2;
    bool repaint=true;
    for (;;) {
        bool idle=false;
        /* 先连续消费输入，再等待；移动、悬停和焦点事件不能各自引入一帧延迟。 */
        for(unsigned batch=0;batch<64U;++batch) {
        os_window_event_t event;
        int64_t bytes=call(LINUX_SYS_READ,fd,(uintptr_t)&event,sizeof(event));
        if (bytes==(int64_t)sizeof(event)) {
            if (event.type==OS_WINDOW_EVENT_CLOSE_REQUEST) goto finished;
            if(event.type==OS_WINDOW_EVENT_CONTROL &&
                !os_control_event_node(&request,nodes,&event.control))continue;
            if (event.type==OS_WINDOW_EVENT_CONTROL &&
                (event.control.action==OS_CONTROL_CHANGE ||
                 (event.control.action==OS_CONTROL_CLICK &&
                  (event.control.target==18 || event.control.target==19)))) {
                {
                    unsigned i=event.control.node_index;
                    if(os_control_is_choice(nodes[i].kind)) {
                        nodes[i].flags &= ~OS_CONTROL_CHECKED;
                        if(event.control.value)nodes[i].flags |= OS_CONTROL_CHECKED;
                    } else if(os_control_is_range(nodes[i].kind)) {
                        nodes[i].value=(uint32_t)event.control.value;
                        nodes[11].value=nodes[i].value;
                    } else nodes[i].flags ^= OS_CONTROL_CHECKED;
                    if(call(OS_SYSCALL_NATIVE_FLAG|OS_SYS_WINDOW_CONTROL_TREE,(uintptr_t)&request,0,0)<0)return 4;
                }
            }
            if (event.type==OS_WINDOW_EVENT_CONTROL &&
                (event.control.target==4 || event.control.target==7) &&
                event.control.action==OS_CONTROL_CLICK) {
                /* Reset 更新 Surface，不重建树；按钮、文字与命中仍由服务器负责。 */
                if (event.control.target==7) {
                    ++patches;
                    repaint=true;
                } else {
                    ++clicks;
                    text[37]='D';text[38]='o';text[39]='n';text[40]='e';text[41]=(char)('0'+clicks%10);
                    if (call(OS_SYSCALL_NATIVE_FLAG|OS_SYS_WINDOW_CONTROL_TREE,(uintptr_t)&request,0,0)<0) return 4;
                }
            }
            if (event.type==OS_WINDOW_EVENT_RESIZE) {
                repaint=true;
            }
        } else if(bytes==-11) {
            idle=true;
            break;
        } else if(bytes!=-4) return 5;
        }
        if(repaint){
            int result=demo_repaint(fd,&window,patches);
            if(result<0)return 6;
            repaint=result!=0;
        }
        if(idle){
            /* 非阻塞读取耗尽后等待窗口事件；只有尺寸竞态还需按短超时重试绘制。 */
            struct {int32_t descriptor;int16_t events,revents;} wait={
                .descriptor=(int32_t)fd,.events=LINUX_POLLIN};
            _Static_assert(sizeof(wait)==8U,"poll ABI");
            int64_t ready=call(LINUX_SYS_POLL,(uintptr_t)&wait,1,repaint?16:UINT64_MAX);
            if((ready<0&&ready!=-4)||
                (wait.revents&(LINUX_POLLERR|LINUX_POLLHUP|LINUX_POLLNVAL)))return 5;
        }
    }
finished:
    (void)call(LINUX_SYS_CLOSE,fd,0,0);
    return 0;
}
__attribute__((naked,noreturn)) void _start(void) {
    __asm__ volatile("andq $-16, %rsp\ncall demo_main\nmovl %eax, %edi\nmovl $231, %eax\nsyscall\nud2");
}
