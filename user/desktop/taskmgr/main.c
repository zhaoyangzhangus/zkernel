#include <stdbool.h>
#include <stdint.h>

#include <uapi/all.h>

#include <uapi/window_theme.h>
#include "../../runtime/caption.h"
#include "../../runtime/caption_tree.h"
#include "../../runtime/text.h"

#define TASKMGR_MAP_BASE       0x0E000000ULL
#define TASKMGR_REFRESH_NS 1000000000ULL
#define TASKMGR_MAX_PROCESSES   256U
#define TASKMGR_MAX_THREADS     512U
#define TASKMGR_MAX_ENTRIES     (TASKMGR_MAX_PROCESSES + TASKMGR_MAX_THREADS)
#define TASKMGR_ROW_HEIGHT      (ui_text_line_height() + taskmgr_scale(14U))
#define TASKMGR_TABLE_TOP       TASKMGR_ROW_HEIGHT
#define TASKMGR_STATUS_CAP      96U

#define TASKMGR_BACKGROUND      OS_THEME_SURFACE
#define TASKMGR_TABLE_HEADER    OS_THEME_TOOLBAR
#define TASKMGR_TEXT            OS_THEME_TEXT
#define TASKMGR_MUTED           OS_THEME_MUTED
#define TASKMGR_SELECTED        OS_THEME_SELECTED

typedef struct taskmgr_window {
    os_handle_t handle;
    uint32_t identifier;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} taskmgr_window_t;

typedef struct taskmgr_entry {
    bool thread;
    uint64_t id;
    uint64_t parent_id;
    uint32_t state;
    uint32_t cpu;
    uint32_t thread_count;
    bool expanded;
    bool cpu_sample_valid;
    os_task_info_t resources;
    char name[OS_PROCESS_NAME_MAX];
} taskmgr_entry_t;

static taskmgr_window_t g_window = {
    OS_INVALID_HANDLE, 0U, 0U, 0U, 0,
};
static taskmgr_entry_t g_entries[TASKMGR_MAX_ENTRIES];
static uint32_t g_entry_count;
static uint32_t g_visible_entries[TASKMGR_MAX_ENTRIES];
static uint32_t g_entry_rows[TASKMGR_MAX_ENTRIES];
static uint32_t g_visible_count;
static bool g_visible_dirty = true;
static uint32_t g_first_row;
static uint32_t g_selected_entry;
static bool g_selection_valid;
static bool g_entries_truncated;
static bool g_ctrl;
/* 上轮只保留匹配身份、累计 CPU 时间和展开状态，不复制名称及整份资源统计。 */
typedef struct taskmgr_previous {
    uint64_t id,parent_id,created,runtime;
    bool thread,expanded;
} taskmgr_previous_t;
static taskmgr_previous_t g_previous[TASKMGR_MAX_ENTRIES];
static uint16_t g_previous_index[1024];
static uint32_t g_previous_count;
static uint64_t g_sample_time;
static uint64_t g_next_refresh;
static os_cpu_usage_t g_usage_previous;
static os_memory_info_t g_memory;
static uint32_t g_total_cpu;
static bool g_usage_baseline,g_usage_valid,g_total_cpu_valid,g_memory_valid;
static uint64_t g_confirm_pid;
static uint64_t g_confirm_created;
static uint64_t g_confirm_tid;
static bool g_menu_open;
static uint32_t g_menu_x, g_menu_y;
static uint32_t g_menu_generation;
static bool g_menu_published;
static os_control_node_t g_nodes[OS_CONTROL_TREE_MAX_NODES];
static char g_tree_text[OS_CONTROL_TREE_MAX_TEXT];
static os_window_control_tree_t g_tree;
static uint32_t g_caption_height;
static void dismiss_menu(void);
static char g_status[TASKMGR_STATUS_CAP] =
    "R REFRESH  CTRL +/- FONT  ESC CLOSE";

void __main(void) {
}

static uint32_t taskmgr_scale(uint32_t value) {
    return (value * ui_text_size() + OS_TEXT_DEFAULT_SIZE / 2U) /
           OS_TEXT_DEFAULT_SIZE;
}

static int64_t taskmgr_syscall6(uint64_t number, uint64_t arg0,
                                uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4,
                                uint64_t arg5) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = arg3;
    register uint64_t r8 __asm__("r8") = arg4;
    register uint64_t r9 __asm__("r9") = arg5;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t taskmgr_syscall_one(uint64_t number, uint64_t arg0) {
    return taskmgr_syscall6(number, arg0, 0U, 0U, 0U, 0U, 0U);
}

