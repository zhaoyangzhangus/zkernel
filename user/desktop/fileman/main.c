#include <stdint.h>
#include <stdbool.h>

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "file_copy.h"

#include <uapi/all.h>

#include "../../runtime/text.h"
#include "../../runtime/caption.h"
#include "../../runtime/caption_tree.h"
#include "../../runtime/modal_tree.h"
#include "../../runtime/message_box.h"
#include <uapi/window_theme.h>
#include <uapi/utf8.h>

#define FILEMAN_MAP_BASE       0x0A000000ULL
#define FILEMAN_EVENT_WAIT OS_WAIT_INFINITE
#define FILEMAN_PATH_CAPACITY  256U
#define FILEMAN_ENTRY_INITIAL_CAPACITY 32U

/*
 * GNOME Files 风格布局，装饰按钮仍交给合成器。
 */
#define FM4_SIDEBAR_WIDTH          (198U * ui_text_size() / OS_TEXT_DEFAULT_SIZE)
/* 导航控件全部位于服务端标题栏，客户区直接从文件内容开始。 */
#define FM4_HEADER_HEIGHT 0U
#define FM4_FRAME_BASE_COUNT (15U + 2U*(g_place_count+g_volume_count))
#define FM4_MENU_NODE_COUNT (1U + FM4_CONTEXT_ROWS)
#define FM4_DIALOG_NODE_COUNT OS_MESSAGE_NODE_CAPACITY
#define FM4_FRAME_NODE_COUNT OS_CONTROL_TREE_MAX_NODES
#define FM4_FRAME_TEXT_CAPACITY OS_CONTROL_TREE_MAX_TEXT
#define FM4_ITEM_ID_BASE 0x10000000U

#define FM4_GRID_MARGIN_X           24U
#define FM4_GRID_MARGIN_Y           20U
#define FM4_SCALE(value)            (((value)*ui_text_size()+OS_TEXT_DEFAULT_SIZE/2U)/OS_TEXT_DEFAULT_SIZE)
#define FM4_GRID_ICON_SIZE          FM4_SCALE(64U)
#define FM4_GRID_LABEL_TOP          FM4_SCALE(78U)

#define FM4_GRID_CARD_WIDTH        (g_list_view ? (g_window.width > FM4_SIDEBAR_WIDTH + 48U ? g_window.width - FM4_SIDEBAR_WIDTH - 48U : 1U) : (126U * ui_text_size() / OS_TEXT_DEFAULT_SIZE))
#define FM4_GRID_CARD_HEIGHT       (g_list_view ? ui_text_line_height() + FM4_SCALE(18U) : FM4_GRID_LABEL_TOP + 2U * ui_text_line_height() + FM4_SCALE(10U))
#define FM4_GRID_STEP_X            (FM4_GRID_CARD_WIDTH + 22U)
#define FM4_GRID_STEP_Y            (FM4_GRID_CARD_HEIGHT + (g_list_view ? 2U : 14U))

#define FM4_CONTEXT_WIDTH          (180U * ui_text_size() / OS_TEXT_DEFAULT_SIZE)
#define FM4_CONTEXT_ROW_HEIGHT      (ui_text_line_height() + 10U)
#define FM4_CONTEXT_ROWS 8U

/* 常用位置与实际挂载卷分组显示；不存在的目录不伪装成可用位置。 */
#define FM4_VOLUME_CAPACITY         16U
#define FM4_VOLUME_ROW_HEIGHT       (ui_text_line_height() + 15U)
#define FM4_VOLUME_FIRST_Y          (FM4_HEADER_HEIGHT + 10U + (g_place_count + 1U) * FM4_VOLUME_ROW_HEIGHT)

#define FM4_HISTORY_CAPACITY        16U
/*
 * input.timestamp 当前保存原始 x86 TSC 节拍。
 *
 * 不要将此值标记为纳秒。在当前高频 x86_64 环境中，30 亿个 TSC 节拍可提供
 * 实用的桌面双击时间窗口。
 */
#define FM4_DOUBLE_CLICK_TSC_TICKS 3000000000ULL

#define FM4_INDEX_NONE      0xFFFFFFFFU


typedef struct fileman_window {
    os_handle_t handle;
    uint32_t identifier;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} fileman_window_t;

typedef struct fileman_entry {
    os_file_info_t info;
    char path[FILEMAN_PATH_CAPACITY];
} fileman_entry_t;

typedef struct fileman_volume {
    char label[OS_FILE_NAME_MAX];
    char path[FILEMAN_PATH_CAPACITY];
} fileman_volume_t;

static const char g_notepad_path[] = "/usr/apps/notepad/notepad";
static const char g_notepad_name[] = "notepad";
static fileman_window_t g_window = {OS_INVALID_HANDLE, 0U, 0U, 0U, 0};
typedef struct fm4_frame_buffer {
    os_control_node_t nodes[FM4_FRAME_NODE_COUNT];
    char text[FM4_FRAME_TEXT_CAPACITY];
} fm4_frame_buffer_t;
static fm4_frame_buffer_t g_frame_buffers[2];
static fm4_frame_buffer_t *g_frame=&g_frame_buffers[0];
static fm4_frame_buffer_t *g_candidate=&g_frame_buffers[1];
static os_window_control_tree_t g_frame_tree;
static uint32_t g_caption_height;
static bool g_frame_input_ready;
/* 视图仅保存指针，文件信息和完整路径始终由目录快照唯一持有。 */
static fileman_entry_t **g_entries;
/* 与可见视图共用一次指针分配，排序不搬动文件信息和完整路径。 */
static fileman_entry_t **g_directory_order;
static fileman_entry_t *g_directory_entries;
static uint32_t g_directory_count;
static fileman_volume_t g_volumes[FM4_VOLUME_CAPACITY];
static char g_path[FILEMAN_PATH_CAPACITY] = "/";
static char g_home[FILEMAN_PATH_CAPACITY] = "/";
static fileman_volume_t g_places[12];
static uint32_t g_place_count;
static bool g_list_view;
static bool g_sort_descending;
/* 输入焦点只有一个；筛选词独立保存，离开输入框后仍保持可见。 */
enum fm4_edit_mode { FM4_EDIT_NONE, FM4_EDIT_LOCATION, FM4_EDIT_SEARCH };
static enum fm4_edit_mode g_edit_mode;
static bool g_edit_select_all;
static char g_query[128];
static char g_location[FILEMAN_PATH_CAPACITY];
static uint32_t g_name_action;
static char g_rename_source[FILEMAN_PATH_CAPACITY];
static char g_clipboard[FILEMAN_PATH_CAPACITY];
static bool g_clipboard_cut;
static fm_copy_context_t g_copy_context;
static pthread_t g_copy_thread;
static atomic_bool g_copy_finished;
static bool g_copy_active, g_close_after_copy;
static int g_copy_error;
static uint64_t g_copy_displayed_bytes;
static char g_copy_source[FILEMAN_PATH_CAPACITY];
static char g_copy_destination[FILEMAN_PATH_CAPACITY];
static char g_status[128] = "READY";
static uint32_t g_entry_count;
static uint32_t g_volume_count;
static uint32_t g_selected;
static uint32_t g_first_entry;
static bool g_ctrl;
static bool g_shift;

static bool g_selection_valid;


static int32_t g_pointer_x;
static int32_t g_pointer_y;

static uint32_t g_last_click_item =
    FM4_INDEX_NONE;

static uint64_t g_last_click_time;

static bool g_context_visible;
static uint32_t g_context_hover;
static bool g_context_keyboard;
static uint32_t g_context_x;
static uint32_t g_context_y;

static ui_message_box_t g_delete_message;
static char g_delete_path[FILEMAN_PATH_CAPACITY];


static char g_history[
    FM4_HISTORY_CAPACITY]
    [FILEMAN_PATH_CAPACITY];

static uint32_t g_history_count;
static uint32_t g_history_index;

void __main(void) {
}

static int64_t fileman_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t fileman_syscall_three(uint64_t number, uint64_t arg0,
                                     uint64_t arg1, uint64_t arg2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = 0U;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10) : : "rcx", "r11", "memory");
    return (int64_t)rax;
}

