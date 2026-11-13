/* 独立运行的 Linux personality 边框切换运行时回归测试。 */
#include <stdint.h>
#include <uapi/linux_abi.h>
#include <uapi/window.h>
#include <uapi/window_state.h>

static int64_t call(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    int64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return result;
}
static void message(const char *s) {
    unsigned n = 0; while (s[n]) ++n;
    (void)call(LINUX_SYS_WRITE, 1, (uintptr_t)s, n);
}
static uint64_t now(void) {
    uint64_t t[2] = {0};
    if (call(LINUX_SYS_CLOCK_GETTIME, 1, (uintptr_t)t, 0) < 0) return 0;
    return t[0] * 1000000000ULL + t[1];
}
static int64_t command(int64_t fd, linux_window_command_t *c) {
    return call(LINUX_SYS_IOCTL, fd, LINUX_WINDOW_IOCTL_COMMIT, (uintptr_t)c);
}
static int draw(int64_t fd, linux_window_create_t *w, unsigned phase) {
    if (!w->width || !w->height || (uint64_t)w->width * w->height > w->buffer_size / 4) return 1;
    uint32_t *p = (void *)(uintptr_t)w->address;
    for (uint32_t y = 0; y < w->height; ++y)
        for (uint32_t x = 0; x < w->width; ++x)
            p[(uint64_t)y * w->width + x] = phase == 1 ? 0x0056AD86 : 0x003671BC;
    linux_window_command_t c = {.operation=LINUX_WINDOW_OPERATION_PRESENT,
        .address=w->address,.length=(uint64_t)w->width*w->height*4,
        .stride=w->width*4,.width=w->width,.height=w->height};
    return command(fd,&c) < 0;
}
__attribute__((used)) static int probe_main(uint64_t argc) {
    linux_window_create_t w = {.x=400,.y=250,.width=640,.height=360,
        .flags=OS_WINDOW_VISIBLE|OS_WINDOW_RESIZABLE|OS_WINDOW_STANDARD_CAPTION_CONTROLS,
        .title="FRAME SWITCH PROBE"};
    int64_t fd=call(LINUX_SYS_WINDOW_CREATE,(uintptr_t)&w,0,0);
    if(fd<0 || draw(fd,&w,0)){message("FRAME_PROBE_CREATE_FAILED\n");return 1;}
    char title[32]="COMPOSITOR FRAME";
    linux_window_command_t c={.operation=LINUX_WINDOW_OPERATION_TITLE,
        .address=(uintptr_t)title,.length=32};
    if(command(fd,&c)<0){message("FRAME_PROBE_TITLE_FAILED\n");return 2;}
    message("FRAME_PROBE_SERVER\n");
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_GEOMETRY};
    if(command(fd,&c)<0 || c.x!=401 || c.y!=291 || c.width!=640 || c.height!=360 || c.stride!=0) {
        message("FRAME_PROBE_QUERY_FAILED\n");return 7;
    }
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_FULLSCREEN,.width=1};
    if(command(fd,&c)<0)return 14;
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_GEOMETRY};
    if(command(fd,&c)<0 || c.stride!=2 || c.x || c.y || c.width!=1920 || c.height!=1080)return 15;
    w.width=c.width;w.height=c.height;if(draw(fd,&w,0))return 16;
    if(argc==1){uint64_t sleep[2]={2,0};(void)call(LINUX_SYS_NANOSLEEP,(uintptr_t)sleep,0,0);}
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_FULLSCREEN};
    if(command(fd,&c)<0)return 17;
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_GEOMETRY};
    if(command(fd,&c)<0 || c.stride || c.x!=401 || c.y!=291 || c.width!=640 || c.height!=360)return 18;
    w.width=c.width;w.height=c.height;
    message("FRAME_PROBE_FULLSCREEN_PASS\n");
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_MAXIMIZE,.width=1};
    if(command(fd,&c)<0)return 8;
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_GEOMETRY};
    if(command(fd,&c)<0 || !c.stride || c.x!=1 || c.y!=41 || c.width!=1918 || c.height!=1038) {
        message("FRAME_PROBE_MAXIMIZE_FAILED\n");return 8;
    }
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_FULLSCREEN,.width=1};
    if(command(fd,&c)<0)return 19;
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_GEOMETRY};
    if(command(fd,&c)<0 || c.stride!=3 || c.x || c.y || c.width!=1920 || c.height!=1080)return 20;
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_FULLSCREEN};
    if(command(fd,&c)<0)return 21;
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_GEOMETRY};
    if(command(fd,&c)<0 || c.stride!=1 || c.x!=1 || c.y!=41 || c.width!=1918 || c.height!=1038)return 22;
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_MAXIMIZE};
    if(command(fd,&c)<0)return 9;
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_GEOMETRY};
    if(command(fd,&c)<0 || c.stride || c.x!=401 || c.y!=291 || c.width!=640 || c.height!=360) {
        message("FRAME_PROBE_RESTORE_FAILED\n");return 9;
    }
    os_window_event_t pending;
    while(call(LINUX_SYS_READ,fd,(uintptr_t)&pending,sizeof(pending))==(int64_t)sizeof(pending)) {}
    if(draw(fd,&w,0))return 10;
    message("FRAME_PROBE_MAXIMIZE_PASS\n");
    if (argc > 1) {
        unsigned moves=0;
        int32_t last_x=401,last_y=291;
        uint64_t start=now();if(!start)return 3;
        message("FRAME_PROBE_DRAG_READY\n");
        while(now()-start < 8000000000ULL) {
            os_window_event_t event;
            int64_t r=call(LINUX_SYS_READ,fd,(uintptr_t)&event,sizeof(event));
            if(r==(int64_t)sizeof(event)) {
                if(event.type==OS_WINDOW_EVENT_RESIZE)return 11;
                if(event.type==OS_WINDOW_EVENT_MOVED) {
                    if(event.moved.reserved || event.moved.reserved2)return 12;
                    last_x=event.moved.x;last_y=event.moved.y;++moves;
                }
            }
            uint64_t sleep[2]={0,1000000};
            (void)call(LINUX_SYS_NANOSLEEP,(uintptr_t)sleep,0,0);
        }
        c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_GEOMETRY};
        if(command(fd,&c)<0 || !moves || (last_x==401&&last_y==291) ||
           c.x!=last_x || c.y!=last_y || c.width!=640 || c.height!=360 || c.stride)return 13;
        c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_CLOSE};
        if(command(fd,&c)<0)return 6;
        (void)call(LINUX_SYS_CLOSE,fd,0,0);
        message("FRAME_PROBE_DRAG_PASS\n");return 0;
    }
    unsigned phase=0, events=0;
    uint64_t start=now(); if(!start)return 3;
    while(now()-start < 9000000000ULL) {
        uint64_t elapsed=now()-start;
        if((phase==0 && elapsed>=3000000000ULL)||(phase==1 && elapsed>=6000000000ULL)) {
            ++phase;
            c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_DECORATIONS,.width=phase==2};
            if(command(fd,&c)<0){message("FRAME_PROBE_SWITCH_FAILED\n");return 4;}
        }
        os_window_event_t event;
        int64_t r=call(LINUX_SYS_READ,fd,(uintptr_t)&event,sizeof(event));
        if(r==(int64_t)sizeof(event)&&event.type==OS_WINDOW_EVENT_RESIZE){
            w.width=event.resize.width;w.height=event.resize.height;++events;
            c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_GEOMETRY};
            if(command(fd,&c)<0 || c.x!=(phase==1?400:401) || c.y!=(phase==1?250:291) ||
               c.width!=w.width || c.height!=w.height || c.stride!=0) {
                message("FRAME_PROBE_QUERY_FAILED\n");return 7;
            }
            if((phase==1&&(w.width!=642||w.height!=402)) ||
               (phase==2&&(w.width!=640||w.height!=360)) || draw(fd,&w,phase)) {
                message("FRAME_PROBE_GEOMETRY_FAILED\n");return 5;
            }
            message(phase==1?"FRAME_PROBE_CLIENT\n":"FRAME_PROBE_RESTORED\n");
        }
        uint64_t sleep[2]={0,10000000};
        (void)call(LINUX_SYS_NANOSLEEP,(uintptr_t)sleep,0,0);
    }
    c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_CLOSE};
    if(command(fd,&c)<0 || events!=2){message("FRAME_PROBE_CLOSE_FAILED\n");return 6;}
    (void)call(LINUX_SYS_CLOSE,fd,0,0);
    message("FRAME_PROBE_PASS\n");return 0;
}
__attribute__((naked,noreturn)) void _start(void) {
    __asm__ volatile("movq (%rsp), %rdi\nandq $-16, %rsp\ncall probe_main\nmovl %eax, %edi\nmovl $231, %eax\nsyscall\nud2");
}
