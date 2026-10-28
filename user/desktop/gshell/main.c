#include <stdint.h>
#include <stdbool.h>

#include <uapi/all.h>
#include <uapi/window_theme.h>

#include "../../runtime/text.h"
#include "../../runtime/caption.h"
#include "../../runtime/caption_tree.h"
#include "../../runtime/clipboard.h"

#define SHELL_LINE_CAPACITY 256U
#define SHELL_OUTPUT_LINES 128U
#define SHELL_MAP_BASE     0x06000000ULL
#define SHELL_EVENT_WAIT OS_WAIT_INFINITE
#define SHELL_FOREGROUND_WAIT_NS 20000000ULL
#define SHELL_OUTPUT_EVENT_BUDGET 32U
#define SHELL_BACKGROUND_WAIT_NS 100000000ULL
#define SHELL_BACKGROUND_REAP_BUDGET 32U
#define SHELL_HISTORY_LINES 16U
#define SHELL_MAX_ARGUMENTS 8U
#define SHELL_PATH_CAPACITY 256U
#define SHELL_DISPLAY_CAPACITY (SHELL_LINE_CAPACITY + SHELL_PATH_CAPACITY + 16U)
#define SHELL_CAPTION_TITLE "终端"
/* 背景和输入最多四个节点；标题栏需要根节点、标题和三个窗口按钮。 */
#define SHELL_TREE_FIXED_NODES (4U + 5U)
#define SHELL_WINDOW_BACKGROUND OS_THEME_SURFACE
#define SHELL_INPUT_BACKGROUND  OS_THEME_FIELD
#define SHELL_INPUT_BORDER      OS_THEME_BORDER
#define SHELL_OUTPUT_TEXT       OS_THEME_TEXT
#define SHELL_INPUT_TEXT        OS_THEME_TEXT
#define SHELL_CURSOR            OS_THEME_ACCENT

typedef struct shell_window {
    os_handle_t handle;
    uint32_t identifier;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
    const char *title;
    uint32_t color;
} shell_window_t;

typedef struct shell_launch_context {
    char path[SHELL_PATH_CAPACITY];
    char argument_storage[SHELL_MAX_ARGUMENTS][SHELL_LINE_CAPACITY];
    char *arguments[SHELL_MAX_ARGUMENTS + 1U];
    os_handle_t output_read;
    os_handle_t output_write;
    bool redirect_output;
} shell_launch_context_t;

static shell_window_t g_shell = {
    OS_INVALID_HANDLE, 0U, 0U, 0U, 0, "终端", SHELL_WINDOW_BACKGROUND,
};
static char g_command[SHELL_LINE_CAPACITY];
static size_t g_command_length;
static size_t g_command_cursor;
static char g_output[SHELL_OUTPUT_LINES][SHELL_LINE_CAPACITY];
static uint32_t g_output_count;
static uint32_t g_output_head;
static uint32_t g_output_scroll;
static uint32_t g_view_first, g_view_lines, g_input_y, g_input_view_start, g_prompt_length;
static size_t g_input_anchor = SIZE_MAX;
static size_t g_output_anchor = SIZE_MAX, g_output_selection_end;
static uint32_t g_mouse_selecting;
static uint32_t g_background_children;
static char g_history[SHELL_HISTORY_LINES][SHELL_LINE_CAPACITY];
static uint32_t g_history_count;
static int32_t g_history_cursor = -1;
static char g_cwd[SHELL_PATH_CAPACITY] = "/";
static bool g_shift;
static bool g_ctrl;
static bool g_foreground_wait;
static bool handle_event(const os_window_event_t *event);
static os_exec_fd_map_t g_exec_fd_map;
static uint32_t g_display_width;
static uint32_t g_display_height;
static uint32_t g_target_width;
static uint32_t g_target_height;
static os_control_node_t g_nodes[SHELL_OUTPUT_LINES*2U+SHELL_TREE_FIXED_NODES];
/* 输出、输入和标题均按最大有效字节预留，不含各自的结束符。 */
static char g_tree_text[SHELL_OUTPUT_LINES*(SHELL_LINE_CAPACITY-1U)+
                        SHELL_DISPLAY_CAPACITY-1U+sizeof(SHELL_CAPTION_TITLE)-1U];
static os_window_control_tree_t g_tree;
static uint32_t g_node_count,g_text_used,g_caption_height;
static uint8_t g_file_buffer[128];
static char g_display_line[SHELL_DISPLAY_CAPACITY];

static int64_t stat_path(const char *path, os_file_info_t *info);
static bool file_exists(const char *path);