__attribute__((noreturn)) static void fileman_exit(uint64_t status) {
    (void)fileman_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static uint32_t text_length(const char *text) {
    uint32_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0') ++length;
    return length;
}

static void copy_text(char *destination, uint32_t capacity, const char *source) {
    uint32_t index = 0U;
    if (destination == 0 || source == 0 || capacity == 0U) return;
    while (index + 1U < capacity && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static void append_text(char *destination, uint32_t capacity, const char *source) {
    uint32_t length;
    uint32_t index = 0U;
    if (destination == 0 || source == 0 || capacity == 0U) return;
    length = text_length(destination);
    while (length + 1U < capacity && source[index] != '\0') {
        destination[length++] = source[index++];
    }
    destination[length] = '\0';
}

static void append_character(char *destination, uint32_t capacity, char value) {
    uint32_t length = text_length(destination);
    if (destination != 0 && length + 1U < capacity) {
        destination[length] = value;
        destination[length + 1U] = '\0';
    }
}

static void append_decimal(char *destination, uint32_t capacity, uint64_t value) {
    char digits[24];
    uint32_t count = 0U;
    if (destination == 0 || capacity == 0U) return;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U) append_character(destination, capacity, digits[--count]);
}

static bool set_path(const char *source) {
    uint32_t length = 0U;
    if (source == 0 || source[0] == '\0') return false;
    if (source[0] == '/') {
        while (source[length] != '\0') {
            if (++length >= FILEMAN_PATH_CAPACITY) return false;
        }
        for (uint32_t index = 0U; index <= length; ++index) g_path[index] = source[index];
        return true;
    }
    static const char prefix[] = "/";
    uint32_t prefix_length = text_length(prefix);
    while (source[length] != '\0') {
        if (prefix_length + length + 1U >= FILEMAN_PATH_CAPACITY) return false;
        ++length;
    }
    for (uint32_t index = 0U; index < prefix_length; ++index) g_path[index] = prefix[index];
    for (uint32_t index = 0U; index <= length; ++index) {
        g_path[prefix_length + index] = source[index];
    }
    return true;
}

static bool join_path(char *destination, const char *directory, const char *name) {
    uint32_t directory_length;
    uint32_t name_length;
    if (destination == 0 || directory == 0 || name == 0) return false;
    directory_length = text_length(directory);
    name_length = text_length(name);
    if (directory_length + name_length + 2U >= FILEMAN_PATH_CAPACITY) return false;
    copy_text(destination, FILEMAN_PATH_CAPACITY, directory);
    if (directory_length != 1U || directory[0] != '/') append_character(
        destination, FILEMAN_PATH_CAPACITY, '/');
    append_text(destination, FILEMAN_PATH_CAPACITY, name);
    return true;
}

static void parent_directory(void) {
    uint32_t length = text_length(g_path);
    if (length <= 1U) return;
    while (length > 1U && g_path[length - 1U] == '/') g_path[--length] = '\0';
    while (length > 1U && g_path[length - 1U] != '/') g_path[--length] = '\0';
    if (length > 1U) g_path[length - 1U] = '\0';
    if (g_path[0] == '\0') copy_text(g_path, sizeof(g_path), "/");
}

static int64_t stat_path(const char *path, os_file_info_t *info) {
    os_file_stat_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)path;
    int64_t status = fileman_syscall_one(OS_SYS_FILE_STAT, (uint64_t)&request);
    if (status == 0 && info != 0) *info = request.info;
    return status;
}


static bool load_directory(void);
static void fm4_apply_filter(void);
static int32_t fm4_compare(const char *a, const char *b);

/* 截断与换行复用合成器字符单元，不再加载私有字体度量。 */
static uint32_t fm4_measure_range(const char *text,size_t length) {
    uint32_t width=0;
    for (size_t offset=0;offset<length;) {
        size_t used=0;uint32_t code=os_utf8_next(text+offset,length-offset,&used);
        width+=os_control_text_cell_width(code,ui_text_size());
        offset+=used ? used : 1U;
    }
    return width;
}
static uint32_t fm4_measure(const char *text) {return fm4_measure_range(text,strlen(text));}

static void fm4_fit_text(char *text, uint32_t capacity,
                         uint32_t maximum_width) {
    if (!text || !capacity) return;
    size_t bytes=strlen(text);
    if (fm4_measure_range(text,bytes)<=maximum_width) return;
    uint32_t font=ui_text_size();
    uint32_t suffix=os_control_text_cell_width(0x2026U,font);
    size_t end=0;
    uint32_t width=0;
    if (suffix<=maximum_width) {
        /* 只前进一次，保留省略号空间，并至少省略原文的最后一个字符。 */
        while (end<bytes) {
            size_t used;uint32_t code=os_utf8_next(text+end,bytes-end,&used);
            size_t next=end+(used?used:1U);
            uint32_t advance=os_control_text_cell_width(code,font);
            if (next>=bytes || next+3U>=capacity || advance>maximum_width-suffix-width) break;
            width+=advance;end=next;
        }
    }
    text[end]=0;
    if (bytes && suffix<=maximum_width && capacity>3U) memcpy(text+end,"…",4U);
}

static void fm4_volume_label(char *destination, uint32_t capacity,
                             const char *name) {
    copy_text(destination, capacity, name);
    /* 将侧栏标签限制在 200 像素宽的区域内，不依赖帧缓冲区裁剪。 */
    fm4_fit_text(destination, capacity, FM4_SIDEBAR_WIDTH - 66U);
}


enum {
    FM4_CONTROL_NONE = 0U,

    FM4_CONTROL_BACK,
    FM4_CONTROL_FORWARD,

    FM4_CONTROL_SEARCH,
    FM4_CONTROL_VIEW,
    FM4_CONTROL_SORT,
    FM4_CONTROL_LOCATION,
    FM4_CONTROL_PLACE_BASE = 0x40U,

    /* 卷标控件的编号为 FM4_CONTROL_VOLUME_BASE + index。 */
    FM4_CONTROL_VOLUME_BASE = 0x100U,

    FM4_CONTROL_CONTEXT_OPEN = 0x200U,
    FM4_CONTROL_CONTEXT_DELETE,
    FM4_CONTROL_CONTEXT_PROPERTIES,
    FM4_CONTROL_CONTEXT_RENAME,
    FM4_CONTROL_CONTEXT_COPY,
    FM4_CONTROL_CONTEXT_CUT,
    FM4_CONTROL_CONTEXT_PASTE,
    FM4_CONTROL_CONTEXT_NEW_FOLDER,

};

static void fm4_volume_add(const char *path, const char *name) {
    if (path == 0 || name == 0 || path[0] == '\0' || name[0] == '\0' ||
        g_volume_count >= FM4_VOLUME_CAPACITY) return;

    for (uint32_t index = 0U; index < g_volume_count; ++index) {
        if (fm4_compare(g_volumes[index].path, path) == 0) return;
    }

    copy_text(g_volumes[g_volume_count].path,
              sizeof(g_volumes[g_volume_count].path), path);
    fm4_volume_label(g_volumes[g_volume_count].label,
                     sizeof(g_volumes[g_volume_count].label), name);
    ++g_volume_count;
}

/*
 * 当前只有一个 VFS 命名空间，可移动文件系统通过常规挂载目录公开。
 * 根目录已由主文件夹入口提供，只补充挂载容器下实际存在的目录；
 * 用户主目录中的普通文件夹不会作为磁盘显示。
 */
static void fm4_load_volumes(void) {
    static const char *const containers[] = {
        "/mnt", "/media", "/volumes", "/disks", "/drives",
    };

    g_volume_count = 0U;

    for (uint32_t container_index = 0U;
         container_index < sizeof(containers) / sizeof(containers[0]);
         ++container_index) {
        os_file_info_t container_info = {0};
        if (stat_path(containers[container_index], &container_info) < 0 ||
            container_info.type != OS_FILE_TYPE_DIRECTORY) continue;

        for (uint32_t entry_index = 0U;
             entry_index < UINT32_MAX;
             ++entry_index) {
            os_file_enumerate_t request = {0};
            char volume_path[FILEMAN_PATH_CAPACITY];
            char volume_label[OS_FILE_NAME_MAX];

            request.hdr.size = sizeof(request);
            request.hdr.version = OS_SYSCALL_ABI_VERSION;
            request.path = (uint64_t)(uintptr_t)containers[container_index];
            request.index = entry_index;

            int64_t status = fileman_syscall_one(
                OS_SYS_FILE_ENUMERATE, (uint64_t)&request);
            if (status == -2) break;
            if (status < 0 || request.info.type != OS_FILE_TYPE_DIRECTORY) {
                continue;
            }
            if (!join_path(volume_path, containers[container_index],
                           request.info.name)) continue;

            copy_text(volume_label, sizeof(volume_label), request.info.name);
            append_text(volume_label, sizeof(volume_label), " Volume");
            fm4_volume_add(volume_path, volume_label);
        }
    }
}

static bool fm4_volume_path_active(const fileman_volume_t *volume) {
    uint32_t base_length;
    if (volume == 0) return false;
    base_length = text_length(volume->path);
    if (text_length(g_path) < base_length) return false;
    for (uint32_t index = 0U; index < base_length; ++index) {
        if (g_path[index] != volume->path[index]) return false;
    }
    return g_path[base_length] == '\0' || g_path[base_length] == '/';
}


static void fm4_status(
    const char *text) {

    copy_text(
        g_status,
        sizeof(g_status),
        text != 0 ? text : "READY");
}


/*
 * ASCII 不区分大小写排序。
 */
static char fm4_fold(char c) {
    if (c >= 'A' &&
        c <= 'Z') {

        return
            (char)(c - 'A' + 'a');
    }

    return c;
}


static int32_t fm4_compare(
    const char *a,
    const char *b) {

    uint32_t index = 0U;

    for (;;) {
        char ca =
            fm4_fold(a[index]);

        char cb =
            fm4_fold(b[index]);

        if (ca < cb) return -1;
        if (ca > cb) return 1;

        if (ca == '\0') {
            return 0;
        }

        ++index;
    }
}


static int fm4_entry_compare(const void *left, const void *right) {
    const fileman_entry_t *a = *(fileman_entry_t *const *)left;
    const fileman_entry_t *b = *(fileman_entry_t *const *)right;
    bool ad = a->info.type == OS_FILE_TYPE_DIRECTORY;
    bool bd = b->info.type == OS_FILE_TYPE_DIRECTORY;
    if (ad != bd) return ad ? -1 : 1;
    int32_t order = fm4_compare(a->info.name, b->info.name);
    return g_sort_descending ? -order : order;
}

/* 自有 libc 使用原地堆排序，大目录不会退化为平方次文件名比较。 */
static void fm4_sort(void) {
    if (g_directory_count > 1U)
        qsort(g_directory_order, g_directory_count, sizeof(*g_directory_order), fm4_entry_compare);
    fm4_apply_filter();
}


/*
 * 网格布局。
 */
static uint32_t fm4_content_left(void) {
    return
        FM4_SIDEBAR_WIDTH +
        FM4_GRID_MARGIN_X;
}


static uint32_t fm4_grid_top(void) {
    return
        FM4_HEADER_HEIGHT +
        FM4_GRID_MARGIN_Y;
}


static uint32_t fm4_columns(void) {
    if (g_list_view) return 1U;
    uint32_t left =
        fm4_content_left();

    uint32_t available;
    uint32_t columns;

    if (g_window.width <=
        left + 16U) {

        return 1U;
    }

    available =
        g_window.width -
        left -
        16U;

    columns =
        available /
        FM4_GRID_STEP_X;

    return columns != 0U ?
        columns :
        1U;
}


static uint32_t fm4_visible_rows(void) {
    uint32_t top =
        fm4_grid_top();

    uint32_t available;
    uint32_t rows;

    if (g_window.height <=
        top + 16U) {

        return 1U;
    }

    available =
        g_window.height -
        top -
        16U;

    rows =
        available /
        FM4_GRID_STEP_Y;

    return rows != 0U ?
        rows :
        1U;
}


static uint32_t fm4_total_rows(void) {
    uint32_t columns =
        fm4_columns();

    if (g_entry_count == 0U) {
        return 0U;
    }

    return
        (g_entry_count +
         columns - 1U) /
        columns;
}


static uint32_t fm4_max_scroll_row(void) {
    uint32_t total =
        fm4_total_rows();

    uint32_t visible =
        fm4_visible_rows();

    return total > visible ?
        total - visible :
        0U;
}


static void fm4_normalize_scroll(void) {
    uint32_t columns =
        fm4_columns();

    uint32_t row =
        g_first_entry /
        columns;

    uint32_t maximum =
        fm4_max_scroll_row();

    if (row > maximum) {
        row = maximum;
    }

    g_first_entry =
        row * columns;
}


static void fm4_selection_visible(void) {
    uint32_t columns;
    uint32_t visible;
    uint32_t first_row;
    uint32_t selected_row;

    if (!g_selection_valid ||
        g_entry_count == 0U) {

        fm4_normalize_scroll();
        return;
    }

    if (g_selected >=
        g_entry_count) {

        g_selected =
            g_entry_count - 1U;
    }

    columns =
        fm4_columns();

    visible =
        fm4_visible_rows();

    first_row =
        g_first_entry /
        columns;

    selected_row =
        g_selected /
        columns;

    if (selected_row < first_row) {
        first_row =
            selected_row;

    } else if (
        selected_row >=
        first_row + visible) {

        first_row =
            selected_row -
            visible + 1U;
    }

    if (first_row >
        fm4_max_scroll_row()) {

        first_row =
            fm4_max_scroll_row();
    }

    g_first_entry =
        first_row * columns;
}


static void fm4_scroll(
    int32_t rows) {

    uint32_t columns =
        fm4_columns();

    int64_t current =
        (int64_t)(
            g_first_entry /
            columns);

    int64_t next =
        current + rows;

    uint32_t maximum =
        fm4_max_scroll_row();

    if (next < 0) {
        next = 0;
    }

    if (next >
        (int64_t)maximum) {

        next = maximum;
    }

    g_first_entry =
        (uint32_t)next *
        columns;
}


static bool fm4_item_rect(
    uint32_t index,
    uint32_t *out_x,
    uint32_t *out_y) {

    uint32_t columns;
    uint32_t position;
    uint32_t row;
    uint32_t column;

    if (index >= g_entry_count ||
        index < g_first_entry) {

        return false;
    }

    columns =
        fm4_columns();

    position =
        index -
        g_first_entry;

    row =
        position /
        columns;

    column =
        position %
        columns;

    if (row >=
        fm4_visible_rows()) {

        return false;
    }

    if (out_x != 0) {
        *out_x =
            fm4_content_left() +
            column *
            FM4_GRID_STEP_X;
    }

    if (out_y != 0) {
        *out_y =
            fm4_grid_top() +
            row *
            FM4_GRID_STEP_Y;
    }

    return true;
}

static void fm4_hide_context(void) {
    g_context_visible = false;
    g_context_hover = FM4_CONTROL_NONE;
    g_context_keyboard = false;
}


static uint32_t fm4_item_at(
    int32_t px,
    int32_t py) {

    uint32_t left =
        fm4_content_left();

    uint32_t top =
        fm4_grid_top();

    uint32_t rx;
    uint32_t ry;

    uint32_t column;
    uint32_t row;

    uint32_t index;

    if (px < (int32_t)left ||
        py < (int32_t)top) {

        return FM4_INDEX_NONE;
    }

    rx =
        (uint32_t)px -
        left;

    ry =
        (uint32_t)py -
        top;

    column =
        rx /
        FM4_GRID_STEP_X;

    row =
        ry /
        FM4_GRID_STEP_Y;

    if (column >=
            fm4_columns() ||
        row >=
            fm4_visible_rows()) {

        return FM4_INDEX_NONE;
    }

    if ((rx %
         FM4_GRID_STEP_X) >=
            FM4_GRID_CARD_WIDTH ||
        (ry %
         FM4_GRID_STEP_Y) >=
            FM4_GRID_CARD_HEIGHT) {

        return FM4_INDEX_NONE;
    }

    index =
        g_first_entry +
        row * fm4_columns() +
        column;

    return index <
        g_entry_count ?
        index :
        FM4_INDEX_NONE;
}


/*
 * 文件名：最多两行。
 */

/*
 * 导航历史记录。
 */
static void fm4_history_reset(void) {
    g_history_count = 1U;
    g_history_index = 0U;

    copy_text(
        g_history[0],
        FILEMAN_PATH_CAPACITY,
        g_path);
}


static void fm4_history_push(void) {
    if (g_history_index + 1U <
        g_history_count) {

        g_history_count =
            g_history_index + 1U;
    }

    if (g_history_count >=
        FM4_HISTORY_CAPACITY) {

        for (uint32_t i = 1U;
             i < g_history_count;
             ++i) {

            copy_text(
                g_history[i - 1U],
                FILEMAN_PATH_CAPACITY,
                g_history[i]);
        }

        --g_history_count;

        if (g_history_index != 0U) {
            --g_history_index;
        }
    }

    copy_text(
        g_history[g_history_count],
        FILEMAN_PATH_CAPACITY,
        g_path);

    g_history_index =
        g_history_count;

    ++g_history_count;
}


static bool fm4_navigate(
    const char *path,
    bool push_history) {

    char previous[
        FILEMAN_PATH_CAPACITY];
    char previous_query[sizeof(g_query)];

    if (path == 0 ||
        path[0] == '\0') {

        return false;
    }

    copy_text(
        previous,
        sizeof(previous),
        g_path);

    copy_text(
        g_path,
        sizeof(g_path),
        path);
    copy_text(previous_query, sizeof(previous_query), g_query);
    g_query[0] = '\0';

    if (!load_directory()) {
        copy_text(
            g_path,
            sizeof(g_path),
            previous);

        copy_text(g_query, sizeof(g_query), previous_query);

        return false;
    }

    g_edit_mode = FM4_EDIT_NONE;
    g_name_action = 0U;
    fm4_hide_context();
    g_delete_message.active = false;

    if (push_history) {
        fm4_history_push();
    }

    fm4_status("READY");

    return true;
}


static bool fm4_history_go(
    int32_t direction) {

    uint32_t next;

    if (direction < 0) {
        if (g_history_index == 0U) {
            return false;
        }

        next =
            g_history_index - 1U;

    } else {
        if (g_history_index + 1U >=
            g_history_count) {

            return false;
        }

        next =
            g_history_index + 1U;
    }

    if (!fm4_navigate(
            g_history[next],
            false)) {

        return false;
    }

    g_history_index =
        next;

    return true;
}


static void fm4_parent(void) {
    char previous[
        FILEMAN_PATH_CAPACITY];

    copy_text(
        previous,
        sizeof(previous),
        g_path);

    parent_directory();

    if (fm4_compare(
            previous,
            g_path) == 0) {

        return;
    }

    char parent[FILEMAN_PATH_CAPACITY];
    copy_text(parent, sizeof(parent), g_path);
    copy_text(g_path, sizeof(g_path), previous);
    (void)fm4_navigate(parent, true);
}

/*
 * 上下文菜单。
 */
static void fm4_show_context(
    int32_t px,
    int32_t py) {

    uint32_t x =
        px < 0 ?
        0U :
        (uint32_t)px;

    uint32_t y =
        py < 0 ?
        0U :
        (uint32_t)py;

    uint32_t height =
        FM4_CONTEXT_ROW_HEIGHT * FM4_CONTEXT_ROWS;

    if (x + FM4_CONTEXT_WIDTH >
        g_window.width) {

        x =
            g_window.width >
            FM4_CONTEXT_WIDTH ?
            g_window.width -
            FM4_CONTEXT_WIDTH :
            0U;
    }

    if (y + height >
        g_window.height) {

        y =
            g_window.height >
            height ?
            g_window.height -
            height :
            0U;
    }

    /* 菜单独占悬停状态，避免下层文件或工具栏同时高亮。 */
    g_context_x = x;
    g_context_y = y;

    g_context_visible = true;
    g_context_hover = FM4_CONTROL_NONE;
    g_context_keyboard = false;
}


/* 查询仅预处理一次；失败前缀让重复字符的文件名也能线性匹配。 */
typedef struct fm4_search {
    uint32_t length;
    char text[sizeof(g_query)];
    uint8_t prefix[sizeof(g_query)];
} fm4_search_t;
_Static_assert(sizeof(g_query)<=UINT8_MAX+1U,"搜索前缀索引必须容纳整个查询");

static fm4_search_t fm4_search_prepare(void) {
    fm4_search_t search={0};
    for (uint32_t i=0;g_query[i];++i) {
        char value=fm4_fold(g_query[i]);
        uint32_t matched=i?search.prefix[i-1U]:0U;
        while (matched && search.text[matched]!=value) matched=search.prefix[matched-1U];
        if (i && search.text[matched]==value) ++matched;
        search.text[i]=value;search.prefix[i]=(uint8_t)matched;++search.length;
    }
    return search;
}

/* 搜索只读取目录快照，ASCII 忽略大小写，UTF-8 字节保持原样。 */
static bool fm4_name_matches(const char *name,const fm4_search_t *search) {
    if (!search->length) return true;
    uint32_t matched=0U;
    for (size_t i=0;name[i];++i) {
        char value=fm4_fold(name[i]);
        while (matched && search->text[matched]!=value) matched=search->prefix[matched-1U];
        if (search->text[matched]==value && ++matched==search->length) return true;
    }
    return false;
}

/* 筛选只读取目录快照。每次按键不再重复扫描磁盘或探测所有挂载点。 */
static void fm4_apply_filter(void) {
    if (!g_query[0]) {
        if (g_directory_count) memcpy(g_entries,g_directory_order,
            (size_t)g_directory_count*sizeof(*g_entries));
        g_entry_count=g_directory_count;
    } else {
        fm4_search_t search=fm4_search_prepare();
        g_entry_count = 0U;
        for (uint32_t i = 0U; i < g_directory_count; ++i)
            if (fm4_name_matches(g_directory_order[i]->info.name,&search))
                g_entries[g_entry_count++] = g_directory_order[i];
    }
    g_selection_valid = false;
    g_last_click_item = FM4_INDEX_NONE;
    g_last_click_time = 0U;
    g_first_entry = 0U;
    fm4_status("READY");
}

static void fm4_load_places(void) {
    static const char *const home_paths[] = {"/home/user", "/home", "/root", "/"};
    static const char *const names[] = {"Documents", "Music", "Pictures", "Videos", "Downloads", "Desktop"};
    static const char *const labels[] = {"文档", "音乐", "图片", "视频", "下载", "桌面"};
    os_file_info_t info;
    for (uint32_t i = 0; i < sizeof(home_paths) / sizeof(home_paths[0]); ++i) {
        if (stat_path(home_paths[i], &info) == 0 && info.type == OS_FILE_TYPE_DIRECTORY) {
            copy_text(g_home, sizeof(g_home), home_paths[i]);
            break;
        }
    }
    g_place_count = 1U;
    copy_text(g_places[0].label, sizeof(g_places[0].label), "主文件夹");
    copy_text(g_places[0].path, sizeof(g_places[0].path), g_home);
    for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[FILEMAN_PATH_CAPACITY];
        if (join_path(path, g_home, names[i]) && stat_path(path, &info) == 0 &&
            info.type == OS_FILE_TYPE_DIRECTORY) {
            copy_text(g_places[g_place_count].label, sizeof(g_places[0].label), labels[i]);
            copy_text(g_places[g_place_count++].path, sizeof(g_places[0].path), path);
        }
    }
}

