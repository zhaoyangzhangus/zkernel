#include <stdint.h>
#include <stdbool.h>
#if OS_REALTEST
#include <stdio.h>
#endif

#include <uapi/all.h>

#include "../../runtime/text.h"
#include "../../runtime/caption.h"
#include "../../runtime/caption_tree.h"
#include "../../runtime/message_box.h"
#include "../../runtime/file_picker.h"
#include "../../runtime/modal_tree.h"
#include "../../runtime/clipboard.h"

/* 编辑器命中、换行与统一控件绘制必须使用同一字符单元。 */
#undef OS_TEXT_WIDTH
#define OS_TEXT_WIDTH os_control_text_cell_width(' ',ui_text_size())

#define NOTEPAD_TEXT_CAPACITY 8192U
#define NOTEPAD_CONTENT_TOP 0U
#define NOTEPAD_MAP_BASE      0x08000000ULL
#define NOTEPAD_EVENT_WAIT OS_WAIT_INFINITE
#define NOTEPAD_FILE_PATH     "/notes.txt"
#define NOTEPAD_UNDO_DEPTH    8U
#define NOTEPAD_PATH_CAPACITY 256U
#define NOTEPAD_CONTENT_BACKGROUND OS_THEME_SURFACE
#define NOTEPAD_CONTENT_TEXT       OS_THEME_TEXT
#define NOTEPAD_CURSOR             OS_THEME_ACCENT

typedef struct notepad_window {
    os_handle_t handle;
    uint32_t identifier;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} notepad_window_t;

typedef struct notepad_snapshot {
    char text[NOTEPAD_TEXT_CAPACITY];
    size_t length;
    size_t cursor;
} notepad_snapshot_t;

typedef struct notepad_history {
    notepad_snapshot_t snapshots[NOTEPAD_UNDO_DEPTH];
    uint32_t count;
    uint32_t next;
} notepad_history_t;

static char g_file_path[NOTEPAD_PATH_CAPACITY] = NOTEPAD_FILE_PATH;
static notepad_window_t g_window = {OS_INVALID_HANDLE, 0U, 0U, 0U, 0};
static char g_text[NOTEPAD_TEXT_CAPACITY];
static char g_load_text[NOTEPAD_TEXT_CAPACITY];
static size_t g_text_length;
static size_t g_cursor;
static size_t g_selection_anchor = SIZE_MAX;
static bool g_mouse_selecting;
static uint32_t g_scroll_line;
static bool g_follow_cursor = true;
static bool g_shift;
static bool g_ctrl;
static bool g_dirty;
static ui_message_box_t g_message;
static ui_file_picker_t g_picker;
static bool g_named_document=true;
static bool g_picker_overwrite;
enum notepad_pending { NOTEPAD_PENDING_NONE, NOTEPAD_PENDING_CLOSE,
                      NOTEPAD_PENDING_NEW, NOTEPAD_PENDING_RELOAD, NOTEPAD_PENDING_OPEN };
static enum notepad_pending g_pending;
static notepad_history_t g_undo;
static notepad_history_t g_redo;
static char g_saved_text[NOTEPAD_TEXT_CAPACITY];
static size_t g_saved_length;
static uint32_t g_target_width;
static uint32_t g_target_height;
static os_control_node_t g_nodes[OS_CONTROL_TREE_MAX_NODES];
static char g_tree_text[OS_CONTROL_TREE_MAX_TEXT];
static os_window_control_tree_t g_tree;
static uint32_t g_node_count,g_text_used,g_caption_height;
static uint32_t g_text_run_right;
static bool g_tree_overflow;
static bool g_published_modal;

/* MinGW 在标准 main 入口前会发出 __main；System 用户程序不链接 CRT。 */
void __main(void) {
}