static int64_t gshell_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t gshell_syscall_two(uint64_t number, uint64_t arg0,
                                  uint64_t arg1) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t gshell_syscall_three(uint64_t number, uint64_t arg0,
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

static int64_t gshell_syscall_four(uint64_t number, uint64_t arg0,
                                   uint64_t arg1, uint64_t arg2,
                                   uint64_t arg3) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = arg3;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10) : : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t gshell_syscall_five(uint64_t number, uint64_t arg0,
                                   uint64_t arg1, uint64_t arg2,
                                   uint64_t arg3, uint64_t arg4) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = arg3;
    register uint64_t r8 __asm__("r8") = arg4;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10), "+r"(r8) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static void gshell_exit(uint64_t status) {
    (void)gshell_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static void copy_text(char *destination, const char *source) {
    uint32_t index = 0U;
    while (index + 1U < SHELL_LINE_CAPACITY && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static uint32_t text_length(const char *text) {
    uint32_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0' && length + 1U < SHELL_LINE_CAPACITY) ++length;
    return length;
}

static void append_text(char *destination, uint32_t capacity, const char *source) {
    uint32_t length = 0U;
    uint32_t source_index = 0U;
    if (destination == 0 || source == 0 || capacity == 0U) return;
    while (length + 1U < capacity && destination[length] != '\0') ++length;
    while (length + 1U < capacity && source[source_index] != '\0') {
        destination[length++] = source[source_index++];
    }
    destination[length] = '\0';
}

static void append_character(char *destination, uint32_t capacity, char character) {
    uint32_t length = 0U;
    if (destination == 0 || capacity < 2U) return;
    while (length + 1U < capacity && destination[length] != '\0') ++length;
    if (length + 1U >= capacity) return;
    destination[length++] = character;
    destination[length] = '\0';
}

static char *shell_output_line(uint32_t index) {
    return g_output[(g_output_head + index) % SHELL_OUTPUT_LINES];
}

static void append_output(const char *text) {
    g_output_scroll = 0U;
    g_output_anchor = SIZE_MAX;
    copy_text(shell_output_line(g_output_count), text);
    if (g_output_count < SHELL_OUTPUT_LINES) ++g_output_count;
    else g_output_head = (g_output_head + 1U) % SHELL_OUTPUT_LINES;
}

static void clear_output(void) {
    g_output_count = 0U;
    g_output_head = 0U;
    g_output_scroll = 0U;
    g_output_anchor = SIZE_MAX;
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      uint32_t color) {
    if (x >= g_target_width || y >= g_target_height || !width || !height) return;
    if (width > g_target_width - x) width = g_target_width - x;
    if (height > g_target_height - y) height = g_target_height - y;
    if (g_node_count>=sizeof(g_nodes)/sizeof(g_nodes[0])) return;
    uint32_t index=g_node_count++;
    g_nodes[index]=(os_control_node_t){.identifier=index+1U,
        .parent=index?0:OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
        .flags=index?OS_CONTROL_POINTER_PASSTHROUGH:OS_CONTROL_CLIP_CHILDREN,
        .x=(int32_t)x,.y=(int32_t)y,.width=(int32_t)width,.height=(int32_t)height,
        .background=color,.foreground=SHELL_OUTPUT_TEXT,.font_size=ui_text_size()};
}

static void draw_text(uint32_t x, uint32_t y, const char *text, uint32_t color) {
    uint32_t length=0;
    while (length+1U<SHELL_DISPLAY_CAPACITY && text[length]) ++length;
    if (!length || x>=g_target_width || y>=g_target_height ||
        g_node_count>=sizeof(g_nodes)/sizeof(g_nodes[0]) ||
        length>sizeof(g_tree_text)-g_text_used) return;
    uint32_t index=g_node_count++;
    g_nodes[index]=(os_control_node_t){.identifier=index+1U,.parent=0,.kind=OS_CONTROL_TEXT,.value=OS_CONTROL_TEXT_APP_SELECTION,
        .flags=OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_INHERIT_FONT,
        .x=(int32_t)x,.y=(int32_t)y,.width=(int32_t)(g_target_width-x),
        .height=(int32_t)OS_TEXT_HEIGHT,.foreground=color,
        .text_offset=g_text_used,.text_length=length};
    __builtin_memcpy(g_tree_text+g_text_used,text,length);g_text_used+=length;
}

static void build_prompt(char *buffer) {
    if (buffer == 0) return;
    buffer[0] = '\0';
    append_text(buffer, SHELL_DISPLAY_CAPACITY, g_cwd);
    append_text(buffer, SHELL_DISPLAY_CAPACITY, "# ");
}

/* 输入显示同时包含路径和命令，与输出行不同。 */
static uint32_t shell_display_length(const char *text) {
    uint32_t length = 0U;
    if (text == 0) return 0U;
    while (length + 1U < SHELL_DISPLAY_CAPACITY && text[length] != '\0') ++length;
    return length;
}

static uint32_t shell_text_width(const char *text, size_t length) {
    if (text == 0 || length == 0U) return 0U;
    uint64_t width=0;
    for (size_t offset=0;offset<length;) {
        size_t used;
        uint32_t code=os_utf8_next(text+offset,length-offset,&used);
        width+=os_control_text_cell_width(code,ui_text_size());
        offset+=used?used:1U;
    }
    return width>UINT32_MAX?UINT32_MAX:(uint32_t)width;
}

static void draw_input_line(void) {
    const uint32_t input_x = 10U;
    uint32_t input_right;
    uint32_t available_width;
    uint32_t prompt_length;
    uint32_t line_length;
    uint32_t view_start = 0U;
    uint32_t cursor_position;
    uint32_t cursor_prefix_width;
    uint32_t prefix_width;
    uint32_t cursor_width;
    uint32_t input_y;
    uint32_t low;
    uint32_t high;
    uint32_t middle;

    build_prompt(g_display_line);
    prompt_length = shell_display_length(g_display_line);
    g_prompt_length = prompt_length;
    append_text(g_display_line, SHELL_DISPLAY_CAPACITY, g_command);
    line_length = shell_display_length(g_display_line);
    cursor_position = prompt_length + (uint32_t)g_command_cursor;
    if (cursor_position > line_length) cursor_position = line_length;
    input_right = g_target_width > input_x ? g_target_width - input_x : 0U;
    available_width = input_right > input_x ? input_right - input_x : 0U;
    if (available_width <= 2U) return;
    cursor_prefix_width = shell_text_width(g_display_line, cursor_position);
    if (cursor_prefix_width > available_width - 2U) {
        /* shell 输入前缀宽度从左到右单调递增。二分查找可避免每次按键事件都测量所有字符。 */
        low = 0U;
        high = cursor_position;
        while (low < high) {
            middle = low + (high - low) / 2U;
            prefix_width = shell_text_width(g_display_line, middle);
            if (cursor_prefix_width > prefix_width &&
                cursor_prefix_width - prefix_width > available_width - 2U) {
                low = middle + 1U;
            } else {
                high = middle;
            }
        }
        view_start = low;
    }
    g_input_view_start = view_start;
    cursor_width = shell_text_width(g_display_line + view_start,
                                    cursor_position - view_start);
    input_y = g_input_y;
    if (g_input_anchor != SIZE_MAX && g_input_anchor != g_command_cursor) {
        size_t first = g_input_anchor < g_command_cursor ? g_input_anchor : g_command_cursor;
        size_t last = g_input_anchor > g_command_cursor ? g_input_anchor : g_command_cursor;
        first += prompt_length; last += prompt_length;
        if (last > view_start) {
            if (first < view_start) first = view_start;
            uint32_t left = shell_text_width(g_display_line + view_start, first - view_start);
            uint32_t right = shell_text_width(g_display_line + view_start, last - view_start);
            if (left < available_width) {
                if (right > available_width) right = available_width;
                fill_rect(input_x + left, input_y, right - left, OS_TEXT_HEIGHT, OS_THEME_SELECTED);
            }
        }
    }
    draw_text(input_x,input_y,g_display_line+view_start,SHELL_INPUT_TEXT);
    /* 文本右侧保留与输入命中、水平滚动一致的边距。 */
    if (g_node_count && g_nodes[g_node_count-1U].kind==OS_CONTROL_TEXT)
        g_nodes[g_node_count-1U].width=(int32_t)available_width;
    if (cursor_width < available_width) {
        fill_rect(input_x + cursor_width, input_y, 2U, OS_TEXT_HEIGHT,
                  SHELL_CURSOR);
    }
}

static uint32_t shell_scroll_maximum(uint32_t visible) {
    /* 视口连一行都无法显示时，不创建输入行。 */
    uint32_t rows = g_output_count +
                    (visible != 0U && !g_foreground_wait ? 1U : 0U);
    return rows > visible ? rows - visible : 0U;
}

static bool shell_scroll_output(int32_t direction) {
    uint32_t previous = g_output_scroll;
    uint32_t maximum = shell_scroll_maximum(g_view_lines);
    if (g_output_scroll > maximum) g_output_scroll = maximum;
    if (direction > 0)
        g_output_scroll = maximum - g_output_scroll > 3U ?
                          g_output_scroll + 3U : maximum;
    else if (direction < 0)
        g_output_scroll = g_output_scroll > 3U ? g_output_scroll - 3U : 0U;
    return previous != g_output_scroll;
}

static void draw_terminal(void) {
    uint32_t caption_height = 0U; /* coordinates are client-area only */
    uint32_t line_height = OS_TEXT_HEIGHT;
    uint32_t visible = line_height != 0U && g_target_height > caption_height ?
                       (g_target_height - caption_height) / line_height : 0U;
    uint32_t maximum = shell_scroll_maximum(visible);
    if (g_output_scroll > maximum) g_output_scroll = maximum;
    uint32_t first = maximum - g_output_scroll;
    g_view_first = first;
    g_view_lines = visible;
    if (visible == 0U) g_mouse_selecting = 0U;
    uint32_t y = caption_height;

    fill_rect(0U, 0U, g_target_width, g_target_height, g_shell.color);
    for (uint32_t index = first, row = 0U;
         index < g_output_count && row < visible;
         ++index, ++row, y += line_height) {
        const char *line = shell_output_line(index);
        if (g_output_anchor != SIZE_MAX) {
            size_t first_selected = g_output_anchor < g_output_selection_end ? g_output_anchor : g_output_selection_end;
            size_t last_selected = g_output_anchor > g_output_selection_end ? g_output_anchor : g_output_selection_end;
            size_t base = (size_t)index * SHELL_LINE_CAPACITY;
            size_t length = text_length(line);
            if (last_selected > base && first_selected < base + length) {
                size_t left = first_selected > base ? first_selected - base : 0U;
                size_t right = last_selected < base + length ? last_selected - base : length;
                uint32_t left_x = shell_text_width(line, left);
                uint32_t right_x = shell_text_width(line, right);
                fill_rect(10U + left_x, y, right_x - left_x, OS_TEXT_HEIGHT, OS_THEME_SELECTED);
            }
        }
        draw_text(10U, y, line, SHELL_OUTPUT_TEXT);
    }
    g_input_y = y;
    if (!g_foreground_wait && g_output_count >= first &&
        g_output_count - first < visible) draw_input_line();
}

static void update_window(void) {
    os_window_update_t request = {0};
    uint32_t caption=user_caption_height();
    request.flags = OS_WINDOW_CAPTION_HEIGHT(caption);
    if (g_shell.identifier == 0U) return;
    if (caption==g_caption_height) return;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.identifier = g_shell.identifier;
    request.width=g_shell.width;request.height=g_shell.height;
    if (gshell_syscall_one(OS_SYS_WINDOW_UPDATE,(uintptr_t)&request)==0)
        g_caption_height=caption;
}

static void render_window(void) {
    g_target_width = g_shell.width;
    g_target_height = g_shell.height;
    if (!g_target_width || !g_target_height) return;
    g_node_count=g_text_used=0;
    draw_terminal();
    if (!user_caption_tree(g_nodes,&g_node_count,sizeof(g_nodes)/sizeof(*g_nodes),g_tree_text,&g_text_used,
            sizeof(g_tree_text),(user_caption_layout_t){.title=SHELL_CAPTION_TITLE,.width=g_shell.width,
                .height=user_caption_height(),.font=ui_text_size()})) {
        g_tree.count=0;return;
    }
    os_window_control_tree_t tree={.hdr={sizeof(tree),OS_SYSCALL_ABI_VERSION},
        .identifier=g_shell.identifier,.count=g_node_count,.nodes=(uintptr_t)g_nodes,
        .text=(uintptr_t)g_tree_text,.text_size=g_text_used};
    if (gshell_syscall_one(OS_SYS_WINDOW_CONTROL_TREE,(uintptr_t)&tree)!=0) {
        g_tree.count=0;return;
    }
    g_tree=tree;
    update_window();
}

static bool create_window(int32_t x, int32_t y, uint32_t width,
                          uint32_t height) {
    os_window_create_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.x = x;
    request.y = y;
    request.width = width;
    request.height = height;
    request.flags = OS_WINDOW_VISIBLE |
                    OS_WINDOW_RESIZABLE |
                    OS_WINDOW_STANDARD_CAPTION_CONTROLS | OS_WINDOW_CAPTION_HEIGHT(user_caption_height());
    request.background = g_shell.color;
    for (uint32_t i = 0U; i < 31U && g_shell.title[i] != '\0'; ++i) {
        request.title[i] = g_shell.title[i];
    }
    request.address = SHELL_MAP_BASE;
    if (gshell_syscall_one(OS_SYS_WINDOW_CREATE, (uint64_t)&request) != 0 ||
        request.window == OS_INVALID_HANDLE || request.address == 0U) return false;
    g_shell.handle = request.window;
    g_shell.identifier = request.identifier;
    g_shell.width = width;
    g_shell.height = height;
    g_shell.pixels = (uint32_t *)(uintptr_t)request.address;
    return true;
}

static bool setup_windows(void) {
    os_display_info_t info = {0};
    uint32_t shell_width;
    uint32_t shell_height;
    info.hdr.size = sizeof(info);
    info.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (gshell_syscall_one(OS_SYS_DISPLAY_GET_INFO, (uint64_t)&info) < 0 ||
        info.width < 320U || info.height < 240U) return false;
    g_display_width = info.width;
    g_display_height = info.height;
    /* 使用紧凑的居中初始窗口；标题栏和边框由合成器统一绘制。 */
    shell_width = info.width * 3U / 4U;
    shell_height = info.height * 3U / 4U;
    if (shell_width < 640U && info.width > 640U) shell_width = 640U;
    if (shell_height < 420U && info.height > 420U) shell_height = 420U;
    if (shell_width > 800U) shell_width = 800U;
    if (shell_height > 500U) shell_height = 500U;
    if (!create_window((int32_t)((info.width - shell_width) / 2U),
                       (int32_t)((info.height - shell_height) / 2U),
                       shell_width, shell_height)) return false;
    return true;
}

static char key_to_ascii(uint32_t code, bool shift) {
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    static const char shifted_letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (code >= (uint32_t)'A' && code <= (uint32_t)'Z') {
        return shift ? (char)code : (char)(code - (uint32_t)'A' + (uint32_t)'a');
    }
    if (code >= (uint32_t)'a' && code <= (uint32_t)'z') {
        return shift ? (char)(code - (uint32_t)'a' + (uint32_t)'A') : (char)code;
    }
    if (code >= 0x04U && code <= 0x1DU) {
        return shift ? shifted_letters[code - 0x04U] : letters[code - 0x04U];
    }
    if (code >= 0x1EU && code <= 0x26U) {
        static const char numbers[] = "123456789";
        static const char shifted[] = "!@#$%^&*(";
        return shift ? shifted[code - 0x1EU] : numbers[code - 0x1EU];
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

static char lower_ascii(char character) {
    return character >= 'A' && character <= 'Z' ?
           (char)(character - 'A' + 'a') : character;
}

static bool text_equals_ignore_case(const char *left, const char *right) {
    uint32_t index = 0U;
    while (left[index] != '\0' || right[index] != '\0') {
        if (lower_ascii(left[index]) != lower_ascii(right[index])) return false;
        ++index;
    }
    return true;
}

static bool is_shell_space(char character) {
    return character == ' ' || character == '\t';
}

/* 就地分词。引号转义现在可用于内建命令，也使命令行与未来支持 argv 的 process_exec
 * ABI 保持兼容。 */
static bool shell_tokenize(char *line, char **arguments, uint32_t *count,
                           bool *background) {
    uint32_t read_index = 0U;
    uint32_t write_index = 0U;
    uint32_t argument_start = 0U;
    uint32_t argument_count = 0U;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;
    bool in_argument = false;

    if (line == 0 || arguments == 0 || count == 0 || background == 0) return false;
    *count = 0U;
    *background = false;
    while (line[read_index] != '\0') {
        char character = line[read_index++];
        /* 引号和转义也可以开始新参数，必须在消费它们前记录写入位置。 */
        if (!in_argument && !is_shell_space(character) && character != '&') {
            argument_start = write_index;
            in_argument = true;
        }
        if (escaped) {
            line[write_index++] = character;
            escaped = false;
            in_argument = true;
            continue;
        }
        if (character == '\\' && !in_single_quote) {
            escaped = true;
            in_argument = true;
            continue;
        }
        if (character == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            in_argument = true;
            continue;
        }
        if (character == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            in_argument = true;
            continue;
        }
        if (in_single_quote || in_double_quote) {
            line[write_index++] = character;
            continue;
        }
        if (character == '&') {
            if (in_argument) {
                if (argument_count >= SHELL_MAX_ARGUMENTS) return false;
                line[write_index++] = '\0';
                arguments[argument_count++] = line + argument_start;
                in_argument = false;
            }
            *background = true;
            while (is_shell_space(line[read_index])) ++read_index;
            if (line[read_index] != '\0') return false;
            continue;
        }
        if (is_shell_space(character)) {
            if (in_argument) {
                if (argument_count >= SHELL_MAX_ARGUMENTS) return false;
                line[write_index++] = '\0';
                arguments[argument_count++] = line + argument_start;
                in_argument = false;
            }
            continue;
        }
        line[write_index++] = character;
    }
    if (escaped || in_single_quote || in_double_quote) return false;
    if (in_argument) {
        if (argument_count >= SHELL_MAX_ARGUMENTS) return false;
        arguments[argument_count++] = line + argument_start;
    }
    /* 最后一个参数只写一次终止符，输入缓冲区无需额外预留一个字节。 */
    line[write_index] = '\0';
    *count = argument_count;
    return true;
}

static bool make_program_path(const char *argument, char *path) {
    static const char *const prefixes[] = { "/sbin/", "/bin/", "/" };
    char candidate[SHELL_LINE_CAPACITY];
    bool has_slash = false;
    if (argument == 0 || argument[0] == '\0' || path == 0) return false;
    uint32_t length = 0U;
    while (length + 1U < SHELL_LINE_CAPACITY && argument[length] != '\0') {
        if (argument[length] == '/') has_slash = true;
        ++length;
    }
    if (argument[length] != '\0') return false;
    if (has_slash || argument[0] == '.' || file_exists(argument)) {
        copy_text(path, argument);
        return true;
    }
    /* 图形、命令行和诊断应用统一按同名目录查找；Chrome 命令经过启动器。 */
    candidate[0] = '\0';
    append_text(candidate, SHELL_LINE_CAPACITY, "/usr/apps/");
    for (uint32_t index = 0U; argument[index] != '\0'; ++index)
        append_character(candidate, SHELL_LINE_CAPACITY, lower_ascii(argument[index]));
    append_character(candidate, SHELL_LINE_CAPACITY, '/');
    for (uint32_t index = 0U; argument[index] != '\0'; ++index)
        append_character(candidate, SHELL_LINE_CAPACITY, lower_ascii(argument[index]));
    if (text_equals_ignore_case(argument, "chrome"))
        append_text(candidate, SHELL_LINE_CAPACITY, "-start");
    if (file_exists(candidate)) {
        copy_text(path, candidate);
        return true;
    }
    for (uint32_t prefix_index = 0U;
         prefix_index < sizeof(prefixes) / sizeof(prefixes[0]); ++prefix_index) {
        candidate[0] = '\0';
        append_text(candidate, SHELL_LINE_CAPACITY, prefixes[prefix_index]);
        for (uint32_t index = 0U; argument[index] != '\0'; ++index) {
            append_character(candidate, SHELL_LINE_CAPACITY,
                             lower_ascii(argument[index]));
        }
        if (file_exists(candidate)) {
            copy_text(path, candidate);
            return true;
        }
    }
    return false;
}

static bool append_text_chunk(const uint8_t *buffer, uint64_t count,
                              char *line, uint32_t *line_length) {
    bool published = false;
    if (buffer == 0 || line == 0 || line_length == 0) return false;
    for (uint64_t index = 0U; index < count; ++index) {
        uint8_t value = buffer[index];
        if (value == '\r') continue;
        if (value == '\n' || *line_length + 1U >= SHELL_LINE_CAPACITY) {
            line[*line_length] = '\0';
            append_output(line);
            published = true;
            *line_length = 0U;
            if (value == '\n') continue;
        }
        if (value < 0x20U || value > 0x7EU) value = '.';
        line[(*line_length)++] = (char)value;
    }
    return published;
}

static bool finish_text_line(char *line, uint32_t *line_length) {
    if (line == 0 || line_length == 0 || *line_length == 0U) return false;
    line[*line_length] = '\0';
    append_output(line);
    *line_length = 0U;
    return true;
}

static void command_cat(const char *argument) {
    char line[SHELL_LINE_CAPACITY] = {0};
    os_handle_t handle = OS_INVALID_HANDLE;
    uint32_t line_length = 0U;
    uint64_t total = 0U;
    if (argument == 0 || argument[0] == '\0') {
        append_output("USAGE: CAT FILE");
        return;
    }
    if (gshell_syscall_four(OS_SYS_FILE_OPEN, (uint64_t)argument,
                            OS_FILE_OPEN_READ, 0U,
                            (uint64_t)&handle) < 0 ||
        handle == OS_INVALID_HANDLE) {
        append_output("CAT: FILE NOT FOUND");
        return;
    }
    while (total < 2048U) {
        uint64_t bytes = 0U;
        uint64_t capacity = sizeof(g_file_buffer);
        int64_t status = gshell_syscall_four(OS_SYS_FILE_READ, handle,
                                             (uint64_t)g_file_buffer, capacity,
                                             (uint64_t)&bytes);
        if (status < 0) {
            append_output("CAT: READ FAILED");
            break;
        }
        if (bytes == 0U) break;
        if (bytes > capacity) bytes = capacity;
        append_text_chunk(g_file_buffer, bytes, line, &line_length);
        total += bytes;
    }
    finish_text_line(line, &line_length);
    (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
    if (total == 0U) append_output("CAT: EMPTY FILE");
}

static int64_t enumerate_directory(const char *path, uint32_t index,
                                   os_file_info_t *info) {
    os_file_enumerate_t request = {0};
    if (path == 0 || info == 0) return -22;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)path;
    request.index = index;
    int64_t status = gshell_syscall_one(OS_SYS_FILE_ENUMERATE,
                                        (uint64_t)&request);
    if (status == 0) *info = request.info;
    return status;
}

static int64_t stat_path(const char *path, os_file_info_t *info) {
    os_file_stat_t request = {0};
    if (path == 0 || info == 0) return -22;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)path;
    int64_t status = gshell_syscall_one(OS_SYS_FILE_STAT,
                                        (uint64_t)(uintptr_t)&request);
    if (status == 0) *info = request.info;
    return status;
}

static bool directory_exists(const char *path) {
    os_file_info_t info = {0};
    return stat_path(path, &info) == 0 && info.type == OS_FILE_TYPE_DIRECTORY;
}

static bool file_exists(const char *path) {
    os_file_info_t info = {0};
    return stat_path(path, &info) == 0 && info.type == OS_FILE_TYPE_REGULAR;
}

static void command_ls(const char *argument) {
    const char *directory = argument != 0 && argument[0] != '\0' ? argument : g_cwd;
    if (!directory_exists(directory)) {
        if (file_exists(directory)) {
            append_output(directory);
        } else {
            append_output("LS: DIRECTORY NOT FOUND");
        }
        return;
    }
    append_output(directory);
    for (uint32_t index = 0U; index < 64U; ++index) {
        os_file_info_t info = {0};
        int64_t status = enumerate_directory(directory, index, &info);
        if (status == -2) break;
        if (status < 0) {
            append_output("LS: ENUMERATION FAILED");
            return;
        }
        g_display_line[0] = '\0';
        append_text(g_display_line, SHELL_LINE_CAPACITY, info.name);
        if (info.type == OS_FILE_TYPE_DIRECTORY) {
            append_character(g_display_line, SHELL_LINE_CAPACITY, '/');
        }
        append_output(g_display_line);
    }
}

static void set_exec_output_entry(os_exec_fd_entry_t *entry,
                                  uint32_t descriptor, os_handle_t handle) {
    if (entry == 0) return;
    *entry = (os_exec_fd_entry_t){0};
    entry->descriptor = descriptor;
    entry->handle = handle;
    entry->open_flags = OS_FILE_OPEN_WRITE;
    entry->resource_flags = OS_EXEC_FD_PIPE;
}

static void build_exec_output_map(os_handle_t output_handle,
                                  os_handle_t error_handle) {
    g_exec_fd_map = (os_exec_fd_map_t){0};
    g_exec_fd_map.hdr.size = sizeof(g_exec_fd_map);
    g_exec_fd_map.hdr.version = OS_SYSCALL_ABI_VERSION;
    g_exec_fd_map.count = 2U;
    set_exec_output_entry(&g_exec_fd_map.entries[0], 1U, output_handle);
    set_exec_output_entry(&g_exec_fd_map.entries[1], 2U, error_handle);
}

static __attribute__((noreturn)) void launch_program_entry(
    uint64_t context_address) {
    shell_launch_context_t *context =
        (shell_launch_context_t *)(uintptr_t)context_address;
    os_handle_t error_handle = OS_INVALID_HANDLE;
    const os_exec_fd_map_t *descriptor_map = 0;
    bool exec_ready = context != 0;
    int64_t status = -22;
    if (context != 0) {
        if (context->redirect_output) {
            (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE,
                                     context->output_read);
            if (gshell_syscall_three(OS_SYS_HANDLE_DUP,
                                     context->output_write, 0U,
                                     (uint64_t)(uintptr_t)&error_handle) < 0) {
                exec_ready = false;
            } else {
                build_exec_output_map(context->output_write, error_handle);
                descriptor_map = &g_exec_fd_map;
            }
        }
        if (exec_ready) {
            status = descriptor_map != 0 ?
                gshell_syscall_four(OS_SYS_PROCESS_EXEC,
                                    (uint64_t)(uintptr_t)context->path,
                                    (uint64_t)(uintptr_t)context->arguments, 0U,
                                    (uint64_t)(uintptr_t)descriptor_map) :
                gshell_syscall_three(OS_SYS_PROCESS_EXEC,
                                     (uint64_t)(uintptr_t)context->path,
                                     (uint64_t)(uintptr_t)context->arguments, 0U);
        }
    }
    (void)gshell_syscall_one(OS_SYS_THREAD_EXIT, status < 0 ? 1U : 0U);
    for (;;) __asm__ volatile ("pause");
}

static void append_decimal(char *destination, uint32_t capacity, int64_t value) {
    char digits[24];
    uint64_t magnitude;
    uint32_t digit_count = 0U;
    if (destination == 0 || capacity == 0U) return;
    destination[0] = '\0';
    if (value < 0) {
        append_character(destination, capacity, '-');
        magnitude = (uint64_t)(-(value + 1)) + 1U;
    } else {
        magnitude = (uint64_t)value;
    }
    do {
        digits[digit_count++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude != 0U && digit_count < sizeof(digits));
    while (digit_count != 0U) append_character(destination, capacity, digits[--digit_count]);
}

static void append_status_line(const char *prefix, int64_t value) {
    char number[32];
    g_display_line[0] = '\0';
    append_text(g_display_line, SHELL_LINE_CAPACITY, prefix);
    append_decimal(number, sizeof(number), value);
    append_text(g_display_line, SHELL_LINE_CAPACITY, number);
    append_output(g_display_line);
}

static void foreground_window_event(uint64_t timeout_ns) {
    os_window_event_read_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.identifier = g_shell.identifier;
    request.timeout_ns = timeout_ns;
    if (gshell_syscall_one(OS_SYS_WINDOW_EVENT_READ, (uint64_t)&request) == 0)
        (void)handle_event(&request.event);
}

static bool read_program_output(os_handle_t handle, bool mirror_serial) {
    char line[SHELL_LINE_CAPACITY] = {0};
    uint32_t line_length = 0U;
    bool success = true;
    bool dirty = false;
    uint32_t chunks = 0U;
    for (;;) {
        uint64_t bytes = 0U;
        int64_t status = gshell_syscall_five(
            OS_SYS_PIPE_READ, handle, (uint64_t)(uintptr_t)g_file_buffer,
            sizeof(g_file_buffer), SHELL_FOREGROUND_WAIT_NS,
            (uint64_t)(uintptr_t)&bytes);
        if (status == -110 || status == -11) {
            if (dirty) {
                render_window();
                dirty = false;
            }
            chunks = 0U;
            foreground_window_event(0U);
            continue;
        }
        if (status < 0) {
            append_output("PROGRAM OUTPUT READ FAILED");
            dirty = true;
            success = false;
            break;
        }
        if (bytes == 0U) break;
        if (bytes > sizeof(g_file_buffer)) bytes = sizeof(g_file_buffer);
        if (mirror_serial) {
            (void)gshell_syscall_two(OS_SYS_DEBUG_WRITE,
                                    (uint64_t)(uintptr_t)g_file_buffer, bytes);
        }
        if (append_text_chunk(g_file_buffer, bytes, line, &line_length)) dirty = true;
        if (++chunks == SHELL_OUTPUT_EVENT_BUDGET) {
            if (dirty) {
                render_window();
                dirty = false;
            }
            chunks = 0U;
            foreground_window_event(0U);
        }
    }
    if (finish_text_line(line, &line_length)) dirty = true;
    if (dirty) {
        render_window();
    }
    return success;
}

static int64_t process_wait_status(int32_t status) {
    uint32_t value = (uint32_t)status;
    if ((value & 0x7FU) != 0U) return 128 + (int64_t)(value & 0x7FU);
    return (int64_t)((value >> 8U) & 0xFFU);
}

static bool launch_program_foreground(shell_launch_context_t *context) {
    os_pipe_create_t pipe = {0};
    int64_t child_pid;
    int32_t wait_status = 0;
    bool output_success;
    pipe.hdr.size = sizeof(pipe);
    pipe.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (gshell_syscall_one(OS_SYS_PIPE_CREATE, (uint64_t)(uintptr_t)&pipe) < 0 ||
        pipe.read_handle == OS_INVALID_HANDLE ||
        pipe.write_handle == OS_INVALID_HANDLE) return false;
    context->output_read = pipe.read_handle;
    context->output_write = pipe.write_handle;
    context->redirect_output = true;
    child_pid = gshell_syscall_one(OS_SYS_PROCESS_FORK, 0U);
    if (child_pid < 0) {
        (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE, pipe.read_handle);
        (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE, pipe.write_handle);
        context->redirect_output = false;
        return false;
    }
    if (child_pid == 0) launch_program_entry((uint64_t)(uintptr_t)context);
    (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE, pipe.write_handle);
    context->output_read = OS_INVALID_HANDLE;
    context->output_write = OS_INVALID_HANDLE;
    context->redirect_output = false;
    g_foreground_wait = true;
    render_window();
    output_success = read_program_output(
        pipe.read_handle,
        text_equals_ignore_case(context->path, "/usr/apps/chrome/chrome-start") ||
        text_equals_ignore_case(context->path, "/usr/apps/chrome/chrome"));
    (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE, pipe.read_handle);
    for (;;) {
        int64_t result = gshell_syscall_three(
            OS_SYS_PROCESS_WAIT, (uint64_t)child_pid, OS_PROCESS_WAIT_NOHANG,
            (uint64_t)(uintptr_t)&wait_status);
        if (result != 0) {
            g_foreground_wait = false;
            if (result < 0) {
                append_output("PROGRAM WAIT FAILED");
                return false;
            }
            break;
        }
        foreground_window_event(SHELL_FOREGROUND_WAIT_NS);
    }
    append_status_line("PROGRAM EXIT STATUS ", process_wait_status(wait_status));
    return output_success;
}

static bool launch_program_background(shell_launch_context_t *context) {
    if (g_background_children == UINT32_MAX) return false;
    int64_t child_pid = gshell_syscall_one(OS_SYS_PROCESS_FORK, 0U);
    if (child_pid < 0) return false;
    if (child_pid == 0) launch_program_entry((uint64_t)(uintptr_t)context);
    ++g_background_children;
    return true;
}

static void reap_background_children(void) {
    /* 前台等待操作拥有其 PID：GUI 回调绝不能回收该进程。 */
    if (g_foreground_wait) return;
    for (uint32_t n = 0; n < SHELL_BACKGROUND_REAP_BUDGET &&
                         g_background_children != 0U; ++n) {
        int32_t status = 0;
        int64_t result = gshell_syscall_three(
            OS_SYS_PROCESS_WAIT, (uint64_t)-1, OS_PROCESS_WAIT_NOHANG,
            (uint64_t)(uintptr_t)&status);
        if (result > 0) --g_background_children;
        else {
            if (result == -10) g_background_children = 0U;
            break;
        }
    }
}

static bool launch_program(const char *path, char **program_arguments,
                           uint32_t argument_count, bool background) {
    shell_launch_context_t launch_context;
    shell_launch_context_t *context = &launch_context;
    if (path == 0 || path[0] == '\0' || program_arguments == 0 ||
        argument_count == 0U || argument_count > SHELL_MAX_ARGUMENTS) {
        return false;
    }
    /* 两条启动路径都会通过 fork 快照此栈。无需永久上下文数组、人为启动上限或共享子进程栈。 */
    copy_text(context->path, path);
    for (uint32_t index = 0U; index < argument_count; ++index) {
        copy_text(context->argument_storage[index], program_arguments[index]);
        context->arguments[index] = context->argument_storage[index];
    }
    context->arguments[argument_count] = 0;
    context->output_read = OS_INVALID_HANDLE;
    context->output_write = OS_INVALID_HANDLE;
    context->redirect_output = false;
    return background ? launch_program_background(context) :
                        launch_program_foreground(context);
}

static void remember_history(void) {
    if (g_command_length == 0U) return;
    if (g_history_count != 0U &&
        text_equals_ignore_case(g_history[g_history_count - 1U], g_command)) return;
    if (g_history_count < SHELL_HISTORY_LINES) {
        copy_text(g_history[g_history_count++], g_command);
    } else {
        for (uint32_t index = 1U; index < SHELL_HISTORY_LINES; ++index) {
            copy_text(g_history[index - 1U], g_history[index]);
        }
        copy_text(g_history[SHELL_HISTORY_LINES - 1U], g_command);
    }
}

static void append_command_echo(void) {
    char row[SHELL_LINE_CAPACITY];
    uint32_t offset = 0U;
    uint32_t available = g_target_width > 20U ? g_target_width - 20U : 0U;
    build_prompt(g_display_line);
    append_text(g_display_line, SHELL_DISPLAY_CAPACITY, g_command);
    uint32_t length = shell_display_length(g_display_line);
    while (offset < length) {
        uint32_t count = length - offset;
        if (count >= SHELL_LINE_CAPACITY) count = SHELL_LINE_CAPACITY - 1U;
        if (shell_text_width(g_display_line + offset, count) > available) {
            uint32_t low = 0U;
            uint32_t high = count;
            while (low < high) {
                uint32_t middle = low + (high - low + 1U) / 2U;
                if (shell_text_width(g_display_line + offset, middle) <= available)
                    low = middle;
                else high = middle - 1U;
            }
            count = low;
        }
        /* 即使视口宽度只容纳一个字形，也要保持 UTF-8 码点完整。 */
        while (count != 0U && offset + count < length &&
               ((uint8_t)g_display_line[offset + count] & 0xC0U) == 0x80U) --count;
        if (count == 0U) {
            count = 1U;
            while (offset + count < length && count < 4U &&
                   ((uint8_t)g_display_line[offset + count] & 0xC0U) == 0x80U) ++count;
        }
        __builtin_memcpy(row, g_display_line + offset, count);
        row[count] = '\0';
        append_output(row);
        offset += count;
    }
}

static void append_history_line(uint32_t index) {
    char number[32];
    append_decimal(number, sizeof(number), (int64_t)(index + 1U));
    g_display_line[0] = '\0';
    append_text(g_display_line, SHELL_LINE_CAPACITY, number);
    append_text(g_display_line, SHELL_LINE_CAPACITY, "  ");
    append_text(g_display_line, SHELL_LINE_CAPACITY, g_history[index]);
    append_output(g_display_line);
}

static void command_echo(char **arguments, uint32_t count) {
    g_display_line[0] = '\0';
    for (uint32_t index = 1U; index < count; ++index) {
        if (index != 1U) append_character(g_display_line, SHELL_LINE_CAPACITY, ' ');
        append_text(g_display_line, SHELL_LINE_CAPACITY, arguments[index]);
    }
    append_output(g_display_line);
}

static void command_cd(char **arguments, uint32_t count) {
    const char *target = count == 1U ? "/" : arguments[1];
    if (count > 2U) {
        append_output("USAGE: CD [DIRECTORY]");
        return;
    }
    if (gshell_syscall_one(OS_SYS_FILE_CHDIR, (uint64_t)(uintptr_t)target) != 0) {
        append_output("CD: DIRECTORY NOT FOUND");
        return;
    }
    if (gshell_syscall_two(OS_SYS_FILE_GETCWD, (uint64_t)(uintptr_t)g_cwd,
                          sizeof(g_cwd)) <= 0)
        append_output("CD: CANNOT READ CURRENT DIRECTORY");
}

static int64_t file_path_operation(uint64_t syscall_number, const char *path,
                                   uint32_t mode) {
    os_file_path_op_t request = {0};
    if (path == 0) return -22;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)path;
    request.mode = mode;
    return gshell_syscall_one(syscall_number, (uint64_t)(uintptr_t)&request);
}

static void command_stat(char **arguments, uint32_t count) {
    os_file_info_t info = {0};
    if (count != 2U) {
        append_output("USAGE: STAT FILE");
        return;
    }
    if (stat_path(arguments[1], &info) < 0) {
        append_output("STAT: PATH NOT FOUND");
        return;
    }
    g_display_line[0] = '\0';
    append_text(g_display_line, SHELL_LINE_CAPACITY,
                info.type == OS_FILE_TYPE_DIRECTORY ? "TYPE DIRECTORY" : "TYPE FILE");
    append_output(g_display_line);
    g_display_line[0] = '\0';
    append_text(g_display_line, SHELL_LINE_CAPACITY, "SIZE ");
    append_decimal(g_display_line + 5U, SHELL_LINE_CAPACITY - 5U,
                   (int64_t)info.size);
    append_output(g_display_line);
}

static void command_mkdir(char **arguments, uint32_t count) {
    if (count != 2U) {
        append_output("USAGE: MKDIR DIRECTORY");
        return;
    }
    if (file_path_operation(OS_SYS_FILE_MKDIR, arguments[1], 0755U) < 0) {
        append_output("MKDIR: FAILED");
        return;
    }
    append_output("DIRECTORY CREATED");
}

static void command_touch(char **arguments, uint32_t count) {
    os_handle_t handle = OS_INVALID_HANDLE;
    if (count != 2U) {
        append_output("USAGE: TOUCH FILE");
        return;
    }
    if (gshell_syscall_four(OS_SYS_FILE_OPEN, (uint64_t)arguments[1],
                            OS_FILE_OPEN_READ | OS_FILE_OPEN_WRITE |
                            OS_FILE_OPEN_CREATE, 0666U, (uint64_t)&handle) < 0 ||
        handle == OS_INVALID_HANDLE) {
        append_output("TOUCH: FAILED");
        return;
    }
    (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
    append_output("FILE READY");
}

static void command_remove(char **arguments, uint32_t count) {
    if (count != 2U) {
        append_output("USAGE: RM PATH");
        return;
    }
    if (file_path_operation(OS_SYS_FILE_REMOVE, arguments[1], 0U) < 0) {
        append_output("RM: FAILED");
        return;
    }
    append_output("REMOVED");
}

static bool is_builtin_command(const char *name) {
    static const char *const names[] = {
        "help", "about", "status", "version", "clear", "echo", "pwd",
        "cd", "ls", "dir", "cat", "stat", "mkdir", "touch", "rm",
        "history", "which", "type", "open", "run", "fileman", "fm",
        "notepad", "exit", "true", "false", "reboot", "shutdown",
    };
    for (uint32_t index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (text_equals_ignore_case(name, names[index])) return true;
    }
    return false;
}

static void command_program(char **arguments, uint32_t count, bool background) {
    char path[SHELL_LINE_CAPACITY];
    if (count < 2U) {
        append_output("USAGE: OPEN PROGRAM [&]");
        return;
    }
    if (!make_program_path(arguments[1], path)) {
        append_output("PROGRAM NOT FOUND");
        return;
    }
    if (!launch_program(path, arguments + 1U, count - 1U, background)) {
        append_output("PROGRAM LAUNCH FAILED");
        return;
    }
    if (background) append_output("PROGRAM STARTED IN BACKGROUND");
}

static void command_external(char **arguments, uint32_t count, bool background) {
    char path[SHELL_LINE_CAPACITY];
    if (count == 0U) return;
    if (!make_program_path(arguments[0], path)) {
        append_output("COMMAND NOT FOUND");
        return;
    }
    if (!launch_program(path, arguments, count, background)) {
        append_output("PROGRAM LAUNCH FAILED");
        return;
    }
    if (background) append_output("PROGRAM STARTED IN BACKGROUND");
}

static void command_power(char **arguments, uint32_t count, bool background) {
    if (count != 1U || background) {
        append_output("USAGE: REBOOT | SHUTDOWN (NO ARGUMENTS OR &)");
        return;
    }
    int64_t status = gshell_syscall_one(OS_SYS_SYSTEM_POWER,
        text_equals_ignore_case(arguments[0], "shutdown") ? 1U : 0U);
    (void)status;
    append_output("POWER REQUEST FAILED; SYSTEM LEFT RUNNING");
}

static void execute_command(void) {
    char *arguments[SHELL_MAX_ARGUMENTS];
    uint32_t count = 0U;
    bool background = false;
    bool parsed;

    if (g_command_length == 0U) {
        append_output("TYPE HELP FOR COMMANDS");
        goto reset_command;
    }
    remember_history();
    append_command_echo();
    parsed = shell_tokenize(g_command, arguments, &count, &background);
    if (!parsed) {
        append_output("SYNTAX ERROR: QUOTE OR ESCAPE NOT CLOSED");
        goto reset_command;
    }
    if (count == 0U) {
        append_output("TYPE HELP FOR COMMANDS");
    } else if (text_equals_ignore_case(arguments[0], "help")) {
        append_output("HELP ABOUT STATUS VERSION");
        append_output("REBOOT SHUTDOWN");
        append_output("CLEAR ECHO PWD CD LS CAT STAT MKDIR TOUCH RM");
        append_output("WHICH TYPE OPEN RUN NOTEPAD FILEMAN FM [ARGS]");
        append_output("QUOTES AND BACKSLASH ESCAPES ARE SUPPORTED");
    } else if (text_equals_ignore_case(arguments[0], "reboot") ||
               text_equals_ignore_case(arguments[0], "shutdown")) {
        command_power(arguments, count, background);
    } else if (text_equals_ignore_case(arguments[0], "about")) {
        append_output("GRAPHICAL SHELL");
        append_output("ONE WINDOW CLIENT COMPOSED BY KERNEL");
    } else if (text_equals_ignore_case(arguments[0], "status")) {
        append_output("WINDOW SERVER: KERNEL");
        append_output("WINDOW COUNT: ONE SHELL");
        append_output(g_cwd);
    } else if (text_equals_ignore_case(arguments[0], "version")) {
        append_output("VERSION 1 ABI 1");
    } else if (text_equals_ignore_case(arguments[0], "clear")) {
        clear_output();
    } else if (text_equals_ignore_case(arguments[0], "echo")) {
        command_echo(arguments, count);
    } else if (text_equals_ignore_case(arguments[0], "pwd")) {
        append_output(g_cwd);
    } else if (text_equals_ignore_case(arguments[0], "cd")) {
        command_cd(arguments, count);
    } else if (text_equals_ignore_case(arguments[0], "ls") ||
               text_equals_ignore_case(arguments[0], "dir")) {
        if (count > 2U) append_output("USAGE: LS [DIRECTORY]");
        else command_ls(count == 2U ? arguments[1] : 0);
    } else if (text_equals_ignore_case(arguments[0], "cat")) {
        if (count != 2U) append_output("USAGE: CAT FILE");
        else command_cat(arguments[1]);
    } else if (text_equals_ignore_case(arguments[0], "stat")) {
        command_stat(arguments, count);
    } else if (text_equals_ignore_case(arguments[0], "mkdir")) {
        command_mkdir(arguments, count);
    } else if (text_equals_ignore_case(arguments[0], "touch")) {
        command_touch(arguments, count);
    } else if (text_equals_ignore_case(arguments[0], "rm")) {
        command_remove(arguments, count);
    } else if (text_equals_ignore_case(arguments[0], "history")) {
        for (uint32_t index = 0U; index < g_history_count; ++index) {
            append_history_line(index);
        }
    } else if (text_equals_ignore_case(arguments[0], "which") ||
               text_equals_ignore_case(arguments[0], "type")) {
        char path[SHELL_LINE_CAPACITY];
        if (count != 2U) {
            append_output("USAGE: WHICH COMMAND");
        } else if (is_builtin_command(arguments[1])) {
            g_display_line[0] = '\0';
            append_text(g_display_line, SHELL_LINE_CAPACITY, "BUILTIN ");
            append_text(g_display_line, SHELL_LINE_CAPACITY, arguments[1]);
            append_output(g_display_line);
        } else if (make_program_path(arguments[1], path)) {
            append_output(path);
        } else {
            append_output("COMMAND NOT FOUND");
        }
    } else if (text_equals_ignore_case(arguments[0], "open") ||
               text_equals_ignore_case(arguments[0], "run")) {
        bool open_command = text_equals_ignore_case(arguments[0], "open");
        command_program(arguments, count, open_command || background);
    } else if (text_equals_ignore_case(arguments[0], "true")) {
        /* 加入命令串联功能后，成功的无操作对脚本也有用。 */
    } else if (text_equals_ignore_case(arguments[0], "false")) {
        append_output("FALSE");
    } else if (text_equals_ignore_case(arguments[0], "exit")) {
        gshell_exit(0U);
    } else {
        command_external(arguments, count, background);
    }

reset_command:
    g_input_anchor = SIZE_MAX;
    g_mouse_selecting = 0U;
    g_command_length = 0U;
    g_command_cursor = 0U;
    g_history_cursor = -1;
    g_command[0] = '\0';
}

static void clear_command_line(void) {
    g_input_anchor = SIZE_MAX;
    g_command_length = 0U;
    g_command_cursor = 0U;
    g_command[0] = '\0';
}

static void load_history_entry(int32_t index) {
    g_input_anchor = SIZE_MAX;
    if (index < 0 || (uint32_t)index >= g_history_count) return;
    copy_text(g_command, g_history[(uint32_t)index]);
    g_command_length = text_length(g_command);
    g_command_cursor = g_command_length;
}

static void history_previous(void) {
    if (g_history_count == 0U) return;
    if (g_history_cursor < 0) g_history_cursor = (int32_t)g_history_count - 1;
    else if (g_history_cursor > 0) --g_history_cursor;
    load_history_entry(g_history_cursor);
}

static void history_next(void) {
    if (g_history_cursor < 0) return;
    if ((uint32_t)(g_history_cursor + 1) >= g_history_count) {
        g_history_cursor = -1;
        clear_command_line();
        return;
    }
    ++g_history_cursor;
    load_history_entry(g_history_cursor);
}

static bool text_has_prefix_ignore_case(const char *text, const char *prefix) {
    uint32_t index = 0U;
    while (prefix[index] != '\0') {
        if (text[index] == '\0' || lower_ascii(text[index]) != lower_ascii(prefix[index])) {
            return false;
        }
        ++index;
    }
    return true;
}

static bool shell_has_selection(void) {
    return g_input_anchor != SIZE_MAX && g_input_anchor != g_command_cursor;
}

static void shell_erase_selection(void) {
    if (!shell_has_selection()) { g_input_anchor = SIZE_MAX; return; }
    size_t first = g_input_anchor < g_command_cursor ? g_input_anchor : g_command_cursor;
    size_t last = g_input_anchor > g_command_cursor ? g_input_anchor : g_command_cursor;
    for (size_t index = last; index <= g_command_length; ++index)
        g_command[first + index - last] = g_command[index];
    g_command_length -= last - first;
    g_command_cursor = first;
    g_input_anchor = SIZE_MAX;
}

static void insert_command_text(const char *text) {
    uint32_t length = text_length(text);
    size_t selected = shell_has_selection() ?
        (g_input_anchor > g_command_cursor ? g_input_anchor - g_command_cursor : g_command_cursor - g_input_anchor) : 0U;
    if (text == 0 || length == 0U || g_command_length - selected + length >= SHELL_LINE_CAPACITY) return;
    shell_erase_selection();
    for (uint32_t index = (uint32_t)g_command_length + length;
         index >= g_command_cursor + length; --index) {
        g_command[index] = g_command[index - length];
    }
    for (uint32_t index = 0U; index < length; ++index) {
        g_command[g_command_cursor + index] = text[index];
    }
    g_command_length += length;
    g_command_cursor += length;
    g_command[g_command_length] = '\0';
}

/* 终端保留 Ctrl+C 中断语义，文本复制/粘贴使用 Ctrl+Shift+C/V。 */
static void shell_copy_selection(void) {
    if (shell_has_selection()) {
        size_t first=g_input_anchor<g_command_cursor?g_input_anchor:g_command_cursor;
        size_t last=g_input_anchor>g_command_cursor?g_input_anchor:g_command_cursor;
        (void)ui_clipboard_copy(gshell_syscall_one,g_command+first,last-first);
    } else if (g_output_anchor!=SIZE_MAX && g_output_anchor!=g_output_selection_end) {
        size_t first=g_output_anchor<g_output_selection_end?g_output_anchor:g_output_selection_end;
        size_t last=g_output_anchor>g_output_selection_end?g_output_anchor:g_output_selection_end;
        char *text=malloc(SHELL_OUTPUT_LINES*SHELL_LINE_CAPACITY);
        if (!text) return;
        size_t used=0;
        for (uint32_t row=0;row<g_output_count;++row) {
            const char *line=shell_output_line(row);size_t length=text_length(line);
            size_t base=(size_t)row*SHELL_LINE_CAPACITY;
            for (size_t i=0;i<length;++i)
                if (base+i>=first && base+i<last)text[used++]=line[i];
            if (row+1U<g_output_count && first<base+length && last>base+SHELL_LINE_CAPACITY)
                text[used++]='\n';
        }
        if (used)(void)ui_clipboard_copy(gshell_syscall_one,text,used);
        free(text);
    }
}
static void shell_paste_selection(void) {
    size_t selected=shell_has_selection()?(g_input_anchor>g_command_cursor?
        g_input_anchor-g_command_cursor:g_command_cursor-g_input_anchor):0U;
    size_t length=0;
    char *text=ui_clipboard_paste(gshell_syscall_one,SHELL_LINE_CAPACITY-1U-(g_command_length-selected),&length);
    if (!text)return;
    /* 粘贴不执行命令，多行和控制字符转换为空格。 */
    for (size_t i=0;i<length;++i)
        if ((unsigned char)text[i]<32U || (unsigned char)text[i]==127U)text[i]=' ';
    insert_command_text(text);free(text);
}

static void insert_command_character(char character) {
    char text[2] = {character, '\0'};
    insert_command_text(text);
}

static void backspace_command_character(void) {
    if (shell_has_selection()) { shell_erase_selection(); return; }
    g_input_anchor = SIZE_MAX;
    if (g_command_cursor == 0U) return;
    for (uint32_t index = (uint32_t)g_command_cursor; index <= g_command_length; ++index) {
        g_command[index - 1U] = g_command[index];
    }
    --g_command_cursor;
    --g_command_length;
}

static void delete_command_character(void) {
    if (shell_has_selection()) { shell_erase_selection(); return; }
    g_input_anchor = SIZE_MAX;
    if (g_command_cursor >= g_command_length) return;
    for (uint32_t index = (uint32_t)g_command_cursor; index < g_command_length; ++index) {
        g_command[index] = g_command[index + 1U];
    }
    --g_command_length;
}

static void complete_command(void) {
    static const char *const candidates[] = {
        "help", "about", "status", "version", "clear", "echo", "pwd", "cd",
        "ls", "dir", "cat", "history", "which", "type", "open", "run", "exit",
        "true", "false", "deviced", "logd", "crashd", "audiod", "gshell",
        "notepad", "fileman", "fm", "taskmgr", "settings", "imageview", "dyn-gfx",
        "libc-test", "nasm", "ndisasm", "wget", "linux-events-test",
        "fat-fsync-probe", "taskmgr-cpu-probe", "tcp-wake-probe",
    };
    uint32_t start = (uint32_t)g_command_cursor;
    uint32_t matches = 0U;
    const char *match = 0;
    if (g_command_cursor != g_command_length) return;
    while (start != 0U && !is_shell_space(g_command[start - 1U])) --start;
    if (start != 0U) return;
    g_command[g_command_cursor] = '\0';
    for (uint32_t index = 0U; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
        if (text_has_prefix_ignore_case(candidates[index], g_command + start)) {
            ++matches;
            match = candidates[index];
        }
    }
    if (matches == 1U && match != 0) {
        insert_command_text(match + g_command_cursor - start);
    } else if (matches > 1U) {
        append_output("MATCHES:");
        for (uint32_t index = 0U; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
            if (text_has_prefix_ignore_case(candidates[index], g_command + start)) {
                append_output(candidates[index]);
            }
        }
    }
}

static bool handle_key(const os_window_event_t *event) {
    const os_input_event_t *input = event != 0 ? &event->input : 0;
    char character;
    /* EVENT_READ 已按 g_shell.identifier 过滤；不要因某个输入后端未回填
     * event.identifier 而丢弃本来属于 shell 的按键。 */
    if (event == 0 || event->type != OS_WINDOW_EVENT_INPUT ||
        input == 0 || input->type != OS_INPUT_EVENT_KEY) return false;
    if (input->code == 0xE1U || input->code == 0xE5U) {
        g_shift = input->value != OS_INPUT_VALUE_RELEASE;
        return false;
    }
    if (input->code == 0xE0U || input->code == 0xE4U) {
        g_ctrl = input->value != OS_INPUT_VALUE_RELEASE;
        return false;
    }
    if (input->value == OS_INPUT_VALUE_RELEASE) return false;
    /* 前台子进程仍拥有命令执行权。保持窗口/字体控件响应，但不要递归启动另一条命令。 */
    if (g_foreground_wait &&
        !(g_ctrl && (input->code == 0x2EU || input->code == 0x2DU ||
                     input->code == 0x14U || input->code == (uint32_t)'Q')))
        return false;
    if (g_ctrl && g_shift && (input->code==0x06U || input->code=='C')) {
        shell_copy_selection();render_window();return true;
    }
    if (g_ctrl && g_shift && (input->code==0x19U || input->code=='V')) {
        shell_paste_selection();render_window();return true;
    }
    if (g_output_scroll) { g_output_scroll = 0U; }
    if (g_output_anchor != SIZE_MAX) { g_output_anchor = SIZE_MAX; }
    bool navigation = input->code == 0x4AU || input->code == 0x4DU ||
        input->code == 0x4FU || input->code == 0x50U || input->code == 0x51U || input->code == 0x52U;
    if (navigation) {
        if (g_shift && g_input_anchor == SIZE_MAX) g_input_anchor = g_command_cursor;
        else if (!g_shift) g_input_anchor = SIZE_MAX;
    }
    if (g_ctrl && input->code == 0x2EU) {
        (void)ui_text_adjust(1);
    } else if (g_ctrl && input->code == 0x2DU) {
        (void)ui_text_adjust(-1);
    } else if (g_ctrl && (input->code == 0x14U || input->code == (uint32_t)'Q')) {
        gshell_exit(0U);
    } else if (g_ctrl &&
               (input->code == 0x04U || input->code == (uint32_t)'A')) {
        g_command_cursor = 0U;
    } else if (g_ctrl && (input->code == 0x08U || input->code == (uint32_t)'E')) {
        g_command_cursor = g_command_length;

    } else if (g_ctrl && (input->code == 0x06U || input->code == (uint32_t)'C')) {
        if (g_command_length != 0U) append_output("^C");
        clear_command_line();
    } else if (g_ctrl && (input->code == 0x0FU || input->code == (uint32_t)'L')) {
        clear_output();
    } else if (g_ctrl && (input->code == 0x18U || input->code == (uint32_t)'U')) {
        clear_command_line();
    } else if (g_ctrl && (input->code == 0x07U || input->code == (uint32_t)'G')) {
        delete_command_character();
    } else if (input->code == 0x28U || input->code == 0x58U) {
        execute_command();
    } else if (input->code == 0x2AU) {
        backspace_command_character();
    } else if (input->code == 0x4FU) {
        if (g_command_cursor < g_command_length) ++g_command_cursor;
    } else if (input->code == 0x50U) {
        if (g_command_cursor != 0U) --g_command_cursor;
    } else if (input->code == 0x4AU) {
        g_command_cursor = 0U;
    } else if (input->code == 0x4DU) {
        g_command_cursor = g_command_length;
    } else if (input->code == 0x4CU) {
        delete_command_character();
    } else if (input->code == 0x52U) {
        history_previous();
    } else if (input->code == 0x51U) {
        history_next();
    } else if (input->code == 0x2BU) {
        complete_command();
    } else if (!g_ctrl) {
        character = key_to_ascii(input->code, g_shift);
        if (character != '\0') insert_command_character(character);
    }
    render_window();
    return true;
}

static size_t shell_mouse_text_offset(const char *text, size_t length, int32_t x) {
    uint32_t position = x > 10 ? (uint32_t)(x - 10) : 0U;
    size_t low = 0, high = length;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        uint32_t left = shell_text_width(text, middle);
        uint32_t right = shell_text_width(text, middle + 1U);
        if (position > left + (right - left) / 2U) low = middle + 1U;
        else high = middle;
    }
    return low;
}

static size_t shell_mouse_command_offset(int32_t x) {
    size_t length = shell_display_length(g_display_line);
    size_t offset = g_input_view_start + shell_mouse_text_offset(
        g_display_line + g_input_view_start, length - g_input_view_start, x);
    if (offset <= g_prompt_length) return 0U;
    offset -= g_prompt_length;
    return offset > g_command_length ? g_command_length : offset;
}

static size_t shell_mouse_output_offset(int32_t x, int32_t y) {
    if (!g_output_count || !g_view_lines) return 0U;
    uint32_t row = y > 0 ? (uint32_t)y / OS_TEXT_HEIGHT : 0U;
    if (g_view_lines && row >= g_view_lines) row = g_view_lines - 1U;
    uint32_t index = g_view_first + row;
    if (index >= g_output_count) return (size_t)(g_output_count - 1U) *
        SHELL_LINE_CAPACITY + text_length(shell_output_line(g_output_count - 1U));
    return (size_t)index * SHELL_LINE_CAPACITY +
        shell_mouse_text_offset(shell_output_line(index),
                                text_length(shell_output_line(index)), x);
}

static bool handle_shell_mouse(const os_window_event_t *event) {
    const os_input_event_t *input = &event->input;
    if (input->type == OS_INPUT_EVENT_RELATIVE && input->code == OS_INPUT_REL_WHEEL &&
        input->value && !g_ctrl) {
        if (!shell_scroll_output(input->value) || g_view_lines == 0U) return true;
    } else if (input->type == OS_INPUT_EVENT_BUTTON && input->code == OS_INPUT_BUTTON_LEFT) {
        if (input->value == OS_INPUT_VALUE_RELEASE) {
            if (!g_mouse_selecting) return false;
            if (g_mouse_selecting == 1U) g_command_cursor = shell_mouse_command_offset(event->pointer_x);
            else g_output_selection_end = shell_mouse_output_offset(event->pointer_x, event->pointer_y);
            g_mouse_selecting = 0U;
        } else if (input->value == OS_INPUT_VALUE_PRESS && g_view_lines != 0U &&
                   event->pointer_y >= 0) {
            if (!g_foreground_wait && g_output_count - g_view_first < g_view_lines &&
                event->pointer_y >= (int32_t)g_input_y &&
                event->pointer_y < (int32_t)(g_input_y + OS_TEXT_HEIGHT)) {
                g_output_anchor = SIZE_MAX;
                size_t next = shell_mouse_command_offset(event->pointer_x);
                if (!g_shift || g_input_anchor == SIZE_MAX) g_input_anchor = g_shift ? g_command_cursor : next;
                g_command_cursor = next;
                g_mouse_selecting = 1U;
            } else if ((int64_t)event->pointer_y < (int64_t)g_shell.height && g_output_count) {
                g_input_anchor = SIZE_MAX;
                g_output_anchor = shell_mouse_output_offset(event->pointer_x, event->pointer_y);
                g_output_selection_end = g_output_anchor;
                g_mouse_selecting = 2U;
            } else return false;
        } else return false;
    } else if (input->type == OS_INPUT_EVENT_RELATIVE && g_mouse_selecting &&
               (input->code == OS_INPUT_REL_X || input->code == OS_INPUT_REL_Y)) {
        if (g_mouse_selecting == 1U) {
            size_t next = shell_mouse_command_offset(event->pointer_x);
            if (next == g_command_cursor) return true;
            g_command_cursor = next;
        } else {
            size_t next = shell_mouse_output_offset(event->pointer_x, event->pointer_y);
            if (next == g_output_selection_end) return true;
            g_output_selection_end = next;
        }
    } else return false;
    render_window();
    return true;
}

/* 根节点语义不随选区刷新变化；拖选中的移动和释放也不能因重绘被丢弃。 */
static bool shell_control_event_valid(const os_control_event_t *event) {
    if (os_control_event_node(&g_tree,g_nodes,event)) return true;
    bool stable_action = event->action==OS_CONTROL_KEY ||
        (g_mouse_selecting && (event->action==OS_CONTROL_UP || event->action==OS_CONTROL_MOVE));
    return g_tree.count && stable_action && event->target==1U &&
        event->node_index==0U && event->node_kind==OS_CONTROL_CONTAINER &&
        event->user_data==0U && event->tree_revision && event->tree_revision<=g_tree.revision;
}

static bool handle_event(const os_window_event_t *event) {
    uint64_t pixels;
    os_window_event_t translated;

    if (event == 0) return false;
    if (event->type==OS_WINDOW_EVENT_CONTROL) {
        if (!shell_control_event_valid(&event->control)) return false;
        translated=*event;translated.type=OS_WINDOW_EVENT_INPUT;
        translated.input=(os_input_event_t){.code=event->control.code,.value=event->control.value};
        switch (event->control.action) {
        case OS_CONTROL_KEY: translated.input.type=OS_INPUT_EVENT_KEY;break;
        case OS_CONTROL_WHEEL: case OS_CONTROL_MOVE:
            translated.input.type=OS_INPUT_EVENT_RELATIVE;break;
        case OS_CONTROL_DOWN: case OS_CONTROL_UP:
            translated.input.type=OS_INPUT_EVENT_BUTTON;break;
        default:return false;
        }
        event=&translated;
    }
    if (event->type == OS_WINDOW_EVENT_MINIMIZED) {
        g_mouse_selecting = 0U;
        return true;
    }

    /* V2.3 协作式关闭 */
    if (event->type ==
        OS_WINDOW_EVENT_CLOSE_REQUEST) {

        gshell_exit(0U);
    }

    if (event->type == OS_WINDOW_EVENT_RESIZE) {
        if (event->resize.width == 0U || event->resize.height == 0U) return false;
        pixels = (uint64_t)event->resize.width * event->resize.height;
        if (pixels > event->resize.buffer_size / sizeof(uint32_t)) return false;
        g_shell.width = event->resize.width;
        g_shell.height = event->resize.height;
        g_mouse_selecting = 0U;
        render_window();
        return true;
    }
    if (event->type == OS_WINDOW_EVENT_INPUT && handle_shell_mouse(event)) return true;
    if (event->type == OS_WINDOW_EVENT_INPUT &&
        event->input.type == OS_INPUT_EVENT_RELATIVE &&
        event->input.code == OS_INPUT_REL_WHEEL &&
        event->input.value != 0 && g_ctrl) {
        (void)ui_text_adjust(event->input.value > 0 ? 1 : -1);
        render_window();
        return true;
    }
    return handle_key(event);
}

__attribute__((noreturn)) void gshell_entry(void) {
    if (gshell_syscall_two(OS_SYS_FILE_GETCWD, (uint64_t)(uintptr_t)g_cwd,
                           sizeof(g_cwd)) <= 0 ||
        !ui_text_init(OS_TEXT_DEFAULT_SIZE) ||
        !setup_windows()) gshell_exit(1U);
    append_output("GRAPHICAL SHELL READY");
    append_output("WINDOW CONTENT IS COMPOSITED BY KERNEL");
    append_output("TYPE HELP FOR COMMANDS");
    append_output("CTRL +/- FONT  CTRL+WHEEL FONT");
    render_window();
    for (;;) {
        reap_background_children();
        os_window_event_read_t request = {0};
        int64_t status;
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_shell.identifier;
        request.timeout_ns = g_background_children != 0U ?
                             SHELL_BACKGROUND_WAIT_NS : SHELL_EVENT_WAIT;
        status = gshell_syscall_one(OS_SYS_WINDOW_EVENT_READ, (uint64_t)&request);
        if (status == 0) {
            (void)handle_event(&request.event);
        } else if (status != -11 && status != -110) {
            __asm__ volatile ("pause");
        }
    }
}

int main(void) {
    gshell_entry();
}