static bool load_directory(void) {
    os_file_info_t directory_info = {0};
    fileman_entry_t *entries = 0;
    fileman_entry_t **visible = 0, **order = 0;
    uint32_t count = 0U, capacity = 0U;
    if (stat_path(g_path, &directory_info) < 0 ||
        directory_info.type != OS_FILE_TYPE_DIRECTORY) {
        copy_text(g_status, sizeof(g_status), "NOT A DIRECTORY");
        return false;
    }
    os_file_info_t batch[OS_FILE_ENUMERATE_BATCH_MAX];
    for (uint32_t index = 0U;;) {
        os_file_enumerate_batch_t request = {0};
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.path = (uint64_t)(uintptr_t)g_path;
        request.entries = (uint64_t)(uintptr_t)batch;
        request.capacity = OS_FILE_ENUMERATE_BATCH_MAX;
        request.index = index;
        int64_t status = fileman_syscall_one(OS_SYS_FILE_ENUMERATE_BATCH,
                                             (uint64_t)&request);
        if (status == -2) break;
        if (status < 0) {
            copy_text(g_status, sizeof(g_status), "DIRECTORY READ FAILED");
            goto fail;
        }
        if (request.count == 0U || request.count > request.capacity ||
            request.count > UINT32_MAX - index) {
            fm4_status("目录批次无效，保留原目录列表");
            goto fail;
        }
        for (uint32_t item = 0U; item < request.count; ++item) {
            if (count == capacity) {
                if (capacity > UINT32_MAX / 2U) {
                    fm4_status("目录条目过多");
                    goto fail;
                }
                uint32_t next_capacity = capacity ? capacity * 2U : FILEMAN_ENTRY_INITIAL_CAPACITY;
                if ((uint64_t)next_capacity * sizeof(*entries) > SIZE_MAX) {
                    fm4_status("目录条目过多");
                    goto fail;
                }
                fileman_entry_t *next = realloc(entries, (size_t)next_capacity * sizeof(*entries));
                if (!next) { fm4_status("内存不足，保留原目录列表"); goto fail; }
                entries = next;
                capacity = next_capacity;
            }
            entries[count].info = batch[item];
            if (!join_path(entries[count].path, g_path, batch[item].name)) {
                copy_text(g_status, sizeof(g_status), "PATH TOO LONG");
                goto fail;
            }
            ++count;
        }
        index += request.count;
    }
    if (count) {
        visible = malloc((size_t)count * 2U * sizeof(*visible));
        if (!visible) { fm4_status("内存不足，保留原目录列表"); goto fail; }
        /* 后半区保存完整排序索引，前半区留给无分配的搜索筛选。 */
        order = visible + count;
        for (uint32_t i = 0U; i < count; ++i) order[i] = entries + i;
        qsort(order, count, sizeof(*order), fm4_entry_compare);
    }
    /* 所有读取、路径拼接和分配成功后一次提交，失败不暴露半份目录。 */
    free(g_directory_entries);
    free(g_entries);
    g_directory_entries = entries;
    g_directory_count = count;
    g_entries = visible;
    g_directory_order = order;
    fm4_apply_filter();
    return true;
fail:
    free(entries);
    free(visible);
    return false;
}