static int64_t notepad_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t notepad_syscall_four(uint64_t number, uint64_t arg0,
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

__attribute__((noreturn)) static void notepad_exit(uint64_t status) {
    (void)notepad_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static bool text_equals(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static bool set_file_path(const char *argument) {
    uint32_t length = 0U;
    uint32_t prefix_length = 0U;
    if (argument == 0 || argument[0] == '\0') return false;
    if (argument[0] == '/') {
        while (argument[length] != '\0') {
            if (++length >= NOTEPAD_PATH_CAPACITY) return false;
        }
        for (uint32_t index = 0U; index <= length; ++index) {
            g_file_path[index] = argument[index];
        }
        return true;
    }
    static const char root_prefix[] = "/";
    while (root_prefix[prefix_length] != '\0') ++prefix_length;
    while (argument[length] != '\0') {
        if (prefix_length + length + 1U >= NOTEPAD_PATH_CAPACITY) return false;
        ++length;
    }
    for (uint32_t index = 0U; index < prefix_length; ++index) {
        g_file_path[index] = root_prefix[index];
    }
    for (uint32_t index = 0U; index <= length; ++index) {
        g_file_path[prefix_length + index] = argument[index];
    }
    return true;
}

static void set_status(const char *text) {
    /* 常规编辑不弹出提示；明确的结果和错误通过对话框显示。 */
    if (!text || text_equals(text, "UNDO") || text_equals(text, "REDO") ||
        text_equals(text, "NOTHING TO UNDO") || text_equals(text, "NOTHING TO REDO") ||
        text_equals(text, "NEW DOCUMENT - CTRL+S TO SAVE") ||
        text_equals(text, "CTRL+S SAVE  CTRL+Z UNDO  CTRL+/- FONT  CTRL+Q EXIT")) return;
    ui_message_show(&g_message, "记事本", text, OS_MESSAGE_OK);
    g_mouse_selecting = false;
}

static void snapshot_capture(notepad_snapshot_t *snapshot) {
    if (snapshot == 0) return;
    snapshot->length = g_text_length;
    snapshot->cursor = g_cursor;
    for (size_t index = 0U; index <= g_text_length; ++index) {
        snapshot->text[index] = g_text[index];
    }
}

static void snapshot_restore(const notepad_snapshot_t *snapshot) {
    if (snapshot == 0 || snapshot->length >= NOTEPAD_TEXT_CAPACITY) return;
    g_selection_anchor = SIZE_MAX;
    g_mouse_selecting = false;
    g_text_length = snapshot->length;
    g_cursor = snapshot->cursor <= g_text_length ? snapshot->cursor : g_text_length;
    for (size_t index = 0U; index <= g_text_length; ++index) {
        g_text[index] = snapshot->text[index];
    }
}

static void push_snapshot(notepad_history_t *history) {
    if (history == 0) return;
    snapshot_capture(&history->snapshots[history->next]);
    history->next = (history->next + 1U) % NOTEPAD_UNDO_DEPTH;
    if (history->count < NOTEPAD_UNDO_DEPTH) ++history->count;
}

static bool pop_snapshot(notepad_history_t *history) {
    if (history == 0 || history->count == 0U) return false;
    history->next = (history->next + NOTEPAD_UNDO_DEPTH - 1U) % NOTEPAD_UNDO_DEPTH;
    snapshot_restore(&history->snapshots[history->next]);
    --history->count;
    return true;
}

static void history_reset(void) {
    g_selection_anchor = SIZE_MAX;
    g_mouse_selecting = false;
    g_undo.count = g_undo.next = 0U;
    g_redo.count = g_redo.next = 0U;
}

static void begin_edit(void) {
    push_snapshot(&g_undo);
    g_redo.count = g_redo.next = 0U;
}

static void update_dirty(void) {
    if (g_text_length != g_saved_length) {
        g_dirty = true;
        return;
    }
    g_dirty = false;
    for (size_t index = 0U; index < g_saved_length; ++index) {
        if (g_text[index] != g_saved_text[index]) {
            g_dirty = true;
            return;
        }
    }
}

static void mark_saved(void) {
    g_saved_length = g_text_length;
    for (size_t index = 0U; index <= g_saved_length; ++index) {
        g_saved_text[index] = g_text[index];
    }
    g_dirty = false;
}

static void undo_document(void) {
    if (g_undo.count == 0U) {
        set_status("NOTHING TO UNDO");
        return;
    }
    push_snapshot(&g_redo);
    (void)pop_snapshot(&g_undo);
    update_dirty();
    set_status("UNDO");
}

static void redo_document(void) {
    if (g_redo.count == 0U) {
        set_status("NOTHING TO REDO");
        return;
    }
    push_snapshot(&g_undo);
    (void)pop_snapshot(&g_redo);
    update_dirty();
    set_status("REDO");
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      uint32_t color) {
    if (!width || !height || x >= g_target_width || y >= g_target_height) return;
    if (width > g_target_width - x) width = g_target_width - x;
    if (height > g_target_height - y) height = g_target_height - y;
    /* 同一行的选区合并成一个节点，不按字符产生海量小控件。 */
    for (uint32_t back=1;back<=2U && back<g_node_count;++back) {
        os_control_node_t *last=&g_nodes[g_node_count-back];
        if (last->kind==OS_CONTROL_CONTAINER && last->background==color &&
            last->y==(int32_t)y && last->height==(int32_t)height &&
            (int64_t)last->x+last->width==x) {
            last->width+=(int32_t)width;return;
        }
    }
    if (g_node_count==OS_CONTROL_TREE_MAX_NODES) {g_tree_overflow=true;return;}
    uint32_t index=g_node_count++;
    g_nodes[index]=(os_control_node_t){.identifier=0x20000000U+index,
        .parent=index?0:OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
        .flags=index?OS_CONTROL_POINTER_PASSTHROUGH:OS_CONTROL_CLIP_CHILDREN,
        .x=(int32_t)x,.y=(int32_t)y,.width=(int32_t)width,.height=(int32_t)height,
        .background=color,.foreground=NOTEPAD_CONTENT_TEXT,.font_size=ui_text_size()};
}

static void draw_character(uint32_t x, uint32_t y, uint32_t character, uint32_t color) {
    if (x>=g_target_width || y>=g_target_height) return;
    char encoded[4];size_t length=os_utf8_encode(character,encoded);
    if (length>sizeof(g_tree_text)-g_text_used) {g_tree_overflow=true;return;}
    os_control_node_t *last=g_node_count?&g_nodes[g_node_count-1U]:0;
    if (last && last->kind==OS_CONTROL_TEXT && last->foreground==color &&
        last->y==(int32_t)y && g_text_run_right==x) {
        __builtin_memcpy(g_tree_text+g_text_used,encoded,length);
        g_text_used+=(uint32_t)length;last->text_length+=(uint32_t)length;
        g_text_run_right=x+os_control_text_cell_width(character,ui_text_size());return;
    }
    if (g_node_count==OS_CONTROL_TREE_MAX_NODES) {g_tree_overflow=true;return;}
    uint32_t index=g_node_count++;
    g_nodes[index]=(os_control_node_t){.identifier=0x20000000U+index,.parent=0,.kind=OS_CONTROL_TEXT,.value=OS_CONTROL_TEXT_APP_SELECTION,
        .flags=OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_INHERIT_FONT,
        .x=(int32_t)x,.y=(int32_t)y,.width=(int32_t)(g_target_width-x),
        .height=(int32_t)OS_TEXT_HEIGHT,.foreground=color,.text_offset=g_text_used,.text_length=(uint32_t)length};
    __builtin_memcpy(g_tree_text+g_text_used,encoded,length);g_text_used+=(uint32_t)length;
    g_text_run_right=x+os_control_text_cell_width(character,ui_text_size());
}

static uint32_t editor_columns(uint32_t width, uint32_t cell_width) {
    uint32_t columns = width > 20U && cell_width != 0U ?
                       (width - 20U) / cell_width : 0U;
    return columns != 0U ? columns : 1U;
}

static uint32_t editor_visible_lines(uint32_t height, uint32_t caption_height,
                                     uint32_t status_height, uint32_t cell_height) {
    if (cell_height == 0U || height <= caption_height ||
        height - caption_height <= status_height) return 0U;
    return (height - caption_height - status_height) / cell_height;
}

/* 宽字符放不下时先换行；即使窗口只有一个单元，也始终能向前推进。 */
static void prepare_position(uint32_t character,uint32_t columns,uint32_t *line,uint32_t *column) {
    uint32_t cells=character<128U?1U:2U;
    if (cells>columns) cells=columns;
    if (character!='\n' && character!='\t' && *column && cells>columns-*column) {
        ++*line;*column=0;
    }
}

static void advance_position(uint32_t character, uint32_t columns,
                             uint32_t *line, uint32_t *column) {
    prepare_position(character,columns,line,column);
    if (character == '\n') {
        ++*line;
        *column = 0U;
    } else if (character == '\t') {
        uint32_t next = (*column + 4U) & ~3U;
        if (next >= columns) {
            ++*line;
            *column = 0U;
        } else {
            *column = next;
        }
    } else {
        uint32_t cells=character<128U?1U:2U;
        *column+=cells>columns?columns:cells;
        if (*column >= columns) {
            ++*line;
            *column = 0U;
        }
    }
}

static void cursor_position(uint32_t columns, uint32_t *line, uint32_t *column) {
    *line = 0U;
    *column = 0U;
    for (size_t index = 0U; index < g_cursor;) {
        size_t used;uint32_t code=os_utf8_next(g_text+index,g_text_length-index,&used);
        advance_position(code,columns,line,column);index+=used?used:1U;
    }
    if (g_cursor<g_text_length) {
        size_t used;uint32_t code=os_utf8_next(g_text+g_cursor,g_text_length-g_cursor,&used);
        prepare_position(code,columns,line,column);
    }
}

static size_t offset_at_position(uint32_t columns, uint32_t wanted_line,
                                 uint32_t wanted_column) {
    uint32_t line = 0U;
    uint32_t column = 0U;
    size_t previous=0;
    for (size_t index = 0U; index <= g_text_length;) {
        size_t used=0;
        uint32_t code=index<g_text_length?os_utf8_next(g_text+index,g_text_length-index,&used):0;
        uint32_t old_line=line;
        if (index<g_text_length) prepare_position(code,columns,&line,&column);
        if (old_line==wanted_line && line>wanted_line) return index;
        if (line == wanted_line && column >= wanted_column) return index;
        if (index == g_text_length) break;
        if (line > wanted_line) return previous;
        if (line==wanted_line && code>=128U && columns>1U && wanted_column==column+1U)
            return index+(used?used:1U);
        previous=index;
        advance_position(code,columns,&line,&column);index+=used?used:1U;
    }
    return g_text_length;
}

static void ensure_cursor_visible(uint32_t line, uint32_t visible_lines) {
    if (visible_lines == 0U) return;
    if (line < g_scroll_line) g_scroll_line = line;
    if (visible_lines != 0U && line >= g_scroll_line + visible_lines) {
        g_scroll_line = line - visible_lines + 1U;
    }
}

static void draw_editor(void) {
    const uint32_t text_top = NOTEPAD_CONTENT_TOP;
    const uint32_t status_height = 0U;
    const uint32_t cell_width = OS_TEXT_WIDTH;
    const uint32_t cell_height = OS_TEXT_HEIGHT;
    uint32_t columns = editor_columns(g_target_width, cell_width);
    uint32_t visible_lines = editor_visible_lines(g_target_height, text_top,
                                                  status_height, cell_height);
    uint32_t line = 0U;
    uint32_t column = 0U;
    uint32_t cursor_line = 0U;
    uint32_t cursor_column = 0U;
    if (visible_lines != 0U) {
        cursor_position(columns, &cursor_line, &cursor_column);
        if (g_follow_cursor) ensure_cursor_visible(cursor_line, visible_lines);
    }
    fill_rect(0U, 0U, g_target_width, g_target_height,
              NOTEPAD_CONTENT_BACKGROUND);
    const size_t selection_start = g_selection_anchor < g_cursor ? g_selection_anchor : g_cursor;
    const size_t selection_end = g_selection_anchor > g_cursor ? g_selection_anchor : g_cursor;
    for (size_t index = 0U; visible_lines != 0U && index < g_text_length;) {
        size_t used;uint32_t character=os_utf8_next(g_text+index,g_text_length-index,&used);
        prepare_position(character,columns,&line,&column);
        if (line >= g_scroll_line && line - g_scroll_line >= visible_lines) break;
        if (g_selection_anchor != SIZE_MAX && index >= selection_start &&
            index < selection_end && line >= g_scroll_line &&
            line < g_scroll_line + visible_lines) {
            uint32_t cells = character == '\t' ? 4U - (column & 3U) : character<128U?1U:2U;
            if (cells > columns - column) cells = columns - column;
            fill_rect(10U + column * cell_width,
                text_top + (line - g_scroll_line) * cell_height,
                cells * cell_width, cell_height, OS_THEME_SELECTED);
        }
        if (character != '\n' && character != '\t' && line >= g_scroll_line &&
            line < g_scroll_line + visible_lines) {
            draw_character(10U + column * cell_width,
                           text_top + (line - g_scroll_line) * cell_height,
                           character, NOTEPAD_CONTENT_TEXT);
        }
        if (character == '\t' && line >= g_scroll_line &&
            line < g_scroll_line + visible_lines) {
            uint32_t spaces = 4U - (column & 3U);
            for (uint32_t space = 0U; space < spaces && column + space < columns; ++space) {
                draw_character(10U + (column + space) * cell_width,
                               text_top + (line - g_scroll_line) * cell_height,
                               ' ', NOTEPAD_CONTENT_TEXT);
            }
        }
        advance_position(character, columns, &line, &column);
        index+=used?used:1U;
    }
    if (cursor_line >= g_scroll_line && cursor_line < g_scroll_line + visible_lines &&
        cursor_column < columns) {
        fill_rect(10U + cursor_column * cell_width,
                  text_top + (cursor_line - g_scroll_line) * cell_height,
                  2U, cell_height, NOTEPAD_CURSOR);
    }
}

static bool create_window(void) {
    os_display_info_t display = {0};
    os_window_create_t request = {0};
    uint32_t width;
    uint32_t height;
    display.hdr.size = sizeof(display);
    display.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (notepad_syscall_one(OS_SYS_DISPLAY_GET_INFO, (uint64_t)&display) < 0 ||
        display.width < 320U || display.height < 240U) return false;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    width = display.width * 3U / 4U;
    height = display.height * 3U / 4U;
    if (width < 640U && display.width > 640U) width = 640U;
    if (height < 420U && display.height > 420U) height = 420U;
    /* 大屏也不按屏幕比例撑满，保留后续缩放与最大化。 */
    if (width > 720U) width = 720U;
    if (height > 480U) height = 480U;
    request.x = (int32_t)((display.width - width) / 2U);
    request.y = (int32_t)((display.height - height) / 2U);
    request.width = width;
    request.height = height;
    request.flags = OS_WINDOW_VISIBLE |
                    OS_WINDOW_RESIZABLE |
                    OS_WINDOW_STANDARD_CAPTION_CONTROLS | OS_WINDOW_CAPTION_HEIGHT(user_caption_height());
    request.background = NOTEPAD_CONTENT_BACKGROUND;
    user_caption_format_title(request.title, "记事本", false);
    request.address = NOTEPAD_MAP_BASE;
    if (notepad_syscall_one(OS_SYS_WINDOW_CREATE, (uint64_t)&request) != 0 ||
        request.window == OS_INVALID_HANDLE || request.address == 0U) return false;
    g_window.handle = request.window;
    g_window.identifier = request.identifier;
    g_window.width = request.width;
    g_window.height = request.height;
    g_window.pixels = (uint32_t *)(uintptr_t)request.address;
    return true;
}

/* 标题栏按钮与正文共用控件树，正文仍保持客户端坐标。 */
static bool sync_caption_tree(void) {
    static const char label[] = "打开";
    if (g_node_count > OS_CONTROL_TREE_MAX_NODES - 2U ||
        sizeof(label)-1U > sizeof(g_tree_text)-g_text_used) return false;
    uint32_t caption=user_caption_height(),font=ui_text_size();
    for (uint32_t i=g_node_count;i>0U;--i) {
        g_nodes[i]=g_nodes[i-1U];
        g_nodes[i].parent=i==1U?0U:g_nodes[i].parent+1U;
    }
    g_nodes[1].identifier=0x1ffffff0U;
    g_nodes[1].y=(int32_t)caption;
    g_nodes[0]=(os_control_node_t){.identifier=0x20000000U,
        .parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_WINDOW_FRAME|OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_POINTER_PASSTHROUGH,
        .width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL,.font_size=font};
    ++g_node_count;
    uint32_t size=os_window_caption_button_width(OS_WINDOW_CAPTION_HEIGHT(caption));
    uint32_t width=2U*font+24U,gap=8U;
    bool fits=g_window.width>=3U*size+width+2U*gap;
    memcpy(g_tree_text+g_text_used,label,sizeof(label)-1U);
    g_nodes[g_node_count++]=(os_control_node_t){.identifier=0x1ffffff1U,.parent=0U,
        .kind=OS_CONTROL_BUTTON,.flags=fits?0U:OS_CONTROL_HIDDEN,
        .x=fits?(int32_t)(g_window.width-3U*size-width-gap):0,
        .y=4,.width=(int32_t)width,.height=(int32_t)caption-8,
        .radius=6,.font_size=font,.foreground=OS_THEME_TEXT,.background=OS_THEME_FIELD,
        .text_offset=g_text_used,.text_length=sizeof(label)-1U};
    g_text_used+=sizeof(label)-1U;
    char title[32];user_caption_format_app_title(title,"记事本",g_file_path,g_dirty);
    return user_caption_tree(g_nodes,&g_node_count,OS_CONTROL_TREE_MAX_NODES,g_tree_text,&g_text_used,
        sizeof(g_tree_text),(user_caption_layout_t){.title=title,.width=g_window.width,
            .height=caption,.font=font,.title_width=fits?g_window.width-3U*size-width-2U*gap:0U});
}

static bool sync_message_tree(void) {
    if (!g_message.active) return true;
    ui_message_document_t document;
    return ui_message_build(&g_message,g_window.identifier,g_window.width,g_window.height,&document) &&
        ui_modal_tree_attach(g_nodes,&g_node_count,OS_CONTROL_TREE_MAX_NODES,
            g_tree_text,&g_text_used,sizeof(g_tree_text),&document.request);
}

static bool sync_file_picker_tree(void) {
    if (!g_picker.active) return true;
    ui_file_picker_document_t document;
    return ui_file_picker_build(&g_picker,g_window.identifier,g_window.width,g_window.height,&document) &&
        ui_modal_tree_attach(g_nodes,&g_node_count,OS_CONTROL_TREE_MAX_NODES,
            g_tree_text,&g_text_used,sizeof(g_tree_text),&document.request);
}

static void update_window(void) {
    os_window_update_t request = {0};
    uint32_t caption=user_caption_height();
    request.flags = OS_WINDOW_CAPTION_HEIGHT(caption);
    os_window_title_t title = {0};
    title.hdr.size = sizeof(title);
    title.hdr.version = OS_SYSCALL_ABI_VERSION;
    title.identifier = g_window.identifier;
    user_caption_format_app_title(title.title, "记事本", g_file_path, g_dirty);
    (void)notepad_syscall_one(OS_SYS_WINDOW_TITLE, (uint64_t)&title);
    if (caption==g_caption_height) return;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.identifier = g_window.identifier;
    request.width=g_window.width;request.height=g_window.height;
    if (notepad_syscall_one(OS_SYS_WINDOW_UPDATE,(uintptr_t)&request)==0) g_caption_height=caption;
}

static void render(void) {
    g_target_width = g_window.width;
    g_target_height = g_window.height;
    if (!g_target_width || !g_target_height) return;
    g_node_count=g_text_used=0;g_tree_overflow=false;
    draw_editor();
    if (g_tree_overflow || !sync_caption_tree() ||
        !(g_message.active?sync_message_tree():sync_file_picker_tree())) {
        g_tree.count=0;g_message.active=false;ui_file_picker_close(&g_picker);g_pending=NOTEPAD_PENDING_NONE;return;
    }
    os_window_control_tree_t tree={.hdr={sizeof(tree),OS_SYSCALL_ABI_VERSION},
        .identifier=g_window.identifier,.count=g_node_count,.nodes=(uintptr_t)g_nodes,
        .text=(uintptr_t)g_tree_text,.text_size=g_text_used};
    if (notepad_syscall_one(OS_SYS_WINDOW_CONTROL_TREE,(uintptr_t)&tree)!=0) {
        /* 发布失败不能执行不可见的确认动作，也不能使用被覆盖的旧节点元数据。 */
        g_tree.count=0;g_message.active=false;ui_file_picker_close(&g_picker);g_pending=NOTEPAD_PENDING_NONE;return;
    }
    g_tree=tree;
    bool modal=g_message.active || g_picker.active;
    if (g_published_modal!=modal) g_ctrl=g_shift=false;
    g_published_modal=modal;
    update_window();
}

static bool load_document(void) {
    os_handle_t handle = OS_INVALID_HANDLE;
    uint64_t total = 0U;
    bool success = true;
    int64_t opened = notepad_syscall_four(OS_SYS_FILE_OPEN, (uint64_t)g_file_path,
                                         OS_FILE_OPEN_READ, 0U, (uint64_t)&handle);
    if (opened < 0 || handle == OS_INVALID_HANDLE) {
        if (opened != -2 || g_text_length != 0U || g_dirty)
            set_status("LOAD FAILED: FILE CANNOT BE OPENED");
        return false;
    }
    while (total + 1U < NOTEPAD_TEXT_CAPACITY) {
        uint64_t bytes = 0U;
        uint64_t capacity = NOTEPAD_TEXT_CAPACITY - 1U - total;
        int64_t status = notepad_syscall_four(OS_SYS_FILE_READ, handle,
                                              (uint64_t)(g_load_text + total), capacity,
                                              (uint64_t)&bytes);
        if (status < 0 || bytes > capacity) {
            success = false;
            set_status("LOAD FAILED");
            break;
        }
        total += bytes;
        if (bytes == 0U) break;
    }
    if (success && total + 1U == NOTEPAD_TEXT_CAPACITY) {
        char extra;
        uint64_t bytes = 0U;
        if (notepad_syscall_four(OS_SYS_FILE_READ, handle, (uint64_t)&extra,
                                 1U, (uint64_t)&bytes) < 0 || bytes != 0U) {
            success = false;
            set_status("LOAD FAILED: DOCUMENT EXCEEDS EDITOR CAPACITY");
        }
    }
    (void)notepad_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
    if (!success) return false;
    for (size_t i = 0U; i < total; ++i) g_text[i] = g_load_text[i];
    g_text_length = (size_t)total;
    g_cursor = g_text_length;
    g_scroll_line = 0U;
    g_follow_cursor = true;
    g_text[g_text_length] = '\0';
    history_reset();
    mark_saved();
    if (success) {
        set_status("CTRL+S SAVE  CTRL+Z UNDO  CTRL+/- FONT  CTRL+Q EXIT");
    }
    return success;
}

static bool remove_temporary_document(const char *path) {
    os_file_path_op_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)path;
    int64_t status = notepad_syscall_one(OS_SYS_FILE_REMOVE, (uint64_t)(uintptr_t)&request);
    return status == 0 || status == -2;
}

static bool save_document(void) {
    os_handle_t handle = OS_INVALID_HANDLE;
    uint64_t total = 0U;
    static uint32_t sequence;
    static const char suffix[] = ".np-save-00000000.tmp";
    static const char hex[] = "0123456789abcdef";
    char temporary[NOTEPAD_PATH_CAPACITY];
    size_t length = 0U, directory = 0U;
    while (length < NOTEPAD_PATH_CAPACITY && g_file_path[length] != '\0') {
        if (g_file_path[length] == '/') directory = length + 1U;
        ++length;
    }
    if (length == 0U || length == NOTEPAD_PATH_CAPACITY ||
        g_file_path[0] != '/' || g_file_path[length - 1U] == '/' ||
        directory + sizeof(suffix) > sizeof(temporary)) {
        set_status("SAVE FAILED: INVALID OR LONG PATH");
        return false;
    }
    for (size_t i = 0U; i < directory; ++i) temporary[i] = g_file_path[i];
    for (size_t i = 0U; i < sizeof(suffix); ++i) temporary[directory + i] = suffix[i];
    for (uint32_t attempt = 0U; attempt < 32U; ++attempt) {
        uint32_t value = ++sequence;
        for (uint32_t digit = 0U; digit < 8U; ++digit)
            temporary[directory + 9U + digit] = hex[(value >> (28U - digit * 4U)) & 15U];
        bool same_path = length == directory + sizeof(suffix) - 1U;
        for (size_t i = 0U; same_path && i < length; ++i)
            same_path = temporary[i] == g_file_path[i];
        if (same_path) continue;
        handle = OS_INVALID_HANDLE;
        int64_t status = notepad_syscall_four(OS_SYS_FILE_OPEN, (uint64_t)(uintptr_t)temporary,
            OS_FILE_OPEN_WRITE | OS_FILE_OPEN_CREATE | OS_FILE_OPEN_EXCLUSIVE,
            0U, (uint64_t)(uintptr_t)&handle);
        if (status == -17) {
            handle = OS_INVALID_HANDLE;
            continue; /* 此名称由另一个编辑器占用。 */
        }
        if (status < 0 || handle == OS_INVALID_HANDLE) {
            set_status("SAVE FAILED: CANNOT CREATE TEMPORARY FILE");
            return false;
        }
        break;
    }
    if (handle == OS_INVALID_HANDLE) {
        set_status("SAVE FAILED: TEMPORARY NAMES ARE BUSY");
        return false;
    }
    while (total < g_text_length) {
        uint64_t bytes = 0U;
        uint64_t capacity = g_text_length - total;
        int64_t status = notepad_syscall_four(OS_SYS_FILE_WRITE, handle,
                                              (uint64_t)(g_text + total), capacity,
                                              (uint64_t)&bytes);
        if (status < 0 || bytes == 0U || bytes > capacity) {
            set_status("SAVE FAILED");
            goto failed;
        }
        total += bytes;
    }
    if (notepad_syscall_one(OS_SYS_FILE_FSYNC, handle) < 0) {
        set_status("SAVE FAILED: FLUSH ERROR");
        goto failed;
    }
    int64_t closed = notepad_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
    handle = OS_INVALID_HANDLE;
    if (closed < 0) {
        set_status("SAVE FAILED: CLOSE ERROR");
        goto failed;
    }
    os_file_rename_t renamed = {0};
    renamed.hdr.size = sizeof(renamed);
    renamed.hdr.version = OS_SYSCALL_ABI_VERSION;
    renamed.old_path = (uint64_t)(uintptr_t)temporary;
    renamed.new_path = (uint64_t)(uintptr_t)g_file_path;
    if (notepad_syscall_one(OS_SYS_FILE_RENAME, (uint64_t)(uintptr_t)&renamed) < 0) {
        set_status("SAVE FAILED: REPLACE ERROR");
        goto failed;
    }
    mark_saved();
    return true;
failed:
    if (handle != OS_INVALID_HANDLE) (void)notepad_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
    if (!remove_temporary_document(temporary))
        set_status("SAVE FAILED: TEMPORARY FILE CLEANUP FAILED");
    return false;
}

static void show_file_picker(bool save) {
    g_mouse_selecting=false;g_picker_overwrite=false;
    (void)ui_file_picker_show(&g_picker,save?UI_PICKER_SAVE_FILE:UI_PICKER_OPEN_FILE,
        g_file_path,notepad_syscall_one);
}

static void request_action(enum notepad_pending action) {
    if (g_dirty) {
        g_pending = action;
        g_mouse_selecting = false;
        ui_message_show(&g_message, "尚未保存",
            "是否保存修改后继续？", OS_MESSAGE_SAVE_DISCARD_CANCEL);
    } else if (action == NOTEPAD_PENDING_CLOSE) notepad_exit(0U);
    else if (action == NOTEPAD_PENDING_OPEN) show_file_picker(false);
}

static void new_document(void) {
    if (g_dirty) {
        request_action(NOTEPAD_PENDING_NEW);
        return;
    }
    if (g_text_length != 0U) begin_edit();
    g_text[0] = '\0';
    g_text_length = 0U;
    g_cursor = 0U;
    g_scroll_line = 0U;
    g_selection_anchor = SIZE_MAX;
    g_follow_cursor = true;
    g_dirty = true;
    g_named_document=false;g_file_path[0]=0;
    set_status("NEW DOCUMENT - CTRL+S TO SAVE");
}

static void reload_document(void) {
    if (g_dirty) {
        request_action(NOTEPAD_PENDING_RELOAD);
        return;
    }
    if (load_document()) set_status("RELOADED");
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

static bool has_selection(void) {
    return g_selection_anchor != SIZE_MAX && g_selection_anchor != g_cursor;
}

static void complete_message(enum ui_message_result result) {
    enum notepad_pending action = g_pending;
    if (result==OS_MESSAGE_SAVE && !g_named_document) {
        show_file_picker(true);return;
    }
    g_pending = NOTEPAD_PENDING_NONE;
    if (result == OS_MESSAGE_CANCEL || action == NOTEPAD_PENDING_NONE) return;
    if (result == OS_MESSAGE_SAVE && !save_document()) return;
    if (action == NOTEPAD_PENDING_CLOSE) notepad_exit(0U);
    if (action == NOTEPAD_PENDING_NEW) {
        /* 用户已确认允许替换未保存的文档。 */
        g_dirty = false;
        new_document();
    } else if (action == NOTEPAD_PENDING_RELOAD) {
        if (load_document()) set_status("RELOADED");
    } else if (action == NOTEPAD_PENDING_OPEN) show_file_picker(false);
}

static void commit_file_picker(bool overwrite) {
    char path[UI_PICKER_PATH];
    if (!ui_file_picker_result(&g_picker,path,sizeof(path))) {g_picker.active=true;return;}
    if (g_picker.save && !overwrite) {
        os_file_stat_t stat={.hdr={sizeof(stat),OS_SYSCALL_ABI_VERSION},.path=(uintptr_t)path};
        int64_t status=notepad_syscall_one(OS_SYS_FILE_STAT,(uintptr_t)&stat);
        if (!status) {
            if (stat.info.type!=OS_FILE_TYPE_REGULAR) {
                g_picker.active=true;ui_message_copy(g_picker.error,sizeof(g_picker.error),"所选路径不是普通文件");return;
            }
            g_picker_overwrite=true;
            ui_message_show(&g_message,"覆盖文件","文件已存在，是否替换？",OS_MESSAGE_YES_NO);return;
        }
        if (status!=-2) {
            g_picker.active=true;ui_message_copy(g_picker.error,sizeof(g_picker.error),"无法检查目标文件");return;
        }
    }
    char previous[sizeof(g_file_path)];
    __builtin_memcpy(previous,g_file_path,sizeof(previous));
    __builtin_memcpy(g_file_path,path,__builtin_strlen(path)+1U);
    bool success=g_picker.save?save_document():load_document();
    if (!success) {
        __builtin_memcpy(g_file_path,previous,sizeof(g_file_path));
        /* 失败只回到选择器；保存重试成功后仍需完成原来的关闭或新建操作。 */
        g_picker.active=true;
        ui_message_copy(g_picker.error,sizeof(g_picker.error),
            g_picker.save?"保存失败，请检查路径后重试":"打开失败，请检查路径后重试");
        return;
    }
    g_named_document=true;ui_file_picker_close(&g_picker);
    if (g_pending!=NOTEPAD_PENDING_NONE) complete_message(OS_MESSAGE_DISCARD);
}

static void reflow_editor(void) {
    /* 从首行开始，再将插入光标滚动到可见区域。此操作也会清除因视口小于文档而遗留的
     * 滚动偏移。 */
    g_scroll_line = 0U;
    g_follow_cursor = true;
}

/* 调用方为整个替换操作记录一个撤销快照。 */
static void erase_selection(void) {
    if (!has_selection()) { g_selection_anchor = SIZE_MAX; return; }
    size_t first = g_selection_anchor < g_cursor ? g_selection_anchor : g_cursor;
    size_t last = g_selection_anchor > g_cursor ? g_selection_anchor : g_cursor;
    for (size_t index = last; index <= g_text_length; ++index)
        g_text[first + index - last] = g_text[index];
    g_text_length -= last - first;
    g_cursor = first;
    g_selection_anchor = SIZE_MAX;
    g_dirty = true;
}

/* 剪切必须先成功发布；粘贴先检查容量，再作为一个可撤销操作替换选区。 */
static void copy_selection(bool cut) {
    if (!has_selection()) return;
    size_t first=g_selection_anchor<g_cursor?g_selection_anchor:g_cursor;
    size_t last=g_selection_anchor>g_cursor?g_selection_anchor:g_cursor;
    if (ui_clipboard_copy(notepad_syscall_one,g_text+first,last-first) && cut) {
        begin_edit();erase_selection();
    }
}
static void paste_selection(void) {
    size_t removed=has_selection()?(g_selection_anchor>g_cursor?
        g_selection_anchor-g_cursor:g_cursor-g_selection_anchor):0U;
    size_t length=0;
    char *text=ui_clipboard_paste(notepad_syscall_one,
        NOTEPAD_TEXT_CAPACITY-1U-(g_text_length-removed),&length);
    if (!text) return;
    /* 文档使用零结尾文本，拒绝嵌入零而不是悄悄截断。 */
    if (!memchr(text,0,length)) {
        begin_edit();erase_selection();
        memmove(g_text+g_cursor+length,g_text+g_cursor,g_text_length-g_cursor+1U);
        memcpy(g_text+g_cursor,text,length);g_cursor+=length;g_text_length+=length;g_dirty=true;
    }
    free(text);
}

static void insert_character(char character) {
    if (!has_selection() && g_text_length + 1U >= NOTEPAD_TEXT_CAPACITY) {
        set_status("DOCUMENT IS FULL");
        return;
    }
    begin_edit();
    erase_selection();
    for (size_t index = g_text_length + 1U; index > g_cursor; --index) {
        g_text[index] = g_text[index - 1U];
    }
    g_text[g_cursor++] = character;
    ++g_text_length;
    g_text[g_text_length] = '\0';
    g_dirty = true;
}

static void backspace(void) {
    if (has_selection()) { begin_edit(); erase_selection(); return; }
    g_selection_anchor = SIZE_MAX;
    if (g_cursor == 0U) return;
    begin_edit();
    size_t first=os_utf8_previous(g_text,g_text_length,g_cursor);
    for (size_t index = g_cursor; index <= g_text_length; ++index) {
        g_text[first+index-g_cursor] = g_text[index];
    }
    g_text_length-=g_cursor-first;g_cursor=first;
    g_dirty = true;
}

static void delete_character(void) {
    if (has_selection()) { begin_edit(); erase_selection(); return; }
    g_selection_anchor = SIZE_MAX;
    if (g_cursor >= g_text_length) return;
    begin_edit();
    size_t last=os_utf8_advance(g_text,g_text_length,g_cursor);
    for (size_t index = last; index <= g_text_length; ++index) {
        g_text[g_cursor+index-last] = g_text[index];
    }
    g_text_length-=last-g_cursor;
    g_dirty = true;
}

static void move_vertical_by(int32_t direction, uint32_t distance) {
    uint32_t columns = editor_columns(g_window.width, OS_TEXT_WIDTH);
    uint32_t line = 0U;
    uint32_t column = 0U;
    cursor_position(columns, &line, &column);
    if (direction < 0) {
        if (distance > line) line = 0U;
        else line -= distance;
    } else {
        if (distance > UINT32_MAX - line) line = UINT32_MAX;
        else line += distance;
    }
    g_cursor = offset_at_position(columns, line, column);
}

static void move_vertical(int32_t direction) {
    move_vertical_by(direction, 1U);
}

static void scroll_editor(int32_t direction) {
    uint32_t columns = editor_columns(g_window.width, OS_TEXT_WIDTH);
    uint32_t visible_lines = editor_visible_lines(g_window.height, NOTEPAD_CONTENT_TOP,
                                                  0U, OS_TEXT_HEIGHT);
    if (visible_lines == 0U || direction == 0) return;
    uint32_t target;
    if(direction<0) {
        uint32_t distance=0U-(uint32_t)direction;
        target=distance>g_scroll_line?0U:g_scroll_line-distance;
    } else target=(uint32_t)direction>UINT32_MAX-g_scroll_line?UINT32_MAX:g_scroll_line+(uint32_t)direction;
    /* 行号只增不减，确认目标视口存在后不再扫描余下正文；回到顶部无需扫描。 */
    uint64_t required=(uint64_t)target+visible_lines;
    uint32_t line = 0U;
    uint32_t column = 0U;
    uint32_t total_lines;
    uint32_t maximum;
    for (size_t index = 0U; target && index < g_text_length && (uint64_t)line+1U<required;) {
        size_t used;uint32_t code=os_utf8_next(g_text+index,g_text_length-index,&used);
        advance_position(code,columns,&line,&column);index+=used?used:1U;
    }
    total_lines = line == UINT32_MAX ? UINT32_MAX : line + 1U;
    maximum = total_lines > visible_lines ? total_lines - visible_lines : 0U;
    g_scroll_line=target>maximum?maximum:target;
    g_follow_cursor = false;
}

static void page_editor(int32_t direction) {
    uint32_t page = editor_visible_lines(g_window.height, NOTEPAD_CONTENT_TOP,
                                         0U, OS_TEXT_HEIGHT);
    if (page > 1000000000U) page = 1000000000U;
    scroll_editor(direction < 0 ? -(int32_t)page : (int32_t)page);
}

static bool key_matches(const os_input_event_t *input, uint32_t hid_code,
                        char ascii_code) {
    return input != 0 &&
           (input->code == hid_code || input->code == (uint32_t)ascii_code);
}

static bool handle_key(const os_window_event_t *event) {
    const os_input_event_t *input = event != 0 ? &event->input : 0;
    char character;
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
    g_follow_cursor = true;
    bool navigation = input->code >= 0x4AU && input->code <= 0x52U && input->code != 0x4CU;
    if (navigation) {
        if (g_shift && g_selection_anchor == SIZE_MAX) g_selection_anchor = g_cursor;
        else if (!g_shift) g_selection_anchor = SIZE_MAX;
    }
    if (g_ctrl && input->code == 0x2EU) {
        (void)ui_text_adjust(1);
        reflow_editor();
    } else if (g_ctrl && input->code == 0x2DU) {
        (void)ui_text_adjust(-1);
        reflow_editor();
    } else if (g_ctrl && key_matches(input, 0x04U, 'A')) {
        g_selection_anchor = 0U;
        g_cursor = g_text_length;
    } else if (g_ctrl && key_matches(input, 0x06U, 'C')) {
        copy_selection(false);
    } else if (g_ctrl && key_matches(input, 0x1BU, 'X')) {
        copy_selection(true);
    } else if (g_ctrl && key_matches(input, 0x19U, 'V')) {
        paste_selection();
    } else if (g_ctrl && key_matches(input, 0x16U, 'S')) {
        if (g_shift || !g_named_document) show_file_picker(true);
        else if (save_document()) set_status("SAVED");
    } else if (g_ctrl && key_matches(input, 0x14U, 'Q')) {
        request_action(NOTEPAD_PENDING_CLOSE);
    } else if (g_ctrl && key_matches(input, 0x1DU, 'Z')) {
        undo_document();
    } else if (g_ctrl && key_matches(input, 0x1CU, 'Y')) {
        redo_document();
    } else if (g_ctrl && key_matches(input, 0x11U, 'N')) {
        new_document();
    } else if (g_ctrl && key_matches(input, 0x12U, 'O')) {
        request_action(NOTEPAD_PENDING_OPEN);
    } else if (g_ctrl && key_matches(input, 0x15U, 'R')) {
        reload_document();
    } else if (g_ctrl && input->code == 0x4AU) {
        g_cursor = 0U;
    } else if (g_ctrl && input->code == 0x4DU) {
        g_cursor = g_text_length;
    } else if (input->code == 0x4FU) {
        g_cursor=os_utf8_advance(g_text,g_text_length,g_cursor);
    } else if (input->code == 0x50U) {
        g_cursor=os_utf8_previous(g_text,g_text_length,g_cursor);
    } else if (input->code == 0x52U) {
        move_vertical(-1);
    } else if (input->code == 0x51U) {
        move_vertical(1);
    } else if (input->code == 0x4BU) {
        page_editor(-1);
    } else if (input->code == 0x4EU) {
        page_editor(1);
    } else if (input->code == 0x4AU) {
        while (g_cursor != 0U && g_text[g_cursor - 1U] != '\n') --g_cursor;
    } else if (input->code == 0x4DU) {
        while (g_cursor < g_text_length && g_text[g_cursor] != '\n') ++g_cursor;
    } else if (input->code == 0x4CU) {
        delete_character();
    } else if (input->code == 0x2AU) {
        backspace();
    } else if (input->code == 0x28U || input->code == 0x58U) {
        insert_character('\n');
    } else if (input->code == 0x2BU) {
        insert_character('\t');
    } else if (!g_ctrl) {
        character = key_to_ascii(input->code, g_shift);
        if (character != '\0') insert_character(character);
    }
    render();
    return true;
}

static size_t editor_mouse_offset(int32_t x, int32_t y) {
    uint32_t cell_width = OS_TEXT_WIDTH;
    uint32_t cell_height = OS_TEXT_HEIGHT;
    uint32_t columns = editor_columns(g_window.width, cell_width);
    int32_t top = (int32_t)NOTEPAD_CONTENT_TOP;
    uint32_t visible_lines = editor_visible_lines(g_window.height, (uint32_t)top,
                                                  0U, cell_height);
    if (visible_lines == 0U) return g_cursor;
    int32_t bottom = top + (int32_t)(visible_lines * cell_height);
    if (y < top) y = top;
    if (y >= bottom) y = bottom > top ? bottom - 1 : top;
    uint32_t line = g_scroll_line + (uint32_t)(y - top) / cell_height;
    uint32_t column = x > 10 ? ((uint32_t)(x - 10) + cell_width / 2U) / cell_width : 0U;
    if (column >= columns) column = columns - 1U;
    return offset_at_position(columns, line, column);
}

static bool handle_editor_mouse(const os_window_event_t *event) {
    const os_input_event_t *input = &event->input;
    size_t previous_cursor=g_cursor,previous_anchor=g_selection_anchor;
    uint32_t previous_scroll=g_scroll_line;
    if (input->type == OS_INPUT_EVENT_BUTTON && input->code == OS_INPUT_BUTTON_LEFT) {
        if (input->value == OS_INPUT_VALUE_RELEASE) {
            if (!g_mouse_selecting) return false;
            g_cursor = editor_mouse_offset(event->pointer_x, event->pointer_y);
            g_mouse_selecting = false;
        } else if (input->value == OS_INPUT_VALUE_PRESS &&
                   editor_visible_lines(g_window.height, NOTEPAD_CONTENT_TOP,
                                        0U, OS_TEXT_HEIGHT) != 0U &&
                   event->pointer_y >= (int32_t)NOTEPAD_CONTENT_TOP &&
                   (int64_t)event->pointer_y < (int64_t)g_window.height) {
            size_t next=editor_mouse_offset(event->pointer_x,event->pointer_y);
            if (!g_shift || g_selection_anchor == SIZE_MAX)
                g_selection_anchor = g_shift ? g_cursor : next;
            g_cursor = next;
            g_mouse_selecting = true;
        } else return false;
    } else if (input->type == OS_INPUT_EVENT_RELATIVE && g_mouse_selecting &&
               (input->code == OS_INPUT_REL_X || input->code == OS_INPUT_REL_Y)) {
        if (event->pointer_y < (int32_t)NOTEPAD_CONTENT_TOP) scroll_editor(-1);
        else if ((int64_t)event->pointer_y >= (int64_t)g_window.height) scroll_editor(1);
        size_t next = editor_mouse_offset(event->pointer_x, event->pointer_y);
        g_cursor = next;
    } else return false;
    g_follow_cursor = false;
    /* 选区端点和视口独立判定，自动滚动不能因端点不变而漏刷。 */
    if(previous_cursor!=g_cursor || previous_anchor!=g_selection_anchor || previous_scroll!=g_scroll_line)
        render();
    return true;
}

static bool handle_message_event(const os_window_event_t *event) {
    if (!event || !g_message.active ||
        (event->type != OS_WINDOW_EVENT_INPUT && event->type != OS_WINDOW_EVENT_CONTROL &&
         event->type != OS_WINDOW_EVENT_CLOSE_REQUEST)) return false;
    /* 模态期间仍同步修饰键释放，避免回到编辑器后留下粘滞状态。 */
    const os_input_event_t *raw = event->type == OS_WINDOW_EVENT_INPUT &&
        event->input.type == OS_INPUT_EVENT_KEY ? &event->input : 0;
    bool control_key = event->type == OS_WINDOW_EVENT_CONTROL && event->control.action == OS_CONTROL_KEY;
    if (raw || control_key) {
        uint32_t code = raw ? raw->code : event->control.code;
        int32_t value = raw ? raw->value : event->control.value;
        if (code == 0xE0U || code == 0xE4U) g_ctrl = value != OS_INPUT_VALUE_RELEASE;
        if (code == 0xE1U || code == 0xE5U) g_shift = value != OS_INPUT_VALUE_RELEASE;
    }
    enum ui_message_result result = ui_message_event(&g_message, event);
    if (result == OS_MESSAGE_NONE) return true;
    if (g_picker_overwrite) {
        g_picker_overwrite=false;
        if (result==OS_MESSAGE_YES) commit_file_picker(true);
        else g_picker.active=true;
    } else complete_message(result);
    render();
    return true;
}

static bool handle_file_picker_event(const os_window_event_t *event) {
    if (!g_picker.active || (event->type!=OS_WINDOW_EVENT_INPUT &&
        event->type!=OS_WINDOW_EVENT_CONTROL && event->type!=OS_WINDOW_EVENT_CLOSE_REQUEST)) return false;
    bool raw=event->type==OS_WINDOW_EVENT_INPUT && event->input.type==OS_INPUT_EVENT_KEY;
    bool key=event->type==OS_WINDOW_EVENT_CONTROL && event->control.action==OS_CONTROL_KEY;
    uint32_t code=raw?event->input.code:event->control.code;
    int32_t value=raw?event->input.value:event->control.value;
    if (event->type==OS_WINDOW_EVENT_CONTROL && !key &&
        event->control.action!=OS_CONTROL_CLICK && event->control.action!=OS_CONTROL_WHEEL) return true;
    if (event->type==OS_WINDOW_EVENT_INPUT && !raw) return true;
    bool changed=false;
    if ((raw || key) && g_picker.ctrl && value!=OS_INPUT_VALUE_RELEASE && (code==0x2eU || code==0x2dU)) {
        changed=ui_text_adjust(code==0x2eU?1:-1);if(changed)reflow_editor();
    } else {
        enum ui_file_picker_event_result result=ui_file_picker_event(&g_picker,event,
            (raw || key)?key_to_ascii(code,g_picker.shift):0);
        if (result==UI_PICKER_SELECTED) commit_file_picker(false);
        else if (result==UI_PICKER_CANCELLED) g_pending=NOTEPAD_PENDING_NONE;
        changed=result!=UI_PICKER_IDLE;
    }
    if(changed)render();
    return true;
}

static bool handle_caption_event(const os_window_event_t *event) {
    if (event->type!=OS_WINDOW_EVENT_CONTROL || event->control.target!=0x1ffffff1U)
        return false;
    if (event->control.action==OS_CONTROL_CLICK) {
        request_action(NOTEPAD_PENDING_OPEN);
        render();
    }
    return true;
}

/* 正文根在重绘后保持同一语义，但模态窗口出现后不得接收正文的旧事件。 */
static bool editor_control_event_valid(const os_control_event_t *event) {
    if (os_control_event_node(&g_tree,g_nodes,event)) return true;
    bool stable_action = event->action==OS_CONTROL_KEY ||
        (g_mouse_selecting && (event->action==OS_CONTROL_UP || event->action==OS_CONTROL_MOVE));
    return g_tree.count && g_tree.revision && !g_message.active && !g_picker.active &&
        stable_action && event->target==0x20000000U && !event->node_index &&
        event->node_kind==OS_CONTROL_CONTAINER && !event->user_data &&
        event->tree_revision && event->tree_revision<=g_tree.revision;
}

static bool handle_event(const os_window_event_t *event) {
    const os_input_event_t *input;
    uint64_t pixels;
    os_window_event_t translated;

    if (event == 0) return false;
    if (event->type==OS_WINDOW_EVENT_CONTROL &&
        !editor_control_event_valid(&event->control)) {
        /* 文件名逐键重绘时，允许同一代控件已排队的按键，不放行旧目录的点击。 */
        if (g_picker.active && !g_message.active && event->control.action==OS_CONTROL_KEY &&
            event->control.tree_revision && event->control.tree_revision<g_tree.revision &&
            (event->control.target&~255U)==ui_file_picker_target(&g_picker,0)) {
            translated=*event;translated.control.tree_revision=g_tree.revision;
            if (!os_control_event_node(&g_tree,g_nodes,&translated.control)) return false;
            return handle_file_picker_event(&translated);
        }
        return false;
    }
    if (event->type == OS_WINDOW_EVENT_MINIMIZED) {
        g_mouse_selecting = false;
        return true;
    }

    if (handle_message_event(event)) return true;
    if (handle_file_picker_event(event)) return true;
    if (handle_caption_event(event)) return true;
    if (event->type == OS_WINDOW_EVENT_CLOSE_REQUEST) {
        request_action(NOTEPAD_PENDING_CLOSE);
        render();
        return true;
    }

    if (event->type == OS_WINDOW_EVENT_RESIZE) {
        if (event->resize.width == 0U || event->resize.height == 0U) return false;
        pixels = (uint64_t)event->resize.width * event->resize.height;
        if (pixels > event->resize.buffer_size / sizeof(uint32_t)) return false;
        g_window.width = event->resize.width;
        g_window.height = event->resize.height;
        g_mouse_selecting = false;
        reflow_editor();
        render();
        return true;
    }
    if (event->type==OS_WINDOW_EVENT_CONTROL) {
        translated=*event;translated.type=OS_WINDOW_EVENT_INPUT;
        translated.input=(os_input_event_t){.code=event->control.code,.value=event->control.value};
        switch (event->control.action) {
        case OS_CONTROL_KEY:translated.input.type=OS_INPUT_EVENT_KEY;break;
        case OS_CONTROL_WHEEL:case OS_CONTROL_MOVE:translated.input.type=OS_INPUT_EVENT_RELATIVE;break;
        case OS_CONTROL_DOWN:case OS_CONTROL_UP:translated.input.type=OS_INPUT_EVENT_BUTTON;break;
        default:return false;
        }
        event=&translated;
    }
    if (event->type != OS_WINDOW_EVENT_INPUT) return false;

    input = &event->input;
    if (handle_editor_mouse(event)) return true;
    if (input->type == OS_INPUT_EVENT_RELATIVE &&
        input->code == OS_INPUT_REL_WHEEL && input->value != 0) {
        if (g_ctrl) {
            (void)ui_text_adjust(input->value > 0 ? 1 : -1);
            reflow_editor();
            render();
            return true;
        }
        scroll_editor(input->value > 0 ? -3 : 3);
        render();
        return true;
    }
    return handle_key(event);
}

int main(int argc, char **argv) {
    if (!ui_text_init(OS_TEXT_DEFAULT_SIZE)) {
        notepad_exit(1U);
    }
    if (argc > 1 && (argv == 0 || argv[1] == 0 || !set_file_path(argv[1]))) {
        notepad_exit(1U);
    }
    if (!create_window()) notepad_exit(1U);
    g_named_document=load_document();
    render();
#if OS_REALTEST
    (void)fprintf(stderr, "OS_NOTEPAD_READY SIZE=%u\n",
                  ui_text_size());
#endif
    for (;;) {
        os_window_event_read_t request = {0};
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_window.identifier;
        request.timeout_ns = NOTEPAD_EVENT_WAIT;
        int64_t status = notepad_syscall_one(OS_SYS_WINDOW_EVENT_READ,
                                             (uint64_t)&request);
        if (status == 0) {
            (void)handle_event(&request.event);
        } else if (status != -11 && status != -110) {
            __asm__ volatile ("pause");
        }
    }
}

__attribute__((noreturn)) void notepad_entry(void) {
    uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
    uint64_t *initial_stack = (uint64_t *)(frame + sizeof(uint64_t));
    int argc = (int)initial_stack[0];
    char **argv = (char **)(initial_stack + 1U);
    int status = main(argc, argv);
    notepad_exit(status < 0 ? 1U : (uint64_t)status);
}