__attribute__((noreturn)) static void taskmgr_exit(uint64_t status) {
    (void)taskmgr_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static uint32_t text_length(const char *text) {
    uint32_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0' && length + 1U < TASKMGR_STATUS_CAP) ++length;
    return length;
}

static void copy_text(char *destination, uint32_t capacity,
                      const char *source) {
    uint32_t index = 0U;
    if (destination == 0 || capacity == 0U) return;
    if (source != 0) {
        while (index + 1U < capacity && source[index] != '\0') {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = '\0';
}

static void append_text(char *destination, uint32_t capacity,
                        const char *source) {
    uint32_t length = text_length(destination);
    uint32_t index = 0U;
    if (destination == 0 || source == 0 || capacity == 0U) return;
    while (length + 1U < capacity && source[index] != '\0') {
        destination[length++] = source[index++];
    }
    destination[length] = '\0';
}

static void append_u64(char *destination, uint32_t capacity, uint64_t value) {
    char digits[21];
    uint32_t begin = sizeof(digits) - 1U;
    digits[begin] = '\0';
    /* 从末尾生成完整数字，只定位一次目标末尾，避免每输出一位都重新扫描。 */
    do {
        digits[--begin] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    append_text(destination, capacity, digits + begin);
}

static void set_status(const char *text) {
    copy_text(g_status, sizeof(g_status), text);
}

static uint64_t monotonic_ns(void) {
    os_timespec_t time = {0};
    if (taskmgr_syscall6(OS_SYS_CLOCK_GET, OS_CLOCK_MONOTONIC,
            (uint64_t)(uintptr_t)&time, 0U, 0U, 0U, 0U) < 0) return 0U;
    return (uint64_t)time.seconds * 1000000000ULL + (uint64_t)time.nanoseconds;
}

/* 正常的一秒采样走一次乘除；极大计数使用有界长除法避免溢出。 */
static uint32_t taskmgr_cpu_percent(uint64_t busy,uint64_t total) {
    if (!total) return 0U;
    if (busy>=total) return 100000U;
    if (busy<=UINT64_MAX/100000U) return (uint32_t)(busy*100000U/total);
    uint64_t remainder=0;uint32_t result=0;
    for (uint32_t bit=1U<<16;bit;bit>>=1U) {
        result*=2U;
        if (remainder>=total-remainder) {remainder-=total-remainder;++result;}
        else remainder*=2U;
        if (100000U&bit) {
            if (remainder>=total-busy) {remainder-=total-busy;++result;}
            else remainder+=busy;
        }
    }
    return result;
}

static void taskmgr_system_sample(void) {
    g_memory=(os_memory_info_t){.hdr={sizeof(g_memory),OS_SYSCALL_ABI_VERSION}};
    g_memory_valid=taskmgr_syscall_one(OS_SYS_MEMORY_INFO,(uintptr_t)&g_memory)==0 &&
        g_memory.page_size && g_memory.free_pages<=g_memory.total_pages &&
        g_memory.total_pages<=UINT64_MAX/g_memory.page_size;
    os_cpu_usage_t usage={.hdr={sizeof(usage),OS_SYSCALL_ABI_VERSION}};
    g_usage_valid=taskmgr_syscall_one(OS_SYS_CPU_GET_USAGE,(uintptr_t)&usage)==0 && usage.cpu_count;
    g_total_cpu_valid=false;
    if (g_usage_valid && g_usage_baseline && usage.cpu_count==g_usage_previous.cpu_count &&
        usage.busy_ns>=g_usage_previous.busy_ns && usage.idle_ns>=g_usage_previous.idle_ns) {
        uint64_t busy=usage.busy_ns-g_usage_previous.busy_ns,idle=usage.idle_ns-g_usage_previous.idle_ns;
        if (busy<=UINT64_MAX-idle && busy+idle) {
            g_total_cpu=taskmgr_cpu_percent(busy,busy+idle);g_total_cpu_valid=true;
        }
    }
    g_usage_baseline=g_usage_valid;g_usage_previous=usage;
}

static void taskmgr_summary(char *text,uint32_t capacity) {
    text[0]=0;append_text(text,capacity,"CPU 总占用：");
    if (g_total_cpu_valid) {
        append_u64(text,capacity,g_total_cpu/1000U);append_text(text,capacity,".");
        char fraction[5]={(char)('0'+g_total_cpu/100U%10U),(char)('0'+g_total_cpu/10U%10U),
            (char)('0'+g_total_cpu%10U),'%',0};append_text(text,capacity,fraction);
    } else append_text(text,capacity,g_usage_valid?"采样中":"不可用");
    append_text(text,capacity,"    空闲内存：");
    if (g_memory_valid) {
        append_u64(text,capacity,g_memory.free_pages*g_memory.page_size/(1024U*1024U));
        append_text(text,capacity," MiB    已使用内存：");
        append_u64(text,capacity,(g_memory.total_pages-g_memory.free_pages)*g_memory.page_size/(1024U*1024U));
        append_text(text,capacity," MiB");
    } else append_text(text,capacity,"不可用    已使用内存：不可用");
}

static uint32_t previous_hash(uint64_t id,uint64_t parent,uint64_t created,bool thread) {
    uint64_t hash=id^(parent*0x9e3779b97f4a7c15ULL)^created^(uint64_t)thread;
    hash=(hash^(hash>>30U))*0xbf58476d1ce4e5b9ULL;
    hash=(hash^(hash>>27U))*0x94d049bb133111ebULL;
    return (uint32_t)(hash^(hash>>31U))&1023U;
}

static void previous_snapshot(void) {
    _Static_assert(TASKMGR_MAX_ENTRIES<1024U,"采样索引必须保留空槽");
    for (uint32_t i=0;i<1024U;++i) g_previous_index[i]=0U;
    g_previous_count=g_entry_count;
    for (uint32_t i=0;i<g_previous_count;++i) {
        const taskmgr_entry_t *entry=g_entries+i;
        taskmgr_previous_t *old=g_previous+i;
        *old=(taskmgr_previous_t){.id=entry->id,.parent_id=entry->parent_id,
            .created=entry->resources.create_time_ns,.runtime=entry->resources.runtime_ns,
            .thread=entry->thread,.expanded=entry->expanded};
        uint32_t slot=previous_hash(old->id,old->parent_id,old->created,old->thread);
        while (g_previous_index[slot]) slot=(slot+1U)&1023U;
        g_previous_index[slot]=(uint16_t)(i+1U);
    }
}

static const taskmgr_previous_t *previous_find(const taskmgr_entry_t *entry) {
    uint64_t created=entry->resources.create_time_ns;
    uint32_t slot=previous_hash(entry->id,entry->parent_id,created,entry->thread);
    while (g_previous_index[slot]) {
        const taskmgr_previous_t *old=g_previous+g_previous_index[slot]-1U;
        if (old->id==entry->id && old->parent_id==entry->parent_id &&
            old->created==created && old->thread==entry->thread) return old;
        slot=(slot+1U)&1023U;
    }
    return 0;
}

static void sample_resources(taskmgr_entry_t *entry, uint64_t now) {
    entry->resources = (os_task_info_t){0};
    entry->resources.hdr.size = sizeof(entry->resources);
    entry->resources.hdr.version = OS_SYSCALL_ABI_VERSION;
    entry->resources.pid = entry->thread ? entry->parent_id : entry->id;
    entry->resources.tid = entry->thread ? entry->id : 0U;
    entry->cpu = 0U;
    entry->cpu_sample_valid = false;
    entry->expanded = !entry->thread && entry->id == OS_KERNEL_WORKERS_PID;
    if (taskmgr_syscall_one(OS_SYS_TASK_QUERY,
            (uint64_t)(uintptr_t)&entry->resources) < 0) {
        entry->resources.flags = OS_TASK_PROTECTED;
        return;
    }
    const taskmgr_previous_t *old=previous_find(entry);
    if (old) {
        entry->expanded = old->expanded;
        if (now > g_sample_time && entry->resources.runtime_ns >= old->runtime) {
            uint64_t elapsed = now - g_sample_time;
            uint32_t cpus = entry->resources.cpu_count;
            if (!cpus) cpus = 1U;
            uint64_t delta = entry->resources.runtime_ns - old->runtime;
            /* 保留三位小数，使较低但实际存在的 CPU 使用率仍可见。 */
            uint64_t thousandths = delta <= UINT64_MAX / 100000U ?
                delta * 100000U / elapsed / cpus : delta / elapsed * 100000U / cpus;
            entry->cpu = thousandths > 100000U ? 100000U : (uint32_t)thousandths;
            entry->cpu_sample_valid = true;
        }
    }
}

static void visible_index_rebuild(void) {
    if (!g_visible_dirty) return;
    g_visible_count = 0U;
    for (uint32_t i = 0U; i < g_entry_count; ++i) g_entry_rows[i] = UINT32_MAX;
    /* 父进程只建一次索引，倒序串起子线程，输出仍保留原枚举顺序。
     * 使用下标加一表示链表，零表示空，不分配内存或搬动统计快照。 */
    _Static_assert(TASKMGR_MAX_ENTRIES<1024U,"父进程索引必须保留空槽并容纳所有条目下标");
    uint16_t parents[1024]={0},heads[TASKMGR_MAX_ENTRIES]={0};
    uint16_t next[TASKMGR_MAX_ENTRIES];
    for (uint32_t i=0U;i<g_entry_count;++i) {
        if (g_entries[i].thread || !g_entries[i].expanded) continue;
        uint64_t id=g_entries[i].id;
        uint32_t slot=(uint32_t)((id^(id>>32U))*2654435761U)&1023U;
        while (parents[slot] && g_entries[parents[slot]-1U].id!=id) slot=(slot+1U)&1023U;
        if (!parents[slot]) parents[slot]=(uint16_t)(i+1U);
    }
    for (uint32_t i=g_entry_count;i>0U;--i) {
        uint32_t child=i-1U;
        if (!g_entries[child].thread) continue;
        uint64_t id=g_entries[child].parent_id;
        uint32_t slot=(uint32_t)((id^(id>>32U))*2654435761U)&1023U;
        while (parents[slot] && g_entries[parents[slot]-1U].id!=id) slot=(slot+1U)&1023U;
        if (!parents[slot]) continue;
        uint32_t parent=parents[slot]-1U;
        next[child]=heads[parent];heads[parent]=(uint16_t)i;
    }
    for (uint32_t i=0U;i<g_entry_count;++i) {
        if (g_entries[i].thread) continue;
        g_entry_rows[i]=g_visible_count;
        g_visible_entries[g_visible_count++]=i;
        for (uint16_t child=heads[i];child;child=next[child-1U]) {
            uint32_t index=child-1U;
            g_entry_rows[index]=g_visible_count;
            g_visible_entries[g_visible_count++]=index;
        }
    }
    g_visible_dirty = false;
}

static bool entry_visible(uint32_t index) {
    visible_index_rebuild();
    return index < g_entry_count && g_entry_rows[index] != UINT32_MAX;
}

static uint32_t visible_entry(uint32_t row) {
    visible_index_rebuild();
    return row < g_visible_count ? g_visible_entries[row] : g_entry_count;
}

static uint32_t entry_row(uint32_t index) {
    visible_index_rebuild();
    return index < g_entry_count && g_entry_rows[index] != UINT32_MAX ?
        g_entry_rows[index] : 0U;
}

static uint32_t visible_count(void) {
    visible_index_rebuild();
    return g_visible_count;
}

static const char *process_state_text(uint32_t state) {
    if (state == OS_PROCESS_NEW) return "NEW";
    if (state == OS_PROCESS_RUNNING) return "RUNNING";
    if (state == OS_PROCESS_EXITING) return "EXITING";
    if (state == OS_PROCESS_DEAD) return "DEAD";
    return "UNKNOWN";
}

static const char *thread_state_text(uint32_t state) {
    if (state == OS_THREAD_NEW) return "NEW";
    if (state == OS_THREAD_READY) return "READY";
    if (state == OS_THREAD_RUNNING) return "RUNNING";
    if (state == OS_THREAD_BLOCKED) return "BLOCKED";
    if (state == OS_THREAD_STOPPED) return "STOPPED";
    if (state == OS_THREAD_DEAD) return "DEAD";
    return "UNKNOWN";
}

static void refresh_processes(void) {
    taskmgr_system_sample();
    g_visible_dirty = true;
    bool selected_thread = g_selection_valid &&
        g_selected_entry < g_entry_count && g_entries[g_selected_entry].thread;
    uint64_t selected_id = g_selection_valid && g_selected_entry < g_entry_count ?
                           g_entries[g_selected_entry].id : 0U;
    uint64_t selected_created = g_selection_valid && g_selected_entry < g_entry_count ?
        g_entries[g_selected_entry].resources.create_time_ns : 0U;
    previous_snapshot();
    uint64_t now = monotonic_ns();
    g_next_refresh = now + TASKMGR_REFRESH_NS;
    g_entry_count = 0U;
    g_entries_truncated = false;
    for (uint32_t index = 0U; index <= TASKMGR_MAX_PROCESSES; ++index) {
        os_process_enumerate_t request = {0};
        int64_t status;
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.index = index;
        status = taskmgr_syscall_one(OS_SYS_PROCESS_ENUMERATE,
                                     (uint64_t)(uintptr_t)&request);
        if (status == -2) break;
        if (status < 0) {
            g_entry_count = 0U;
            g_selection_valid = false;
            set_status("PROCESS QUERY FAILED");
            return;
        }
        if (index == TASKMGR_MAX_PROCESSES) {
            g_entries_truncated = true;
            break;
        }
        taskmgr_entry_t *entry = &g_entries[g_entry_count++];
        entry->thread = false;
        entry->id = request.info.pid;
        entry->parent_id = request.info.parent_pid;
        entry->state = request.info.state;
        entry->cpu = 0U;
        entry->thread_count = request.info.thread_count;
        copy_text(entry->name, sizeof(entry->name), request.info.name);
        sample_resources(entry, now);
    }
    for (uint32_t index = 0U; index <= TASKMGR_MAX_THREADS; ++index) {
        os_thread_enumerate_t request = {0};
        int64_t status;
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.index = index;
        status = taskmgr_syscall_one(OS_SYS_THREAD_ENUMERATE,
                                     (uint64_t)(uintptr_t)&request);
        if (status == -2) break;
        if (status < 0) {
            g_entry_count = 0U;
            g_selection_valid = false;
            set_status("THREAD QUERY FAILED");
            return;
        }
        if (index == TASKMGR_MAX_THREADS) {
            g_entries_truncated = true;
            break;
        }
        taskmgr_entry_t *entry = &g_entries[g_entry_count++];
        entry->thread = true;
        entry->id = request.info.tid;
        entry->parent_id = request.info.process_pid;
        entry->state = request.info.state;
        entry->cpu = request.info.current_cpu;
        entry->thread_count = 0U;
        copy_text(entry->name, sizeof(entry->name), request.info.name);
        sample_resources(entry, now);
    }
    g_sample_time = now;
    g_next_refresh = now + TASKMGR_REFRESH_NS;
    g_selection_valid = g_entry_count != 0U;
    g_selected_entry = 0U;
    bool restored = false;
    if (selected_id != 0U) {
        for (uint32_t index = 0U; index < g_entry_count; ++index) {
            if (g_entries[index].id == selected_id &&
                g_entries[index].thread == selected_thread &&
                g_entries[index].resources.create_time_ns == selected_created &&
                entry_visible(index)) {
                g_selected_entry = index;
                restored = true;
                break;
            }
        }
    }
    if (selected_id != 0U && !restored) dismiss_menu();
    uint32_t count = visible_count();
    if (g_first_row >= count) g_first_row = count ? count - 1U : 0U;
    if (!g_confirm_pid) set_status(g_entries_truncated ? "LIST LIMIT  AUTO REFRESH 1s" :
        "AUTO REFRESH 1s  |  CPU: MACHINE %  |  MEMORY: RESIDENT MiB");
}

static uint32_t menu_width(void) {
    uint32_t width = ui_text_measure("确认结束进程") + 32U;
    return width < g_window.width ? width : g_window.width;
}

static uint32_t menu_rows(void) {
    return g_selected_entry < g_entry_count && g_entries[g_selected_entry].thread ? 2U : 1U;
}

static void menu_next_generation(void) {
    g_menu_generation = (g_menu_generation + 1U) & 0xfffffU;
    if (!g_menu_generation) g_menu_generation = 1U;
}

static uint32_t menu_target(uint32_t index) {
    return 0x60000000U | (g_menu_generation << 4U) | index;
}

static void dismiss_menu(void) {
    g_menu_open = false;
    g_confirm_pid = 0U;
    g_confirm_tid = 0U;
}

static uint32_t visible_rows(void) {
    uint32_t bottom = g_window.height;
    return bottom > TASKMGR_TABLE_TOP ?
        (bottom - TASKMGR_TABLE_TOP) / TASKMGR_ROW_HEIGHT : 0U;
}

static void ensure_selection_visible(void) {
    uint32_t rows = visible_rows();
    if (!g_selection_valid || rows == 0U) return;
    uint32_t selected_row = entry_row(g_selected_entry);
    if (selected_row < g_first_row) g_first_row = selected_row;
    if (selected_row >= g_first_row + rows) {
        g_first_row = selected_row - rows + 1U;
    }
    if (g_first_row > g_entry_count) g_first_row = g_entry_count;
}

static void select_delta(int32_t delta) {
    int64_t next;
    if (g_entry_count == 0U) return;
    if (!g_selection_valid) {
        g_selection_valid = true;
        g_selected_entry = 0U;
        ensure_selection_visible();
        return;
    }
    next = (int64_t)entry_row(g_selected_entry) + delta;
    if (next < 0) next = 0;
    if (next >= (int64_t)visible_count()) next = (int64_t)visible_count() - 1;
    g_selected_entry = visible_entry((uint32_t)next);
    dismiss_menu();
    ensure_selection_visible();
}

/* 表格只提交可见行；文字、裁剪、选中态和悬停全部由公共控件绘制。 */
static bool tree_text(os_control_node_t *node, uint32_t *used, const char *text) {
    uint32_t length=text_length(text);
    if (length>OS_CONTROL_TREE_MAX_TEXT-*used) return false;
    node->text_offset=*used;node->text_length=length;
    for (uint32_t i=0;i<length;++i) g_tree_text[(*used)++]=text[i];
    return true;
}

static bool table_label(uint32_t *count, uint32_t *used, uint32_t parent,
    uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t color, const char *text) {
    if (*count==OS_CONTROL_TREE_MAX_NODES) return false;
    os_control_node_t *node=g_nodes+(*count)++;
    *node=(os_control_node_t){.identifier=id,.parent=parent,.kind=OS_CONTROL_TEXT,
        .flags=OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_INHERIT_FONT,
        .x=(int32_t)x,.y=(int32_t)y,.width=(int32_t)width,.height=(int32_t)ui_text_size(),
        .foreground=color};
    return tree_text(node,used,text);
}

static bool build_table(uint32_t *count, uint32_t *used) {
    uint32_t row_height=TASKMGR_ROW_HEIGHT;
    uint32_t bottom=g_window.height;
    uint32_t margin=taskmgr_scale(12U), text_y=(row_height-ui_text_size())/2U;
    uint32_t right_margin=taskmgr_scale(16U);
    uint32_t columns[]={taskmgr_scale(44U),g_window.width*36U/100U,g_window.width*48U/100U,
        g_window.width*67U/100U,g_window.width*81U/100U,
        g_window.width>right_margin?g_window.width-right_margin:0U};
    g_nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
        .flags=g_menu_open?OS_CONTROL_MANAGED_FOCUS:0,.background=TASKMGR_BACKGROUND,
        .foreground=TASKMGR_TEXT,.font_size=ui_text_size()};
    g_nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_INHERIT_FONT,.width=OS_CONTROL_FILL,.height=(int32_t)row_height,
        .background=TASKMGR_TABLE_HEADER};
    *count=2;
    const char *headers[]={"名称","ID","状态","CPU","MiB"};
    for (uint32_t c=0;c<5;++c) {
        uint32_t right=columns[c+1]>margin?columns[c+1]-margin:0U;
        if (!table_label(count,used,1,10U+c,columns[c],text_y,
            right>columns[c]?right-columns[c]:0U,TASKMGR_MUTED,headers[c])) return false;
    }
    uint32_t table=(*count)++;
    g_nodes[table]=(os_control_node_t){.identifier=3,.parent=0,.kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_INHERIT_FONT|
            (g_menu_open?OS_CONTROL_INERT:0),.y=(int32_t)TASKMGR_TABLE_TOP,
        .width=OS_CONTROL_FILL,.height=bottom>TASKMGR_TABLE_TOP?(int32_t)(bottom-TASKMGR_TABLE_TOP):0,
        .background=TASKMGR_BACKGROUND};
    for (uint32_t visible=0;visible<visible_rows();++visible) {
        uint32_t row=visible_entry(g_first_row+visible);
        if (row>=g_entry_count) break;
        if (*count+7U+8U>OS_CONTROL_TREE_MAX_NODES) return false;
        const taskmgr_entry_t *entry=g_entries+row;
        uint32_t id=0x10000U+row*8U,y=visible*row_height;
        bool selected=g_selection_valid&&row==g_selected_entry;
        g_nodes[(*count)++]=(os_control_node_t){.identifier=id,.parent=table,.kind=OS_CONTROL_LIST_ITEM,
            .flags=selected?OS_CONTROL_CHECKED:0,.user_data=row,
            .y=(int32_t)y,.width=OS_CONTROL_FILL,.height=(int32_t)row_height,
            .background=TASKMGR_BACKGROUND};
        /* 展开按钮和文字是同一行的兄弟控件，命中信息直接携带条目索引。 */
        g_nodes[(*count)++]=(os_control_node_t){.identifier=id+1U,.parent=table,.kind=OS_CONTROL_BUTTON,
            .flags=OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_INHERIT_FONT|
                (entry->thread?OS_CONTROL_HIDDEN:0),.user_data=row,
            .x=(int32_t)taskmgr_scale(4U),.y=(int32_t)y,.width=(int32_t)taskmgr_scale(36U),
            .height=(int32_t)row_height,.foreground=TASKMGR_MUTED};
        if (!tree_text(g_nodes+*count-1U,used,entry->expanded?"v":">")) return false;
        char numbers[3][32]={{0}};
        if (entry->resources.flags&OS_TASK_KERNEL) copy_text(numbers[0],32,"-");
        else append_u64(numbers[0],32,entry->id);
        if (entry->cpu_sample_valid) {
            append_u64(numbers[1],32,entry->cpu/1000U);append_text(numbers[1],32,".");
            uint32_t fraction=entry->cpu%1000U;
            if (fraction<100U) append_text(numbers[1],32,"0");
            if (fraction<10U) append_text(numbers[1],32,"0");
            append_u64(numbers[1],32,fraction);append_text(numbers[1],32,"%");
        } else copy_text(numbers[1],32,"-");
        if (entry->thread||(entry->resources.flags&OS_TASK_KERNEL)) copy_text(numbers[2],32,"-");
        else {
            uint64_t kib=entry->resources.rss_bytes/1024U;
            append_u64(numbers[2],32,kib/1024U);append_text(numbers[2],32,".");
            append_u64(numbers[2],32,(kib%1024U)*10U/1024U);
        }
        bool running=entry->thread?entry->state==OS_THREAD_RUNNING:entry->state==OS_PROCESS_RUNNING;
        const char *values[]={entry->name,numbers[0],entry->thread?thread_state_text(entry->state):
            process_state_text(entry->state),numbers[1],numbers[2]};
        for (uint32_t c=0;c<5;++c) {
            uint32_t x=c==0&&entry->thread?taskmgr_scale(64U):columns[c];
            uint32_t right=columns[c+1]>margin?columns[c+1]-margin:0U;
            if (!table_label(count,used,table,id+2U+c,x,y+text_y,right>x?right-x:0U,
                c==2?(running?0x002A7A43U:TASKMGR_MUTED):c==1?TASKMGR_MUTED:TASKMGR_TEXT,values[c])) return false;
        }
    }
    return true;
}

static void render(void) {
    uint32_t count=0,used=0;
    uint32_t caption=user_caption_height();
    if (caption!=g_caption_height) {
        os_window_update_t update={.hdr={sizeof(update),OS_SYSCALL_ABI_VERSION},
            .identifier=g_window.identifier,.flags=OS_WINDOW_CAPTION_HEIGHT(caption),
            .width=g_window.width,.height=g_window.height};
        if (taskmgr_syscall_one(OS_SYS_WINDOW_UPDATE,(uintptr_t)&update)==0) g_caption_height=caption;
    }
    if (g_selected_entry>=g_entry_count) dismiss_menu();
    if (!build_table(&count,&used)) {set_status("Cannot build interface");return;}
    if (g_menu_open) {
        /* 菜单行高、内边距和边缘约束统一交给合成器，避免缩放后两套尺寸不一致。 */
        uint32_t width=menu_width();
        g_nodes[count++]=(os_control_node_t){.identifier=menu_target(0),.parent=0,.kind=OS_CONTROL_CONTAINER,
            .flags=OS_CONTROL_BACKGROUND_ALPHA,.width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL};
        uint32_t panel=count++;
        g_nodes[panel]=(os_control_node_t){.identifier=menu_target(1),.parent=0,.kind=OS_CONTROL_CONTAINER,
            .x=(int32_t)g_menu_x,.y=(int32_t)g_menu_y,.width=(int32_t)width,.height=0,
            .flags=OS_CONTROL_MENU_PANEL|OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_INHERIT_TEXT};
        for (uint32_t row=0;row<menu_rows();++row) {
            bool thread_only=g_entries[g_selected_entry].thread&&row==0;
            bool confirm=g_confirm_pid&&((g_confirm_tid!=0)==thread_only);
            bool protected=(g_entries[g_selected_entry].resources.flags&OS_TASK_PROTECTED)!=0;
            const char *label=thread_only?(confirm?"确认结束线程":"结束线程"):
                (confirm?"确认结束进程":"结束进程");
            g_nodes[count]=(os_control_node_t){.identifier=menu_target(2+row),.parent=panel,.kind=OS_CONTROL_BUTTON,
                .width=OS_CONTROL_FILL,
                .flags=OS_CONTROL_INHERIT_FONT|(protected?OS_CONTROL_DISABLED:
                    ((g_confirm_pid?confirm:row==0)?OS_CONTROL_DEFAULT_FOCUS:0)),
                .background=OS_THEME_SURFACE,.foreground=protected?TASKMGR_MUTED:OS_THEME_DANGER};
            if (!tree_text(g_nodes+count++,&used,label)) return;
        }
    }
    if (!user_caption_tree(g_nodes,&count,OS_CONTROL_TREE_MAX_NODES,g_tree_text,&used,sizeof(g_tree_text),
            (user_caption_layout_t){.title="任务管理器",.width=g_window.width,
                .height=caption,.font=ui_text_size(),.title_width=5U*ui_text_size()})) return;
    /* 汇总作为标题栏控件，缩放与窗口按钮采用同一套布局。 */
    uint32_t summary_x=28U+5U*ui_text_size();
    uint32_t button_width=os_window_caption_button_width(OS_WINDOW_CAPTION_HEIGHT(caption));
    if (button_width>g_window.width/3U) button_width=g_window.width/3U;
    uint32_t summary_right=g_window.width-3U*button_width;
    if (summary_right>summary_x+12U) {
        char summary[160];taskmgr_summary(summary,sizeof(summary));
        if (!table_label(&count,&used,0,6,summary_x,(caption-ui_text_size())/2U,
                summary_right-summary_x-12U,TASKMGR_TEXT,summary)) return;
    }
    os_window_control_tree_t request={.hdr={sizeof(request),OS_SYSCALL_ABI_VERSION},
        .identifier=g_window.identifier,.count=count,.nodes=(uintptr_t)g_nodes,
        .text=(uintptr_t)g_tree_text,.text_size=used};
    if (taskmgr_syscall_one(OS_SYS_WINDOW_CONTROL_TREE,(uintptr_t)&request)==0) {
        g_tree=request;g_menu_published=g_menu_open;
    } else {
        /* 提交失败后不再根据未发布的节点解释事件，下一次刷新会重试完整文档。 */
        g_tree.count=0;g_menu_published=false;dismiss_menu();set_status("Cannot update interface");
    }
}

static bool create_window(void) {
    os_display_info_t display = {0};
    os_window_create_t request = {0};
    uint32_t width;
    uint32_t height;
    display.hdr.size = sizeof(display);
    display.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (taskmgr_syscall_one(OS_SYS_DISPLAY_GET_INFO,
                            (uint64_t)(uintptr_t)&display) < 0 ||
        display.width < 760U || display.height < 420U) return false;
    width = display.width * 4U / 5U;
    height = display.height * 3U / 4U;
    if (width > 1040U) width = 1040U;
    if (height > 680U) height = 680U;
    if (width < 760U) width = 760U;
    if (height < 420U) height = 420U;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.x = (int32_t)((display.width - width) / 2U);
    request.y = (int32_t)((display.height - height) / 2U);
    request.width = width;
    request.height = height;
    request.flags = OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE |
                    OS_WINDOW_STANDARD_CAPTION_CONTROLS | OS_WINDOW_CAPTION_HEIGHT(user_caption_height());
    request.background = TASKMGR_BACKGROUND;
    user_caption_format_title(request.title, "任务管理器", false);
    request.address = TASKMGR_MAP_BASE;
    if (taskmgr_syscall_one(OS_SYS_WINDOW_CREATE,
                            (uint64_t)(uintptr_t)&request) < 0 ||
        request.window == OS_INVALID_HANDLE || request.address == 0U) return false;
    g_window.handle = request.window;
    g_window.identifier = request.identifier;
    g_window.width = request.width;
    g_window.height = request.height;
    g_window.pixels = (uint32_t *)(uintptr_t)request.address;
    return true;
}

static bool key_is(const os_input_event_t *input, uint32_t hid, char ascii) {
    return input != 0 && (input->code == hid || input->code == (uint32_t)ascii);
}

static void end_selected_process(bool thread_only) {
    if (!g_selection_valid || g_selected_entry >= g_entry_count) return;
    taskmgr_entry_t *entry = &g_entries[g_selected_entry];
    uint64_t pid = entry->thread ? entry->parent_id : entry->id;
    uint64_t tid = thread_only && entry->thread ? entry->id : 0U;
    if (entry->resources.flags & OS_TASK_PROTECTED) {
        g_confirm_pid = 0U;
        set_status("SYSTEM PROCESS: CANNOT END");
        return;
    }
    if (g_confirm_pid != pid || g_confirm_tid != tid || g_confirm_created != entry->resources.create_time_ns) {
        g_confirm_pid = pid;
        g_confirm_tid = tid;
        g_confirm_created = entry->resources.create_time_ns;
        set_status(thread_only ? "Click Confirm end thread to terminate only this thread" :
            "Click Confirm end process to terminate the whole process");
        return;
    }
    os_process_terminate_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.pid = pid;
    request.create_time_ns = g_confirm_created;
    int64_t status;
    if (tid != 0U) {
        os_thread_terminate_t thread = {0};
        thread.hdr = request.hdr;
        thread.hdr.size = sizeof(thread);
        thread.pid = pid; thread.tid = tid; thread.create_time_ns = g_confirm_created;
        status = taskmgr_syscall_one(OS_SYS_THREAD_TERMINATE, (uint64_t)(uintptr_t)&thread);
    } else status = taskmgr_syscall_one(OS_SYS_PROCESS_TERMINATE, (uint64_t)(uintptr_t)&request);
    dismiss_menu();
    refresh_processes();
    set_status(status == 0 ? "Termination requested" : "Cannot end this process");
}

static bool handle_menu_event(const os_window_event_t *event) {
    if(event->type!=OS_WINDOW_EVENT_CONTROL || !g_menu_open)return false;
    if (!os_control_event_node(&g_tree,g_nodes,&event->control)) {
        /* 自动刷新或菜单重建不能丢掉已经排队的 Ctrl 释放。 */
        if (g_tree.count && event->control.tree_revision &&
            event->control.tree_revision<=g_tree.revision &&
            event->control.action==OS_CONTROL_KEY &&
            (event->control.code==0xe0U || event->control.code==0xe4U) &&
            event->control.value==OS_INPUT_VALUE_RELEASE) g_ctrl=false;
        return true;
    }
    /* 旧树排队的事件不能越过确认步骤，也不能作用于之后新打开的菜单。 */
    if (!g_menu_published) return true;
    if (event->control.action==OS_CONTROL_KEY && event->control.code==0x29U &&
        event->control.value!=OS_INPUT_VALUE_RELEASE) {dismiss_menu();render();return true;}
    if ((event->control.target & ~15U)!=menu_target(0)) return true;
    uint32_t target=event->control.target&15U;
    if(event->control.action==OS_CONTROL_CLICK && target>=2 && target<2+menu_rows()) {
        end_selected_process(g_entries[g_selected_entry].thread && target==2);
        if(g_menu_open)menu_next_generation();
        render();
    } else if((event->control.action==OS_CONTROL_DOWN && target==0) ||
        (event->control.action==OS_CONTROL_KEY && event->control.code==0x29U &&
         event->control.value!=OS_INPUT_VALUE_RELEASE)) {
        dismiss_menu();render();
    } else if(event->control.action==OS_CONTROL_KEY &&
        (event->control.code==0xe0U || event->control.code==0xe4U)) {
        g_ctrl=event->control.value!=OS_INPUT_VALUE_RELEASE;
    }
    return true;
}

static void handle_event(const os_window_event_t *event) {
    const os_input_event_t *input;
    os_input_event_t translated={0};
    if (event == 0) return;
    if (event->type == OS_WINDOW_EVENT_CLOSE_REQUEST) taskmgr_exit(0U);
    if (event->type == OS_WINDOW_EVENT_RESIZE) {
        if (event->resize.width == 0U || event->resize.height == 0U ||
            (uint64_t)event->resize.width * event->resize.height * 4U >
                event->resize.buffer_size) return;
        g_window.width = event->resize.width;
        g_window.height = event->resize.height;
        dismiss_menu();
        ensure_selection_visible();
        render();
        return;
    }
    if (handle_menu_event(event)) return;
    if (event->type==OS_WINDOW_EVENT_INPUT) {
        /* 普通窗口的键盘事件直接交给应用；菜单接管焦点时才走控件事件。 */
        input=&event->input;
    } else if (event->type==OS_WINDOW_EVENT_CONTROL) {
        const os_control_node_t *node=os_control_event_node(&g_tree,g_nodes,&event->control);
        /* 自动刷新会替换控件树，但根节点和修饰键的身份不随条目变化。 */
        bool modifier=event->control.action==OS_CONTROL_KEY &&
            (event->control.code==0xE0U || event->control.code==0xE4U);
        bool root_key=event->control.action==OS_CONTROL_KEY && event->control.target==1U &&
            event->control.node_index==0U && event->control.node_kind==OS_CONTROL_CONTAINER &&
            event->control.user_data==0U;
        if (!node && (modifier || root_key) && g_tree.count && event->control.tree_revision &&
            event->control.tree_revision<=g_tree.revision) node=g_nodes;
        if (!node) return;
        if (event->control.action==OS_CONTROL_DOWN || event->control.action==OS_CONTROL_CLICK) {
            uint32_t row=(uint32_t)node->user_data;
            bool disclosure=node->identifier==0x10001U+row*8U;
            bool entry=node->identifier==0x10000U+row*8U;
            if (row>=g_entry_count || (!entry&&!disclosure)) return;
            bool right=event->control.action==OS_CONTROL_DOWN&&event->control.code==OS_INPUT_BUTTON_RIGHT;
            bool choose=event->control.action==OS_CONTROL_DOWN&&event->control.code==OS_INPUT_BUTTON_LEFT&&entry;
            bool expand=event->control.action==OS_CONTROL_CLICK&&disclosure&&!g_entries[row].thread;
            if (!right&&!choose&&!expand) return;
            g_selected_entry=row;g_selection_valid=true;dismiss_menu();
            if (right) {
                g_menu_open=true;menu_next_generation();
                g_menu_x=event->pointer_x>0?(uint32_t)event->pointer_x:0U;
                g_menu_y=event->pointer_y>0?(uint32_t)event->pointer_y:0U;
            } else if (expand) {
                g_entries[row].expanded=!g_entries[row].expanded;g_visible_dirty=true;
            }
            render();return;
        }
        if (event->control.action==OS_CONTROL_KEY) translated.type=OS_INPUT_EVENT_KEY;
        else if (event->control.action==OS_CONTROL_WHEEL) translated.type=OS_INPUT_EVENT_RELATIVE;
        else return;
        translated.code=event->control.code;translated.value=event->control.value;input=&translated;
    } else return;
    if (input->type == OS_INPUT_EVENT_KEY &&
        (input->code == 0xE0U || input->code == 0xE4U)) {
        g_ctrl = input->value != OS_INPUT_VALUE_RELEASE;
        return;
    }
    if (input->type == OS_INPUT_EVENT_RELATIVE &&
        input->code == OS_INPUT_REL_WHEEL && input->value != 0) {
        if (g_ctrl) {
            (void)ui_text_adjust(input->value > 0 ? 1 : -1);
            ensure_selection_visible();
            render();
            return;
        }
        select_delta(input->value > 0 ? -3 : 3);
        render();
        return;
    }
    if (input->type != OS_INPUT_EVENT_KEY || input->value == OS_INPUT_VALUE_RELEASE) return;
    if (key_is(input, 0x29U, 27) && g_menu_open) { dismiss_menu(); render(); return; }
    if (key_is(input, 0x29U, 27) || key_is(input, 0x14U, 'q')) taskmgr_exit(0U);
    if (g_ctrl && input->code == 0x2EU) {
        (void)ui_text_adjust(1);
        ensure_selection_visible();
    } else if (g_ctrl && input->code == 0x2DU) {
        (void)ui_text_adjust(-1);
        ensure_selection_visible();
    } else if (input->code == 0x52U) {
        select_delta(-1);
    } else if (input->code == 0x51U) {
        select_delta(1);
    } else if (input->code == 0x4BU) {
        select_delta(-(int32_t)visible_rows());
    } else if (input->code == 0x4EU) {
        select_delta((int32_t)visible_rows());
    } else {
        return;
    }
    render();
}

int main(void) {
    if (!ui_text_init(OS_TEXT_DEFAULT_SIZE) ||
        !create_window()) return 1;
    refresh_processes();
    render();
    for (;;) {
        os_window_event_read_t request = {0};
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_window.identifier;
        uint64_t now = monotonic_ns();
        if (now >= g_next_refresh) {
            refresh_processes();
            render();
            now = monotonic_ns();
        }
        request.timeout_ns = now < g_next_refresh ? g_next_refresh - now : 0U;
        int64_t status = taskmgr_syscall_one(OS_SYS_WINDOW_EVENT_READ,
                                             (uint64_t)(uintptr_t)&request);
        if (status == 0) handle_event(&request.event);
        else if (status != -11 && status != -110) __asm__ volatile ("pause");
    }
}

__attribute__((noreturn)) void taskmgr_entry(void) {
    taskmgr_exit((uint64_t)(main() == 0 ? 0 : 1));
}