/* 文本与候选树一起发布，容量不足时保留上一份完整文档。 */
static bool fm4_frame_text(os_control_node_t *node, char *text, uint32_t *size,
                           const char *label) {
    uint32_t bytes = (uint32_t)strlen(label);
    if (bytes > FM4_FRAME_TEXT_CAPACITY - *size) return false;
    node->text_offset = *size;
    node->text_length = bytes;
    memcpy(text + *size, label, bytes);
    *size += bytes;
    return true;
}

#include "content_tree.h"
#include "caption_layout.h"

/* 标题栏、侧栏、文件内容与模态框统一由合成器绘制。 */
static bool fm4_publish_frame(void) {
    g_frame_input_ready=false;
    uint32_t caption = user_caption_height();
    os_control_node_t *nodes=g_candidate->nodes;
    char *text=g_candidate->text;
    /* 构建器逐个完整初始化实际节点，不清空本帧不会提交的其余容量。 */
    bool searching = g_query[0] || g_edit_mode == FM4_EDIT_SEARCH;
    const char *path = g_edit_mode == FM4_EDIT_LOCATION ? g_location : searching ? g_query : g_path;
    copy_text(text, FILEMAN_PATH_CAPACITY, path);
    uint32_t length = (uint32_t)strlen(text);
    nodes[0] = (os_control_node_t){.identifier=1U, .parent=OS_CONTROL_NO_PARENT,
        .kind=OS_CONTROL_CONTAINER, .flags=OS_CONTROL_WINDOW_FRAME|OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_POINTER_PASSTHROUGH,
        .width=OS_CONTROL_FILL, .height=OS_CONTROL_FILL};
    const uint32_t actions[7] = {FM4_CONTROL_BACK, FM4_CONTROL_FORWARD, FM4_CONTROL_LOCATION,
        FM4_CONTROL_SEARCH, FM4_CONTROL_VIEW, FM4_CONTROL_SORT, FM4_CONTROL_NONE};
    const uint32_t symbols[7] = {OS_CONTROL_SYMBOL_BACK, OS_CONTROL_SYMBOL_FORWARD, OS_CONTROL_SYMBOL_NONE,
        OS_CONTROL_SYMBOL_SEARCH, g_list_view ? OS_CONTROL_SYMBOL_GRID : OS_CONTROL_SYMBOL_LIST,
        g_sort_descending ? OS_CONTROL_SYMBOL_SORT_DESCENDING : OS_CONTROL_SYMBOL_SORT_ASCENDING,
        OS_CONTROL_SYMBOL_MENU};
    for (uint32_t i=0; i<7; ++i) {
        nodes[1+i] = (os_control_node_t){.identifier=16U+i, .parent=0U,
            .kind=OS_CONTROL_BUTTON, .flags=OS_CONTROL_BACKGROUND_ALPHA|(i==2 ? 0U : OS_CONTROL_SYMBOL),
            .foreground=OS_THEME_TEXT,
            .font_size=ui_text_size(), .value=symbols[i],
            .user_data=actions[i]};
    }
    fm4_caption_layout(nodes+1U,g_window.width,caption,FM4_SIDEBAR_WIDTH);
    if (g_history_index==0) nodes[1].flags |= OS_CONTROL_DISABLED;
    if (g_history_index+1>=g_history_count) nodes[2].flags |= OS_CONTROL_DISABLED;
    nodes[3].text_offset=0; nodes[3].text_length=length;
    nodes[3].flags &= ~OS_CONTROL_BACKGROUND_ALPHA;
    nodes[3].background=(g_edit_mode!=FM4_EDIT_NONE)&&g_edit_select_all ?
        OS_THEME_SELECTED : OS_THEME_FIELD;
    if (searching) {
        nodes[4].flags &= ~OS_CONTROL_BACKGROUND_ALPHA;
        nodes[4].background=OS_THEME_ACCENT;
        nodes[4].foreground=OS_THEME_ON_ACCENT;
    }
    nodes[8] = (os_control_node_t){.identifier=3U, .parent=0U,
        .kind=OS_CONTROL_CONTAINER, .flags=OS_CONTROL_POINTER_PASSTHROUGH,
        .y=(int32_t)caption, .width=OS_CONTROL_FILL, .height=OS_CONTROL_FILL, .background=OS_THEME_SURFACE};
    uint32_t text_size=length;
    uint32_t count=FM4_FRAME_BASE_COUNT;
    uint32_t rows = g_place_count + g_volume_count;
    if (count + FM4_MENU_NODE_COUNT + FM4_DIALOG_NODE_COUNT + 1U > FM4_FRAME_NODE_COUNT) return false;
    nodes[9] = (os_control_node_t){.identifier=32U, .parent=0U, .kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_CLIP_CHILDREN,
        .width=(int32_t)FM4_SIDEBAR_WIDTH, .height=OS_CONTROL_FILL,
        .background=OS_THEME_TOOLBAR};
    for (uint32_t i=0; i<rows; ++i) {
        bool place = i < g_place_count;
        uint32_t index = place ? i : i - g_place_count;
        uint32_t row = 10U + 2U*i;
        uint32_t id = (place ? 256U : 512U) + index;
        bool active = place ? fm4_compare(g_path,g_places[index].path)==0 :
                              fm4_volume_path_active(&g_volumes[index]);
        nodes[row] = (os_control_node_t){.identifier=id, .parent=9U, .kind=OS_CONTROL_LIST_ITEM,
            .flags=active ? OS_CONTROL_CHECKED : 0U,
            .x=place ? 6 : 10,
            .y=(int32_t)(caption+(place ? 10U : FM4_VOLUME_FIRST_Y) + index*FM4_VOLUME_ROW_HEIGHT),
            .width=(int32_t)FM4_SIDEBAR_WIDTH-(place ? 12 : 20),
            .height=(int32_t)FM4_VOLUME_ROW_HEIGHT-4, .padding=FM4_SCALE(38U),
            .font_size=ui_text_size(), .foreground=OS_THEME_TEXT,
            .background=OS_THEME_TOOLBAR, .radius=6U,
            .user_data=(place ? FM4_CONTROL_PLACE_BASE : FM4_CONTROL_VOLUME_BASE)+index};
        if (!fm4_frame_text(&nodes[row],text,&text_size,
                            place ? g_places[index].label : g_volumes[index].label)) return false;
        nodes[row+1U] = (os_control_node_t){.identifier=id+1024U, .parent=9U, .kind=OS_CONTROL_BUTTON,
            .flags=OS_CONTROL_SYMBOL|OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_POINTER_PASSTHROUGH,
            .x=nodes[row].x+(int32_t)FM4_SCALE(8U),
            .y=nodes[row].y+(nodes[row].height-(int32_t)ui_text_size())/2,
            .width=(int32_t)ui_text_size(), .height=(int32_t)ui_text_size(),
            .font_size=ui_text_size(), .foreground=OS_THEME_TEXT,
            .value=place && index==0 ? OS_CONTROL_SYMBOL_HOME : OS_CONTROL_SYMBOL_DRIVE};
    }
    nodes[10U+2U*rows] = (os_control_node_t){.identifier=33U, .parent=9U,
        .kind=OS_CONTROL_SEPARATOR, .flags=OS_CONTROL_VERTICAL,
        .x=(int32_t)FM4_SIDEBAR_WIDTH-1, .width=1, .height=OS_CONTROL_FILL,
        .border_color=OS_THEME_BORDER};
    nodes[11U+2U*rows] = (os_control_node_t){.identifier=34U, .parent=9U,
        .kind=OS_CONTROL_SEPARATOR, .flags=g_volume_count ? 0U : OS_CONTROL_HIDDEN,
        .x=7, .y=(int32_t)(caption+FM4_VOLUME_FIRST_Y)-12,
        .width=(int32_t)FM4_SIDEBAR_WIDTH-14, .height=1, .border_color=OS_THEME_BORDER};
    /* 仅提交可见条目，滚动条与状态层排在文件内容之后。 */
    uint32_t scroll_index=12U+2U*rows;
    /* 侧栏从标题栏延伸至底部，标题作为统一文字控件绘制，不另画应用装饰。 */
    nodes[scroll_index]=(os_control_node_t){.identifier=35U,.parent=9U,.kind=OS_CONTROL_TEXT,
        .flags=OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_BACKGROUND_ALPHA,
        .x=12,.y=(int32_t)((caption-ui_text_size())/2U),
        .width=(int32_t)FM4_SIDEBAR_WIDTH-24,.height=(int32_t)ui_text_size(),
        .font_size=ui_text_size(),.foreground=OS_THEME_TEXT};
    if (!fm4_frame_text(&nodes[scroll_index++],text,&text_size,"文件管理器")) return false;
    if (!fm4_append_content(nodes,text,&scroll_index,&text_size)) return false;
    count=scroll_index+3U;
    if (count+FM4_MENU_NODE_COUNT+FM4_DIALOG_NODE_COUNT>FM4_FRAME_NODE_COUNT) return false;
    uint32_t maximum=fm4_max_scroll_row(), total=fm4_total_rows();
    uint32_t track_height=g_window.height>24U ? g_window.height-24U : 0U;
    nodes[scroll_index]=(os_control_node_t){.identifier=40U,.parent=0U,.kind=OS_CONTROL_SCROLLBAR,
        .flags=OS_CONTROL_VERTICAL|(!maximum ? OS_CONTROL_HIDDEN : 0U)|
            (g_context_visible ? OS_CONTROL_DISABLED : 0U),
        .x=g_window.width>14U ? (int32_t)g_window.width-14 : 0,
        .y=(int32_t)caption+8,.width=12,.height=(int32_t)track_height,
        .background=OS_THEME_SURFACE,.foreground=OS_THEME_MUTED,
        .value=maximum ? (uint32_t)((uint64_t)(g_first_entry/fm4_columns())*OS_CONTROL_VALUE_MAX/maximum) : 0U,
        .page_size=maximum ? (uint32_t)((uint64_t)fm4_visible_rows()*OS_CONTROL_VALUE_MAX/total) : OS_CONTROL_VALUE_MAX};
    uint32_t status_panel=scroll_index+1U;
    bool show_status=g_status[0] && strcmp(g_status,"READY")!=0;
    uint32_t status_x=FM4_SIDEBAR_WIDTH+16U;
    uint32_t status_width=g_window.width>status_x+10U ? g_window.width-status_x-10U : 0U;
    uint32_t status_height=ui_text_line_height()+10U;
    uint32_t measured=show_status ? fm4_measure(g_status)+22U : 0U;
    if (status_width>measured) status_width=measured;
    uint32_t status_y=g_window.height>status_height+12U ? g_window.height-status_height-12U : 0U;
    nodes[status_panel]=(os_control_node_t){.identifier=48U, .parent=0U, .kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_POINTER_PASSTHROUGH|(!show_status ? OS_CONTROL_HIDDEN : 0U),
        .x=(int32_t)status_x, .y=(int32_t)(caption+status_y),
        .width=(int32_t)status_width, .height=(int32_t)status_height,
        .background=OS_THEME_FIELD, .radius=6U};
    nodes[status_panel+1U]=(os_control_node_t){.identifier=49U, .parent=status_panel, .kind=OS_CONTROL_TEXT,
        .flags=OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_BACKGROUND_ALPHA,
        .x=11, .y=((int32_t)status_height-(int32_t)ui_text_size())/2,
        .width=status_width>22U ? (int32_t)(status_width-22U) : 0,
        .height=(int32_t)ui_text_size(), .font_size=ui_text_size(), .foreground=OS_THEME_MUTED};
    if (show_status && !fm4_frame_text(&nodes[status_panel+1U],text,&text_size,g_status)) return false;
    if (!user_caption_tree(nodes,&count,FM4_FRAME_NODE_COUNT,text,&text_size,FM4_FRAME_TEXT_CAPACITY,
            (user_caption_layout_t){.width=g_window.width,.height=caption,.font=ui_text_size()})) return false;
    if (g_context_visible) {
        static const char *const labels[]={"打开","删除…","属性","重命名…","复制","剪切","粘贴","新建文件夹…"};
        /* 菜单在内容宿主之后绘制；条目输入与悬停直接由合成器路由。 */
        uint32_t panel=count++;
        nodes[panel]=(os_control_node_t){.identifier=63U,.parent=0U,.kind=OS_CONTROL_CONTAINER,
            .flags=OS_CONTROL_MENU_PANEL,.font_size=ui_text_size(),
            .x=(int32_t)g_context_x,.y=(int32_t)(caption+g_context_y),
            .width=FM4_CONTEXT_WIDTH};
        for (uint32_t i=0;i<FM4_CONTEXT_ROWS;++i) {
            nodes[count]=(os_control_node_t){.identifier=64U+i,.parent=panel,.kind=OS_CONTROL_LIST_ITEM,
                .flags=g_context_keyboard ? OS_CONTROL_NO_HOVER|
                    (g_context_hover==FM4_CONTROL_CONTEXT_OPEN+i ? OS_CONTROL_CHECKED : 0) : 0,
                .font_size=ui_text_size(),
                .foreground=i==1U ? OS_THEME_DANGER : OS_THEME_TEXT,
                .user_data=FM4_CONTROL_CONTEXT_OPEN+i};
            if (!fm4_frame_text(&nodes[count++],text,&text_size,labels[i])) return false;
        }
    }
    if (g_delete_message.active) {
        ui_message_document_t modal;
        if(!ui_message_build(&g_delete_message,g_window.identifier,g_window.width,g_window.height,&modal) ||
           !ui_modal_tree_attach(nodes,&count,FM4_FRAME_NODE_COUNT,text,&text_size,
               FM4_FRAME_TEXT_CAPACITY,&modal.request))return false;
    }
    /* 普通鼠标悬停不重新提交文档；只有菜单状态或键盘选项变化才发布。 */
    if (g_frame_tree.revision && g_frame_tree.count==count && !memcmp(nodes,g_frame->nodes,count*sizeof(*nodes)) &&
        g_frame_tree.text_size==text_size && !memcmp(text,g_frame->text,text_size)) {
        g_frame_input_ready=true;return true;
    }
    os_window_control_tree_t request = {.hdr={sizeof(request), OS_SYSCALL_ABI_VERSION},
        .identifier=g_window.identifier, .count=count, .nodes=(uint64_t)(uintptr_t)nodes,
        .text=(uint64_t)(uintptr_t)text, .text_size=text_size};
    if (fileman_syscall_one(OS_SYS_WINDOW_CONTROL_TREE, (uint64_t)(uintptr_t)&request) != 0) return false;
    /* 内核已同步复制提交数据。成功后直接交换缓冲区，旧界面成为下次候选；
     * 失败及幂等提交均不交换，事件索引始终绑定最后成功发布的快照。 */
    fm4_frame_buffer_t *previous=g_frame;
    g_frame=g_candidate;
    g_candidate=previous;
    g_frame_tree=request;
    g_frame_input_ready=true;
    return true;
}

