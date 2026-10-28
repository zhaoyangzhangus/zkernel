#pragma once
#include <uapi/memory.h>

/* 系统信息通过现有只读接口采样，绘制仍交给统一控件树。 */
static os_memory_info_t g_system_memory;
static os_display_info_t g_system_display;
static os_task_info_t g_system_tasks;
static struct utsname g_system_name;
static bool g_memory_valid,g_display_valid,g_tasks_valid,g_name_valid;
static char g_processor_name[49];
#define SETTINGS_GPU_CAPACITY 4U
static os_gpu_info_t g_system_gpus[SETTINGS_GPU_CAPACITY];
static uint32_t g_system_gpu_count;
static bool g_gpus_valid;

static void settings_gpu_refresh(void) {
    g_system_gpu_count=0;g_gpus_valid=false;
    for (uint32_t i=0;i<SETTINGS_GPU_CAPACITY;++i) {
        os_gpu_info_t info={.hdr={sizeof(info),OS_SYSCALL_ABI_VERSION},.index=i};
        int64_t status=settings_syscall_one(OS_SYS_GPU_GET_INFO,(uintptr_t)&info);
        if (status) {if (status==-2 && !i) g_gpus_valid=true;break;}
        if (info.index!=i || info.count<=i || info.vendor_id>65535U || info.device_id>65535U) break;
        g_system_gpus[g_system_gpu_count++]=info;g_gpus_valid=true;
        if (i+1U>=info.count) break;
    }
}


static void settings_processor_name(void) {
    if (g_processor_name[0]) return;
    uint32_t a,b,c,d;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(0x80000000U),"c"(0U));
    if (a>=0x80000004U) {
        for (uint32_t i=0;i<3U;++i) {
            __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(0x80000002U+i),"c"(0U));
            uint32_t words[4]={a,b,c,d};
            memcpy(g_processor_name+16U*i,words,sizeof(words));
        }
        uint32_t begin=0,end=48;
        while (begin<end && g_processor_name[begin]==' ') ++begin;
        while (end>begin && (g_processor_name[end-1U]==' ' || !g_processor_name[end-1U])) --end;
        memmove(g_processor_name,g_processor_name+begin,end-begin);
        g_processor_name[end-begin]=0;
    }
    if (!g_processor_name[0]) memcpy(g_processor_name,"x86_64",7U);
}

static void settings_system_refresh(void) {
    g_system_memory=(os_memory_info_t){.hdr={sizeof(g_system_memory),OS_SYSCALL_ABI_VERSION}};
    g_system_display=(os_display_info_t){.hdr={sizeof(g_system_display),OS_SYSCALL_ABI_VERSION}};
    g_system_tasks=(os_task_info_t){.hdr={sizeof(g_system_tasks),OS_SYSCALL_ABI_VERSION},.pid=OS_KERNEL_WORKERS_PID};
    g_memory_valid=settings_syscall_one(OS_SYS_MEMORY_INFO,(uintptr_t)&g_system_memory)==0;
    g_display_valid=settings_syscall_one(OS_SYS_DISPLAY_GET_INFO,(uintptr_t)&g_system_display)==0;
    g_tasks_valid=settings_syscall_one(OS_SYS_TASK_QUERY,(uintptr_t)&g_system_tasks)==0;
    if (!g_name_valid) g_name_valid=uname(&g_system_name)==0;
    settings_processor_name();
    settings_gpu_refresh();
}

static void settings_memory_value(uint32_t card,const char *label,uint64_t pages) {
    char value[64]={0};
    /* 先验证乘法，异常数据不能绕回后显示成正常的小容量。 */
    if (!g_memory_valid || !g_system_memory.page_size || pages>UINT64_MAX/g_system_memory.page_size)
        append_text(value,sizeof(value),"不可用");
    else {
        append_decimal(value,sizeof(value),pages*g_system_memory.page_size/(1024U*1024U));
        append_text(value,sizeof(value)," MiB");
    }
    settings_value(card,label,value);
}