static bool create_window(void) {
    os_display_info_t display = {0};
    os_window_create_t request = {0};

    uint32_t width;
    uint32_t height;

    display.hdr.size =
        sizeof(display);

    display.hdr.version =
        OS_SYSCALL_ABI_VERSION;

    if (fileman_syscall_one(
            OS_SYS_DISPLAY_GET_INFO,
            (uint64_t)&display) < 0 ||
        display.width < 320U ||
        display.height < 240U) {

        return false;
    }

    /* 初始窗口应明显处于非最大化居中状态。服务端仍预留与显示器等大的可调整表面，
     * 但可见的 Files 窗口宽高各为显示区域的 75%，从而留出可用桌面边距并使四条
     * 调整边缘都可触及。 */
    width = display.width * 3U / 4U;
    height = display.height * 3U / 4U;
    if (width < 640U && display.width > 640U) width = 640U;
    if (height < 420U && display.height > 420U) height = 420U;

    request.hdr.size =
        sizeof(request);

    request.hdr.version =
        OS_SYSCALL_ABI_VERSION;

    if (width > 960U) width = 960U;
    if (height > 640U) height = 640U;
    request.x = (int32_t)((display.width - width) / 2U);
    request.y = (int32_t)((display.height - height) / 2U);

    request.width = width;
    request.height = height;

    request.flags = OS_WINDOW_VISIBLE |
                    OS_WINDOW_RESIZABLE |
                    OS_WINDOW_STANDARD_CAPTION_CONTROLS |
                    OS_WINDOW_CAPTION_HEIGHT(user_caption_height());

    request.background = OS_THEME_SURFACE;

    user_caption_format_title(request.title, "文件管理器", false);

    request.address =
        FILEMAN_MAP_BASE;

    if (fileman_syscall_one(
            OS_SYS_WINDOW_CREATE,
            (uint64_t)&request) != 0 ||
        request.window ==
            OS_INVALID_HANDLE ||
        request.address == 0U) {

        return false;
    }

    g_window.handle =
        request.window;

    g_window.identifier =
        request.identifier;

    g_window.width =
        request.width;

    g_window.height =
        request.height;

    g_window.pixels =
        (uint32_t *)(uintptr_t)
            request.address;

    return true;
}

static void update_window(void) {
    uint32_t caption=user_caption_height();
    if (g_caption_height==caption) return;
    os_window_update_t request={.hdr={sizeof(request),OS_SYSCALL_ABI_VERSION},
        .identifier=g_window.identifier,.flags=OS_WINDOW_CAPTION_HEIGHT(caption)};
    if (!fileman_syscall_one(OS_SYS_WINDOW_UPDATE,(uintptr_t)&request)) g_caption_height=caption;
}


static void render(void) {
    fm4_normalize_scroll();
    if (!g_window.width || !g_window.height) return;
    if (fm4_publish_frame()) update_window();
}

static void open_selected(void) {
    if (!g_selection_valid ||
        g_entry_count == 0U ||
        g_selected >= g_entry_count) {

        return;
    }

    if (g_entries[g_selected]->info.type ==
        OS_FILE_TYPE_DIRECTORY) {

        if (!fm4_navigate(
                g_entries[g_selected]->path,
                true)) {

            fm4_status(
                "OPEN DIRECTORY FAILED");
        }

        return;
    }

    {
        char *arguments[3];

        arguments[0] =
            (char *)g_notepad_name;

        arguments[1] =
            g_entries[g_selected]->path;

        arguments[2] = 0;

        int64_t child_pid = fileman_syscall_one(OS_SYS_PROCESS_FORK, 0U);
        if (child_pid == 0) {
            int64_t status = fileman_syscall_three(
                OS_SYS_PROCESS_EXEC,
                (uint64_t)(uintptr_t)g_notepad_path,
                (uint64_t)(uintptr_t)arguments,
                0U);
            fileman_exit(status < 0 ? 1U : 0U);
        }
        if (child_pid < 0) {
            fm4_status("NOTEPAD LAUNCH FAILED");
        } else {
            fm4_status("OPENED IN NOTEPAD");
        }
    }
}


static bool fm4_remove_now(void) {
    os_file_path_op_t request = {0};

    if (!g_delete_path[0]) return false;

    request.hdr.size =
        sizeof(request);

    request.hdr.version =
        OS_SYSCALL_ABI_VERSION;

    request.path =
        (uint64_t)(uintptr_t)
            g_delete_path;

    int64_t status = fileman_syscall_one(
            OS_SYS_FILE_REMOVE,
            (uint64_t)&request);
    if (status < 0) {
        const char *reason;
        switch (status) {
        case -ENOENT: reason = "项目已不存在，请刷新文件夹。"; break;
        case -EBUSY: reason = "项目正在使用中，请关闭相关应用后重试。"; break;
        case -EACCES: case -EPERM: reason = "没有删除此项目的权限。"; break;
        case -EROFS: reason = "所在文件系统为只读，无法删除。"; break;
        case -ENOTEMPTY: reason = "文件夹不为空，请先删除其中的内容。"; break;
        case -EIO: reason = "文件系统读写失败，未能完成删除。"; break;
        case -ENOMEM: reason = "内存不足，未能完成删除。"; break;
        default: reason = "删除失败，请刷新文件夹后重试。"; break;
        }
        fm4_status(reason);
        ui_message_show(&g_delete_message, "无法删除", reason, OS_MESSAGE_OK);
        return false;
    }

    g_selection_valid = false;
    g_first_entry = 0U;

    if (!load_directory()) {
        fm4_status(
            "REFRESH FAILED");

        return false;
    }

    fm4_status("READY");

    return true;
}


static void remove_selected(void) {
    if (!g_selection_valid ||
        g_selected >= g_entry_count) {

        return;
    }

    fm4_hide_context();
    /* 确认期间即使后台复制刷新列表，删除目标也不能跟着选择索引变化。 */
    copy_text(g_delete_path, sizeof(g_delete_path), g_entries[g_selected]->path);
    ui_message_show(&g_delete_message,"要永久删除这个项目吗？",
        g_entries[g_selected]->info.name,OS_MESSAGE_DELETE_CANCEL);
}

/* 公共消息框负责会话校验和按钮语义；文件操作仍只使用打开弹窗时锁定的路径。 */
static void fm4_complete_delete(enum ui_message_result result) {
    if(result==OS_MESSAGE_NONE)return;
    /* 错误提示的确定按钮只关闭提示，不能再次触发删除。 */
    bool confirmed=g_delete_message.buttons==OS_MESSAGE_DELETE_CANCEL && result==OS_MESSAGE_ACCEPT;
    g_delete_message.active=false;
    if(confirmed)(void)fm4_remove_now();
    g_delete_path[0]='\0';
    render();
}

static void fm4_begin_name(uint32_t action) {
    if (action == 2U && (!g_selection_valid || g_selected >= g_entry_count)) return;
    g_name_action = action;
    g_edit_mode = FM4_EDIT_LOCATION;
    g_edit_select_all = true;
    copy_text(g_location, sizeof(g_location), action == 1U ? "New Folder" : g_entries[g_selected]->info.name);
    if (action == 2U) copy_text(g_rename_source, sizeof(g_rename_source), g_entries[g_selected]->path);
    fm4_status(action == 1U ? "输入文件夹名称，按回车创建" : "输入新名称，按回车重命名");
}

static bool fm4_finish_name(void) {
    char path[FILEMAN_PATH_CAPACITY];
    os_file_info_t info;
    if (!g_location[0] || fm4_compare(g_location, ".") == 0 ||
        fm4_compare(g_location, "..") == 0) return false;
    for (uint32_t i = 0U; g_location[i]; ++i) if (g_location[i] == '/') return false;
    if (!join_path(path, g_path, g_location)) return false;
    /* 不覆盖同名文件；无论重命名还是创建都先显式检查冲突。 */
    if (stat_path(path, &info) != -2) return false;
    int result = g_name_action == 1U ? mkdir(path, 0755U) : rename(g_rename_source, path);
    if (result != 0) return false;
    g_query[0] = '\0';
    g_selection_valid = false;
    return load_directory();
}

static void fm4_copy_selection(bool cut) {
    if (!g_selection_valid || g_selected >= g_entry_count) return;
    copy_text(g_clipboard, sizeof(g_clipboard), g_entries[g_selected]->path);
    g_clipboard_cut = cut;
    fm4_status(cut ? "已剪切，选择目标文件夹后粘贴" : "已复制，选择目标文件夹后粘贴");
}

static void *fm4_copy_worker(void *unused) {
    (void)unused;
    g_copy_error = fm_copy_run(&g_copy_context, g_copy_source, g_copy_destination);
    atomic_store_explicit(&g_copy_finished, true, memory_order_release);
    return 0;
}

/* 只有界面线程发布状态和重载目录；工作线程不接触窗口或导航数据。 */
static void fm4_copy_poll(void) {
    if (!g_copy_active) return;
    if (atomic_load_explicit(&g_copy_finished, memory_order_acquire)) {
        if (pthread_join(g_copy_thread, 0) != 0) return;
        g_copy_active = false;
        if (g_close_after_copy) fileman_exit(0U);
        (void)load_directory();
        if (!g_copy_error) fm4_status("复制完成");
        else if (g_copy_error == ECANCELED) fm4_status("已取消复制，已写入的内容保留");
        else if (g_copy_error == EEXIST) fm4_status("复制失败：目标已存在，未覆盖");
        else if (g_copy_error == EINVAL) fm4_status("不能复制到原位置或文件夹自身内部");
        else fm4_status("复制未完成，已写入的内容保留");
        render();
        return;
    }
    uint64_t bytes = atomic_load_explicit(&g_copy_context.bytes, memory_order_relaxed);
    if (bytes != g_copy_displayed_bytes) {
        char status[128] = "正在复制：";
        g_copy_displayed_bytes = bytes;
        append_decimal(status, sizeof(status), bytes / 1024U);
        append_text(status, sizeof(status), " KiB，按 Esc 取消");
        fm4_status(status);
        render();
    }
}

static void fm4_request_close(void) {
    if (!g_copy_active) fileman_exit(0U);
    g_close_after_copy = true;
    atomic_store_explicit(&g_copy_context.cancel, true, memory_order_relaxed);
    fm4_status("正在取消复制并关闭窗口…");
    render();
}

static bool fm4_paste(void) {
    char destination[FILEMAN_PATH_CAPACITY];
    os_file_info_t info;
    uint32_t start = 0U;
    if (!g_clipboard[0] || g_copy_active) return false;
    for (uint32_t i = 0U; g_clipboard[i]; ++i) if (g_clipboard[i] == '/') start = i + 1U;
    if (!join_path(destination, g_path, g_clipboard + start) ||
        stat_path(destination, &info) != -2) return false;
    if (g_clipboard_cut) {
        if (rename(g_clipboard, destination) != 0) return false;
        g_clipboard[0] = '\0';
    } else {
        copy_text(g_copy_source, sizeof(g_copy_source), g_clipboard);
        copy_text(g_copy_destination, sizeof(g_copy_destination), destination);
        atomic_store_explicit(&g_copy_context.cancel, false, memory_order_relaxed);
        atomic_store_explicit(&g_copy_context.bytes, 0U, memory_order_relaxed);
        atomic_store_explicit(&g_copy_finished, false, memory_order_relaxed);
        g_copy_displayed_bytes = 0U;
        g_copy_active = true;
        if (pthread_create(&g_copy_thread, 0, fm4_copy_worker, 0) != 0) {
            g_copy_active = false;
            return false;
        }
        fm4_status("正在复制，按 Esc 取消；仍可浏览其他文件夹");
        return true;
    }
    return load_directory();
}

/* 鼠标与键盘共用同一菜单动作入口，先关闭弹层，再执行文件操作。 */
static void fm4_activate_context(uint32_t control) {
    fm4_hide_context();
    switch (control) {
    case FM4_CONTROL_CONTEXT_OPEN: open_selected(); break;
    case FM4_CONTROL_CONTEXT_DELETE: remove_selected(); break;
    case FM4_CONTROL_CONTEXT_RENAME: fm4_begin_name(2U); break;
    case FM4_CONTROL_CONTEXT_COPY: fm4_copy_selection(false); break;
    case FM4_CONTROL_CONTEXT_CUT: fm4_copy_selection(true); break;
    case FM4_CONTROL_CONTEXT_PASTE:
        if (!fm4_paste()) fm4_status("无法粘贴：目标已存在或文件无法复制");
        break;
    case FM4_CONTROL_CONTEXT_NEW_FOLDER: fm4_begin_name(1U); break;
    case FM4_CONTROL_CONTEXT_PROPERTIES:
        if (g_selection_valid && g_selected < g_entry_count) {
            if (g_entries[g_selected]->info.type == OS_FILE_TYPE_DIRECTORY) {
                fm4_status("文件夹");
            } else {
                char message[128] = "文件  ";
                append_decimal(message, sizeof(message), g_entries[g_selected]->info.size);
                append_text(message, sizeof(message), " B");
                fm4_status(message);
            }
        }
        break;
    default:
        render();
        return;
    }
    render();
}