static void settings_system_content(uint32_t viewport) {
    uint32_t line=ui_text_line_height(),gap=settings_scale(12U);
    uint32_t gpu_cards=g_system_gpu_count?g_system_gpu_count:1U;
    uint32_t gpu_rows=g_system_gpu_count?5U:2U;
    uint32_t total=(12U+gpu_cards*gpu_rows)*line+settings_scale(128U+gpu_cards*32U)+(3U+gpu_cards)*gap;
    uint32_t reserved=settings_scale(36U);
    uint32_t visible=g_window.height>reserved ? g_window.height-reserved : 0U;
    g_scroll_limit=total>visible ? total-visible : 0U;
    if (g_scroll>g_scroll_limit) g_scroll=g_scroll_limit;
    g_nodes[viewport].scroll_y=(int32_t)g_scroll;
    uint32_t body=settings_box(viewport,OS_CONTROL_CONTAINER,OS_CONTROL_COLUMN,
        OS_CONTROL_FILL,(int32_t)total,SETTINGS_CONTENT_BACKGROUND);
    g_nodes[body].gap=gap;
    uint32_t card=settings_card(body,3U);
    settings_label(card,"系统",SETTINGS_SECTION_TEXT);
    char value[128]={0};
    if (g_name_valid) {
        append_text(value,sizeof(value),g_system_name.sysname);
        append_text(value,sizeof(value)," ");append_text(value,sizeof(value),g_system_name.release);
    } else append_text(value,sizeof(value),"不可用");
    settings_value(card,"版本  ",value);
    settings_value(card,"架构  ",g_name_valid ? g_system_name.machine : "x86_64");
    card=settings_card(body,3U);
    settings_label(card,"处理器",SETTINGS_SECTION_TEXT);
    settings_label(card,g_processor_name,SETTINGS_VALUE_TEXT);
    value[0]=0;
    if (g_tasks_valid) append_decimal(value,sizeof(value),g_system_tasks.cpu_count);
    else append_text(value,sizeof(value),"不可用");
    settings_value(card,"在线逻辑处理器  ",value);
    card=settings_card(body,4U);
    settings_label(card,"物理内存",SETTINGS_SECTION_TEXT);
    settings_memory_value(card,"总计  ",g_system_memory.total_pages);
    bool consistent=g_system_memory.free_pages<=g_system_memory.total_pages;
    if (consistent) settings_memory_value(card,"已用  ",g_system_memory.total_pages-g_system_memory.free_pages);
    else settings_value(card,"已用  ","不可用");
    settings_memory_value(card,"可用  ",g_system_memory.free_pages);
    card=settings_card(body,2U);
    settings_label(card,"显示",SETTINGS_SECTION_TEXT);
    value[0]=0;
    if (g_display_valid) {
        append_decimal(value,sizeof(value),g_system_display.width);
        append_text(value,sizeof(value)," × ");append_decimal(value,sizeof(value),g_system_display.height);
    } else append_text(value,sizeof(value),"不可用");
    settings_value(card,"分辨率  ",value);
    for (uint32_t i=0;i<gpu_cards;++i) {
        card=settings_card(body,gpu_rows);settings_label(card,"显卡",SETTINGS_SECTION_TEXT);
        if (!g_system_gpu_count) {
            settings_label(card,g_gpus_valid?"未检测到显卡":"信息不可用",SETTINGS_HINT_TEXT);
            break;
        }
        const os_gpu_info_t *gpu=g_system_gpus+i;
        const char *name=gpu->vendor_id==0x10deU && gpu->device_id==0x2f04U ? "NVIDIA GeForce RTX 5070" :
            gpu->vendor_id==0x10deU ? "NVIDIA 显卡" : gpu->vendor_id==0x1002U ? "AMD Radeon 显卡" :
            gpu->vendor_id==0x8086U ? "Intel 显卡" : gpu->vendor_id==0x1234U ? "虚拟显卡" : "显示适配器";
        settings_label(card,name,SETTINGS_VALUE_TEXT);
        static const char digits[]="0123456789ABCDEF";
        char pci[10];
        for(uint32_t k=0;k<4U;++k) {pci[k]=digits[(gpu->vendor_id>>(12U-4U*k))&15U];pci[5U+k]=digits[(gpu->device_id>>(12U-4U*k))&15U];}
        pci[4]=':';pci[9]=0;settings_value(card,"PCI 标识  ",pci);
        settings_value(card,"设备状态  ",gpu->state==OS_GPU_DEVICE_FAILED?"失败":gpu->state==OS_GPU_DEVICE_REMOVED?"已移除":
            gpu->state==OS_GPU_DEVICE_ACTIVE?"活动":gpu->state==OS_GPU_DEVICE_SUSPENDED?"挂起":gpu->state==OS_GPU_DEVICE_RECOVERING?"恢复中":"已识别");
        char capacity[40]="";
        if (gpu->vram_bytes) {
            append_decimal(capacity,sizeof(capacity),gpu->vram_bytes/(1024U*1024U));
            append_text(capacity,sizeof(capacity)," MiB");
        } else append_text(capacity,sizeof(capacity),"不可用");
        settings_value(card,"显存容量  ",capacity);
    }
}