static void handle_key(
    const os_window_event_t *event) {

    const os_input_event_t *input =
        event != 0 ?
        &event->input :
        0;

    uint32_t columns =
        fm4_columns();

    uint32_t page =
        columns *
        fm4_visible_rows();

    if (event == 0 ||
        event->type !=
            OS_WINDOW_EVENT_INPUT ||
        input == 0 ||
        input->type !=
            OS_INPUT_EVENT_KEY) {

        return;
    }

    if (input->code == 0xE0U ||
        input->code == 0xE4U) {

        g_ctrl =
            input->value !=
            OS_INPUT_VALUE_RELEASE;

        return;
    }

    if (input->code == 0xE1U || input->code == 0xE5U) {
        g_shift = input->value != OS_INPUT_VALUE_RELEASE;
        return;
    }

    if (input->value ==
        OS_INPUT_VALUE_RELEASE) {

        return;
    }

    /* 焦点移动与回车由服务端处理，原始按键只保留安全取消，不能穿透。 */
    if (g_delete_message.active) {
        if(input->code==0x29U)fm4_complete_delete(OS_MESSAGE_CANCEL);
        return;
    }

    /* 弹出菜单先接管键盘，不能让方向键、删除键或回车穿透到文件列表。 */
    if (g_context_visible) {
        uint32_t next = g_context_hover;
        if (input->code == 0x29U) {
            fm4_hide_context();
            render();
        } else if (input->code == 0x28U || input->code == 0x58U) {
            fm4_activate_context(next);
        } else {
            bool selected = next >= FM4_CONTROL_CONTEXT_OPEN &&
                next < FM4_CONTROL_CONTEXT_OPEN + FM4_CONTEXT_ROWS;
            if (input->code == 0x52U)
                next = !selected || next == FM4_CONTROL_CONTEXT_OPEN ?
                    FM4_CONTROL_CONTEXT_OPEN + FM4_CONTEXT_ROWS - 1U : next - 1U;
            else if (input->code == 0x51U)
                next = !selected || next == FM4_CONTROL_CONTEXT_OPEN + FM4_CONTEXT_ROWS - 1U ?
                    FM4_CONTROL_CONTEXT_OPEN : next + 1U;
            else if (input->code == 0x4AU) next = FM4_CONTROL_CONTEXT_OPEN;
            else if (input->code == 0x4DU) next = FM4_CONTROL_CONTEXT_OPEN + FM4_CONTEXT_ROWS - 1U;
            bool navigation=input->code==0x52U || input->code==0x51U || input->code==0x4aU || input->code==0x4dU;
            if (navigation && (next != g_context_hover || !g_context_keyboard)) {
                g_context_keyboard = true;
                g_context_hover = next;
                (void)fm4_publish_frame();
            }
        }
        return;
    }

    if (g_copy_active && input->code == 0x29U) {
        atomic_store_explicit(&g_copy_context.cancel, true, memory_order_relaxed);
        fm4_status("正在取消复制，已写入的内容将保留");
        render(); return;
    }

    if (g_ctrl && (input->code == 0x0FU || input->code == 0x09U)) {
        g_name_action = 0U;
        g_edit_mode = input->code == 0x09U ? FM4_EDIT_SEARCH : FM4_EDIT_LOCATION;
        copy_text(g_location, sizeof(g_location), g_path);
        g_edit_select_all = true;
        render();
        return;
    }

    if (g_edit_mode != FM4_EDIT_NONE) {
        char *buffer = g_edit_mode == FM4_EDIT_SEARCH ? g_query : g_location;
        uint32_t capacity = g_edit_mode == FM4_EDIT_SEARCH ? sizeof(g_query) : sizeof(g_location);
        uint32_t length = text_length(buffer);
        bool changed = false;
        if (input->code == 0x29U) {
            g_edit_mode = FM4_EDIT_NONE;
            g_name_action = 0U;
            g_query[0] = '\0';
            fm4_apply_filter();
        } else if (input->code == 0x28U || input->code == 0x58U) {
            if (g_name_action) {
                if (!fm4_finish_name()) {
                    fm4_status("操作失败：请检查名称、同名项目及写入权限");
                    render();
                    return;
                }
                g_name_action = 0U;
            } else if (g_edit_mode == FM4_EDIT_LOCATION) {
                if (!fm4_navigate(g_location, true)) fm4_status("无法打开该位置");
            }
            /* 搜索回车只结束编辑，保留筛选结果，允许方向键选择文件。 */
            g_edit_mode = FM4_EDIT_NONE;
        } else if (g_ctrl && input->code == 0x04U) {
            g_edit_select_all = true;
        } else if (!g_ctrl) {
            char character = '\0';
            if (input->code >= 0x04U && input->code <= 0x1DU)
                character = (g_shift ? 'A' : 'a') + input->code - 0x04U;
            else if (input->code >= 0x1EU && input->code <= 0x26U)
                character = '1' + input->code - 0x1EU;
            else if (input->code == 0x27U) character = '0';
            else if (input->code == 0x2CU) character = ' ';
            else if (input->code == 0x2DU) character = g_shift ? '_' : '-';
            else if (input->code == 0x37U) character = '.';
            else if (input->code == 0x38U) character = '/';
            if (character || input->code == 0x2AU) {
                if (g_edit_select_all) buffer[length = 0U] = '\0';
                g_edit_select_all = false;
                if (input->code == 0x2AU) {
                    /* 按完整 UTF-8 字符回退，避免截断目录名。 */
                    if (length) {
                        do { --length; } while (length && ((unsigned char)buffer[length] & 0xC0U) == 0x80U);
                        buffer[length] = '\0';
                    }
                } else if (length + 1U < capacity) {
                    buffer[length] = character;
                    buffer[length + 1U] = '\0';
                }
                changed = true;
            }
        }
        if (changed && g_edit_mode == FM4_EDIT_SEARCH) {
            fm4_apply_filter();
        }
        render();
        return;
    }

    if ((g_ctrl && g_shift && input->code == 0x11U) || input->code == 0x3BU) {
        fm4_begin_name(input->code == 0x3BU ? 2U : 1U);
        render(); return;
    }
    if (g_ctrl && (input->code == 0x06U || input->code == 0x1BU)) {
        fm4_copy_selection(input->code == 0x1BU);
        render(); return;
    }
    if (g_ctrl && input->code == 0x19U) {
        if (!fm4_paste()) fm4_status("无法粘贴：目标已存在或文件无法复制");
        render(); return;
    }

    if (g_ctrl && input->code == 0x2EU) {
        (void)ui_text_adjust(1);
        render();
        return;
    }

    if (g_ctrl && input->code == 0x2DU) {
        (void)ui_text_adjust(-1);
        render();
        return;
    }

    if (g_ctrl &&
        (input->code == 0x14U ||
         input->code == (uint32_t)'Q')) {

        fm4_request_close();
        return;
    }

    if ((g_ctrl &&
         (input->code == 0x15U ||
          input->code == (uint32_t)'R')) ||
        input->code == 0x3EU) {

        fm4_hide_context();
        g_delete_message.active = false;

        fm4_load_volumes();
        (void)load_directory();
        render();
        return;
    }

    /* 先退出仍生效的筛选，再处理普通文件选择。 */
    if (input->code == 0x29U) {
        if (g_query[0]) {
            g_query[0]='\0';
            fm4_apply_filter();
        } else g_selection_valid = false;

        fm4_status("READY");

        render();
        return;
    }

    if (input->code == 0x2AU) {
        fm4_parent();
        render();
        return;
    }

    if (input->code == 0x28U ||
        input->code == 0x58U) {

        open_selected();
        render();
        return;
    }

    if (input->code == 0x4CU) {
        remove_selected();
        render();
        return;
    }

    if (g_entry_count == 0U) {
        return;
    }

    if (!g_selection_valid) {
        g_selection_valid = true;
        g_selected = 0U;

        fm4_selection_visible();
        render();
        return;
    }

    /*
    * 网格键盘导航。
     */
    if (input->code == 0x50U) {
        if (g_selected != 0U)
            --g_selected;

    } else if (
        input->code == 0x4FU) {

        if (g_selected + 1U <
            g_entry_count) {

            ++g_selected;
        }

    } else if (
        input->code == 0x52U) {

        if (g_selected >= columns) {
            g_selected -= columns;
        }

    } else if (
        input->code == 0x51U) {

        if (g_selected + columns <
            g_entry_count) {

            g_selected += columns;
        }

    } else if (
        input->code == 0x4AU) {

        g_selected = 0U;

    } else if (
        input->code == 0x4DU) {

        g_selected =
            g_entry_count - 1U;

    } else if (
        input->code == 0x4BU) {

        g_selected =
            g_selected > page ?
            g_selected - page :
            0U;

    } else if (
        input->code == 0x4EU) {

        uint32_t maximum =
            g_entry_count - 1U;

        if (page >
            maximum -
            (g_selected < maximum ?
             g_selected :
             maximum)) {

            g_selected =
                maximum;

        } else {
            g_selected += page;
        }

    } else {
        return;
    }

    fm4_hide_context();

    fm4_selection_visible();

    render();
}

static void fm4_activate_control(uint32_t control) {
    fm4_hide_context();
    if (control == FM4_CONTROL_BACK) {
        (void)fm4_history_go(-1);
    } else if (control == FM4_CONTROL_FORWARD) {
        (void)fm4_history_go(1);
    } else if (control == FM4_CONTROL_SEARCH) {
        g_name_action = 0U;
        bool cancel = g_edit_mode == FM4_EDIT_SEARCH || g_query[0];
        g_edit_mode = cancel ? FM4_EDIT_NONE : FM4_EDIT_SEARCH;
        g_edit_select_all = !cancel;
        if (cancel) {
            g_query[0] = '\0';
            fm4_apply_filter();
        }
    } else if (control == FM4_CONTROL_VIEW) {
        g_list_view = !g_list_view;
        g_first_entry = 0U;
        fm4_selection_visible();
    } else if (control == FM4_CONTROL_SORT) {
        g_sort_descending = !g_sort_descending;
        g_selection_valid = false;
        fm4_sort();
    } else if (control == FM4_CONTROL_LOCATION) {
        g_name_action = 0U;
        g_edit_mode = g_query[0] ? FM4_EDIT_SEARCH : FM4_EDIT_LOCATION;
        copy_text(g_location, sizeof(g_location), g_path);
        g_edit_select_all = true;
    } else if (control >= FM4_CONTROL_PLACE_BASE && control < FM4_CONTROL_PLACE_BASE + g_place_count) {
        (void)fm4_navigate(g_places[control - FM4_CONTROL_PLACE_BASE].path, true);
    } else if (control >= FM4_CONTROL_VOLUME_BASE && control < FM4_CONTROL_VOLUME_BASE + g_volume_count) {
        (void)fm4_navigate(g_volumes[control - FM4_CONTROL_VOLUME_BASE].path, true);
    }
    render();
}

/* 使用事件快照携带的目录索引，不对同一次控件点击再做几何搜索。 */
static void fm4_select_item(uint32_t item,uint64_t timestamp) {
    if (g_context_visible) {fm4_hide_context();render();return;}
    /* 文件区域取得键盘操作权；保留筛选词，但不再让输入框吞掉删除和方向键。 */
    g_edit_mode=FM4_EDIT_NONE;
    g_name_action=0U;
    g_edit_select_all=false;
    if (item>=g_entry_count) {
        g_selection_valid=false;g_last_click_item=FM4_INDEX_NONE;
        fm4_status("READY");render();return;
    }
    g_selected=item;g_selection_valid=true;
    if (g_last_click_item==item && timestamp>=g_last_click_time &&
        timestamp-g_last_click_time<=FM4_DOUBLE_CLICK_TSC_TICKS) {
        g_last_click_item=FM4_INDEX_NONE;g_last_click_time=0;
        open_selected();render();return;
    }
    g_last_click_item=item;g_last_click_time=timestamp;
    fm4_status("READY");render();
}

static void handle_event(
    const os_window_event_t *event) {

    uint64_t pixels;

    if (event == 0) {
        return;
    }

    if (event->type == OS_WINDOW_EVENT_CONTROL) {
        /* 发布失败时屏幕仍显示旧快照，不能将旧索引解释成新目录里的项目。 */
        if (!g_frame_input_ready) {g_ctrl=g_shift=false;render();return;}
        const os_control_node_t *node = os_control_event_node(&g_frame_tree, g_frame->nodes, &event->control);
        /* 根键盘目标在刷新中保持稳定，旧队列里的修饰键释放仍须消费。 */
        bool modifier=event->control.action==OS_CONTROL_KEY &&
            (event->control.code==0xE0U || event->control.code==0xE4U ||
             event->control.code==0xE1U || event->control.code==0xE5U);
        bool root_key=event->control.target==1U && event->control.node_index==0U &&
            event->control.node_kind==OS_CONTROL_CONTAINER && event->control.user_data==0U;
        if (!node && event->control.action==OS_CONTROL_KEY && (modifier || root_key) &&
            event->control.tree_revision &&
            event->control.tree_revision<=g_frame_tree.revision) node=g_frame->nodes;
        if (!node) return;
        /* 模态框独占语义事件，修饰键释放仍须更新应用状态。 */
        if(g_delete_message.active && !modifier) {
            fm4_complete_delete(ui_message_event(&g_delete_message,event));
            return;
        }
        if (event->control.action==OS_CONTROL_KEY) {
            os_window_event_t key=*event;key.type=OS_WINDOW_EVENT_INPUT;
            key.input=(os_input_event_t){.type=OS_INPUT_EVENT_KEY,
                .code=event->control.code,.value=event->control.value,.timestamp=event->control.timestamp};
            handle_key(&key);return;
        }
        if (event->control.action==OS_CONTROL_WHEEL && node->identifier!=40U) {
            os_window_event_t wheel=*event;wheel.type=OS_WINDOW_EVENT_INPUT;
            wheel.input=(os_input_event_t){.type=OS_INPUT_EVENT_RELATIVE,.code=OS_INPUT_REL_WHEEL,
                .value=event->control.value,.timestamp=event->control.timestamp};
            handle_event(&wheel);return;
        }
        if (node->identifier>=FM4_ITEM_ID_BASE && node->identifier<0x20000000U) {
            uint32_t item=(uint32_t)node->user_data;
            if (item>=g_entry_count || node->identifier!=FM4_ITEM_ID_BASE+item) return;
            if (event->control.action==OS_CONTROL_CLICK)
                fm4_select_item(item,event->control.timestamp);
            else if (event->control.action==OS_CONTROL_DOWN && event->control.code==OS_INPUT_BUTTON_RIGHT) {
                g_selected=item;g_selection_valid=true;
                fm4_show_context(event->pointer_x,event->pointer_y);render();
            }
            return;
        }
        if (node->identifier==40U) {
            if (g_context_visible) {
                if (event->control.action==OS_CONTROL_DOWN || event->control.action==OS_CONTROL_CLICK) {
                    fm4_hide_context();render();
                }
                return;
            }
            uint32_t previous=g_first_entry;
            if (event->control.action==OS_CONTROL_CHANGE) {
                int32_t value=event->control.value;
                if (value<0 || value>(int32_t)OS_CONTROL_VALUE_MAX) return;
                uint32_t row=(uint32_t)(((uint64_t)(uint32_t)value*fm4_max_scroll_row()+
                    OS_CONTROL_VALUE_MAX/2U)/OS_CONTROL_VALUE_MAX);
                g_first_entry=row*fm4_columns();
            } else if (event->control.action==OS_CONTROL_WHEEL) {
                fm4_scroll(event->control.value>0 ? -1 : event->control.value<0 ? 1 : 0);
            } else return;
            if (previous!=g_first_entry) {
                g_last_click_item=FM4_INDEX_NONE;
                g_last_click_time=0U;
                render();
            }
            return;
        }
        if (node && node->identifier>=64U && node->identifier<64U+FM4_CONTEXT_ROWS) {
            if (!g_context_visible) return;
            if (event->control.action==OS_CONTROL_CLICK) fm4_activate_context((uint32_t)node->user_data);
            else if (event->control.action==OS_CONTROL_ENTER || event->control.action==OS_CONTROL_MOVE ||
                     event->control.action==OS_CONTROL_DOWN) {
                g_context_hover=(uint32_t)node->user_data;
                if (g_context_keyboard) {g_context_keyboard=false;(void)fm4_publish_frame();}
            } else if (event->control.action==OS_CONTROL_LEAVE && !g_context_keyboard &&
                g_context_hover==node->user_data) g_context_hover=FM4_CONTROL_NONE;
            return;
        }
        if (node && event->control.action == OS_CONTROL_CLICK) {
            if (g_context_visible && node->identifier>=256U && node->identifier<768U) {
                fm4_hide_context();
                render();
                return;
            }
            if (node->identifier != 22U) { fm4_activate_control((uint32_t)node->user_data); return; }
            if (g_context_visible) fm4_hide_context();
            else { fm4_show_context((int32_t)g_window.width-190, 0); }
            render();
        }
        return;
    }

    if (event->type ==
        OS_WINDOW_EVENT_CLOSE_REQUEST) {
        fm4_request_close();
        return;
    }

    if (event->type ==
        OS_WINDOW_EVENT_RESIZE) {

        if (event->resize.width == 0U ||
            event->resize.height == 0U) {

            return;
        }

        pixels =
            (uint64_t)
            event->resize.width *
            event->resize.height;

        if (pixels >
            event->resize.buffer_size /
            sizeof(uint32_t)) {

            return;
        }

        g_window.width =
            event->resize.width;

        g_window.height =
            event->resize.height;

        fm4_normalize_scroll();
        fm4_selection_visible();

        fm4_hide_context();
        render();
        return;
    }

    if (event->type !=
        OS_WINDOW_EVENT_INPUT) {

        return;
    }
    if (!g_frame_input_ready) {g_ctrl=g_shift=false;render();return;}

    /*
    * Ring0 已将这些坐标转换为客户端局部坐标。
     */
    g_pointer_x =
        event->pointer_x;

    g_pointer_y =
        event->pointer_y;

    /* 模态框拦截所有客户区输入，右键、滚轮也不能穿透到底层文件。 */
    if (g_delete_message.active) {
        if (event->input.type == OS_INPUT_EVENT_KEY) handle_key(event);
        return;
    }

    /*
    * 鼠标移动/滚轮。
     */
    if (event->input.type ==
        OS_INPUT_EVENT_RELATIVE) {

        if (event->input.code ==
            OS_INPUT_REL_WHEEL) {

            if (g_ctrl) {
                (void)ui_text_adjust(event->input.value > 0 ? 1 : -1);
                render();
                return;
            }

            if (event->input.value > 0) {
                fm4_scroll(-1);

            } else if (
                event->input.value < 0) {

                fm4_scroll(1);
            }

            fm4_hide_context();
            render();
            return;
        }

        /* 悬停与按压反馈由公共控件处理，空白移动不重新提交文档。 */
        return;
    }

    /*
    * 键盘。
     */
    if (event->input.type ==
        OS_INPUT_EVENT_KEY) {

        handle_key(event);
        return;
    }

    if (event->input.type !=
        OS_INPUT_EVENT_BUTTON) {

        return;
    }

    if (event->input.value !=
        OS_INPUT_VALUE_PRESS) {

        return;
    }

    /*
    * 右键单击项目 -> 上下文菜单。
     */
    if (event->input.code ==
        OS_INPUT_BUTTON_RIGHT) {

        uint32_t item =
            fm4_item_at(
                g_pointer_x,
                g_pointer_y);

        g_selection_valid=item!=FM4_INDEX_NONE;
        if (g_selection_valid) g_selected=item;
        fm4_show_context(g_pointer_x,g_pointer_y);
        render();

        return;
    }

    if (event->input.code !=
        OS_INPUT_BUTTON_LEFT) {

        return;
    }


    /*
    * 上下文菜单。
     */
    if (g_context_visible) {
        /* 条目点击已由控件事件消费；收到原始点击说明位于菜单之外或边缘。 */
        fm4_hide_context();
        render();
        return;
    }


    fm4_select_item(fm4_item_at(g_pointer_x,g_pointer_y),event->input.timestamp);
}

int main(int argc, char **argv) {
    if (!ui_text_init(OS_TEXT_DEFAULT_SIZE)) {
        fileman_exit(1U);
    }
    fm4_load_places();
    fm4_load_volumes();
    copy_text(g_path, sizeof(g_path), g_home);
    if (argc > 1 && (argv == 0 || argv[1] == 0 || !set_path(argv[1]))) {
        fileman_exit(1U);
    }
    if (!create_window()) {
        fileman_exit(1U);
    }
    if (!fm4_publish_frame()) fileman_exit(1U);
    if (!load_directory()) {
        fileman_exit(1U);
    }

    fm4_history_reset();

    g_selection_valid = false;
    g_first_entry = 0U;


    g_context_visible = false;
    g_delete_message.active = false;

    fm4_status("READY");
    render();
#if OS_REALTEST
    (void)fprintf(stderr, "OS_FILEMAN_READY\n");
#endif
    for (;;) {
        fm4_copy_poll();
        os_window_event_read_t request = {0};
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_window.identifier;
        request.timeout_ns = g_copy_active || !g_frame_input_ready ? 50000000ULL : FILEMAN_EVENT_WAIT;
        int64_t status = fileman_syscall_one(OS_SYS_WINDOW_EVENT_READ,
                                             (uint64_t)&request);
        if (status == 0) handle_event(&request.event);
        else if (status != -11 && status != -110) __asm__ volatile ("pause");
        if (!g_frame_input_ready) render();
    }
}

__attribute__((noreturn)) void fileman_entry(void) {
    uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
    uint64_t *initial_stack = (uint64_t *)(frame + sizeof(uint64_t));
    int argc = (int)initial_stack[0];
    char **argv = (char **)(initial_stack + 1U);
    int status = main(argc, argv);
    fileman_exit(status < 0 ? 1U : (uint64_t)status);
}
