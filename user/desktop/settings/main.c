#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/utsname.h>

#include <uapi/all.h>

#include "../../runtime/text.h"
#include "../../runtime/caption.h"
#include "../../runtime/caption_tree.h"
#include <uapi/window_theme.h>

#define SETTINGS_MAP_BASE          0x0C000000ULL
#define SETTINGS_MIN_WIDTH         640U
#define SETTINGS_MIN_HEIGHT        400U
#define SETTINGS_CONTENT_BACKGROUND OS_THEME_SURFACE
#define SETTINGS_CARD_BACKGROUND    OS_THEME_SURFACE
#define SETTINGS_CARD_BORDER        OS_THEME_BORDER
#define SETTINGS_SECTION_TEXT       OS_THEME_MUTED
#define SETTINGS_VALUE_TEXT         OS_THEME_TEXT
#define SETTINGS_HINT_TEXT          OS_THEME_MUTED
#define SETTINGS_COMMAND_TEXT       OS_THEME_MUTED
#define SETTINGS_CONNECTED_TEXT     0x002D8A63U
#define SETTINGS_DISCONNECTED_TEXT  0x00B56A28U
#define SETTINGS_FOOTER_BACKGROUND  OS_THEME_TOOLBAR
#define SETTINGS_ERROR_TEXT         0x00B24F3EU

typedef struct settings_window {
    os_handle_t handle;
    uint32_t identifier;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} settings_window_t;

static settings_window_t g_window = {
    OS_INVALID_HANDLE, 0U, 0U, 0U, 0
};
static os_net_status_t g_status;
static bool g_status_valid;
static bool g_ctrl;

#define SETTINGS_NODE_CAPACITY 192U
static os_control_node_t g_nodes[SETTINGS_NODE_CAPACITY];
static char g_tree_text[4096];
static os_control_node_t g_published_nodes[SETTINGS_NODE_CAPACITY];
static char g_published_text[4096];
static os_window_control_tree_t g_tree;
static uint32_t g_node_count,g_text_used,g_caption_height;
static bool g_tree_overflow;

void __main(void) {
}

static int64_t settings_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

__attribute__((noreturn)) static void settings_exit(uint64_t status) {
    if (g_window.handle != OS_INVALID_HANDLE) {
        (void)settings_syscall_one(OS_SYS_HANDLE_CLOSE, g_window.handle);
        g_window.handle = OS_INVALID_HANDLE;
    }
    (void)settings_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static int64_t settings_get_status(os_net_status_t *status) {
    if (status == 0) return -22;
    *status = (os_net_status_t){0};
    status->hdr.size = sizeof(*status);
    status->hdr.version = OS_SYSCALL_ABI_VERSION;
    return settings_syscall_one(OS_SYS_NET_GET_STATUS, (uint64_t)status);
}

static uint32_t text_length(const char *text) {
    uint32_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0') ++length;
    return length;
}

static void append_character(char *destination, uint32_t capacity, char value) {
    uint32_t length;
    if (destination == 0 || capacity == 0U) return;
    length = text_length(destination);
    if (length + 1U >= capacity) return;
    destination[length] = value;
    destination[length + 1U] = '\0';
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

static void append_decimal(char *destination, uint32_t capacity, uint64_t value) {
    char digits[24];
    uint32_t count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U) {
        append_character(destination, capacity, digits[--count]);
    }
}

static char hex_digit(uint8_t value) {
    value &= 0x0FU;
    return value < 10U ? (char)('0' + value) :
                         (char)('A' + value - 10U);
}

static void append_hex_byte(char *destination, uint32_t capacity, uint8_t value) {
    append_character(destination, capacity, hex_digit((uint8_t)(value >> 4U)));
    append_character(destination, capacity, hex_digit(value));
}

static void append_hex_group(char *destination, uint32_t capacity,
                             uint16_t value) {
    char digits[4];
    uint32_t first = 0U;
    for (uint32_t index = 0U; index < 4U; ++index) {
        digits[3U - index] = hex_digit((uint8_t)value);
        value >>= 4U;
    }
    while (first < 3U && digits[first] == '0') ++first;
    for (uint32_t index = first; index < 4U; ++index) {
        append_character(destination, capacity, digits[index]);
    }
}

static void format_ipv4(char *output, uint32_t capacity, uint32_t address) {
    if (output == 0 || capacity == 0U) return;
    output[0] = '\0';
    if (address == 0U) {
        append_text(output, capacity, "未配置");
        return;
    }
    append_decimal(output, capacity, (address >> 24U) & 0xFFU);
    append_character(output, capacity, '.');
    append_decimal(output, capacity, (address >> 16U) & 0xFFU);
    append_character(output, capacity, '.');
    append_decimal(output, capacity, (address >> 8U) & 0xFFU);
    append_character(output, capacity, '.');
    append_decimal(output, capacity, address & 0xFFU);
}

static void format_ipv4_prefix(char *output, uint32_t capacity,
                               uint32_t address, uint8_t prefix) {
    format_ipv4(output, capacity, address);
    if (address != 0U) {
        append_character(output, capacity, '/');
        append_decimal(output, capacity, prefix);
    }
}

static void format_mac(char *output, uint32_t capacity, const uint8_t mac[6]) {
    if (output == 0 || capacity == 0U || mac == 0) return;
    output[0] = '\0';
    for (uint32_t index = 0U; index < 6U; ++index) {
        if (index != 0U) append_character(output, capacity, ':');
        append_hex_byte(output, capacity, mac[index]);
    }
}

static void format_ipv6(char *output, uint32_t capacity,
                        const uint8_t address[16], bool configured) {
    uint16_t groups[8];
    uint32_t best_start = 8U;
    uint32_t best_length = 0U;
    if (output == 0 || capacity == 0U) return;
    output[0] = '\0';
    if (!configured) {
        append_text(output, capacity, "Not configured");
        return;
    }
    for (uint32_t group = 0U; group < 8U; ++group) {
        groups[group] = (uint16_t)(((uint16_t)address[group * 2U] << 8U) |
                                   address[group * 2U + 1U]);
    }
    for (uint32_t start = 0U; start < 8U;) {
        uint32_t length = 0U;
        while (start + length < 8U && groups[start + length] == 0U) ++length;
        if (length >= 2U && length > best_length) {
            best_start = start;
            best_length = length;
        }
        start += length != 0U ? length : 1U;
    }
    for (uint32_t group = 0U; group < 8U;) {
        if (group == best_start) {
            append_character(output, capacity, ':');
            append_character(output, capacity, ':');
            group += best_length;
            continue;
        }
        if (text_length(output) != 0U &&
            output[text_length(output) - 1U] != ':') {
            append_character(output, capacity, ':');
        }
        append_hex_group(output, capacity, groups[group]);
        ++group;
    }
}

#include "network_tree.h"

static uint32_t g_scroll,g_scroll_limit;
enum { SETTINGS_NETWORK, SETTINGS_SOUND, SETTINGS_SYSTEM, SETTINGS_APPEARANCE };
static uint32_t g_page;
static uint32_t settings_scale(uint32_t value) {
    return (value*ui_text_size()+8U)/16U;
}
static uint32_t settings_box(uint32_t parent,uint32_t kind,uint32_t layout,
    int32_t width,int32_t height,uint32_t background) {
    if (g_node_count>=SETTINGS_NODE_CAPACITY) {g_tree_overflow=true;return 0;}
    uint32_t index=g_node_count++;
    g_nodes[index]=(os_control_node_t){.identifier=index+1U,.parent=parent,.kind=kind,
        .layout=layout,.width=width,.height=height,.background=background,
        .foreground=SETTINGS_VALUE_TEXT,
        .flags=OS_CONTROL_INHERIT_FONT|OS_CONTROL_CLIP_CHILDREN};
    return index;
}
static void settings_label(uint32_t parent,const char *text,uint32_t color) {
    uint32_t index=settings_box(parent,OS_CONTROL_TEXT,OS_CONTROL_ABSOLUTE,
        OS_CONTROL_FILL,(int32_t)ui_text_line_height(),0);
    os_control_node_t *node=g_nodes+index;
    uint32_t bytes=text_length(text);
    if (!index || bytes>sizeof(g_tree_text)-g_text_used) {g_tree_overflow=true;return;}
    node->flags|=OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_BACKGROUND_ALPHA;
    node->foreground=color;node->text_offset=g_text_used;node->text_length=bytes;
    memcpy(g_tree_text+g_text_used,text,bytes);g_text_used+=bytes;
}
static uint32_t settings_card(uint32_t parent,uint32_t rows) {
    uint32_t index=settings_box(parent,OS_CONTROL_CONTAINER,OS_CONTROL_COLUMN,
        OS_CONTROL_FILL,(int32_t)(rows*ui_text_line_height()+settings_scale(32U)),
        SETTINGS_CARD_BACKGROUND);
    g_nodes[index].padding=settings_scale(16U);g_nodes[index].radius=settings_scale(8U);
    g_nodes[index].border_width=1U;g_nodes[index].border_color=SETTINGS_CARD_BORDER;
    return index;
}
static void settings_value(uint32_t parent,const char *label,const char *value) {
    char line[160]={0};append_text(line,sizeof(line),label);append_text(line,sizeof(line),value);
    settings_label(parent,line,SETTINGS_VALUE_TEXT);
}
static void settings_navigation(uint32_t parent,const char *text,uint32_t identifier,bool active) {
    settings_label(parent,text,SETTINGS_VALUE_TEXT);
    os_control_node_t *item=g_nodes+g_node_count-1U;
    item->identifier=identifier;item->kind=OS_CONTROL_LIST_ITEM;
    item->flags=OS_CONTROL_INHERIT_FONT|OS_CONTROL_CLIP_CHILDREN|(active ? OS_CONTROL_CHECKED : 0U);
    item->height=(int32_t)(ui_text_line_height()+settings_scale(16U));
    item->background=OS_THEME_TOOLBAR;item->radius=settings_scale(6U);
}

#include "system_info.h"
#include "sound.h"
#include "appearance.h"
#include "adapters.h"

static void render(void) {
    if (!g_window.width || !g_window.height) return;
    g_node_count=0;g_text_used=0;g_tree_overflow=false;
    uint32_t root=settings_box(OS_CONTROL_NO_PARENT,OS_CONTROL_CONTAINER,OS_CONTROL_ROW,
        OS_CONTROL_FILL,OS_CONTROL_FILL,SETTINGS_CONTENT_BACKGROUND);
    g_nodes[root].flags=OS_CONTROL_CLIP_CHILDREN;g_nodes[root].font_size=ui_text_size();
    uint32_t sidebar=settings_scale(180U);
    if (sidebar>g_window.width/2U) sidebar=g_window.width/2U;
    uint32_t nav=settings_box(root,OS_CONTROL_CONTAINER,OS_CONTROL_COLUMN,
        (int32_t)sidebar,OS_CONTROL_FILL,OS_THEME_TOOLBAR);
    g_nodes[nav].padding=settings_scale(12U);
    settings_navigation(nav,"网络",1001U,g_page==SETTINGS_NETWORK);
    settings_navigation(nav,"声音",1002U,g_page==SETTINGS_SOUND);
    settings_navigation(nav,"系统信息",1003U,g_page==SETTINGS_SYSTEM);
    settings_navigation(nav,"外观",1004U,g_page==SETTINGS_APPEARANCE);

    uint32_t page=settings_box(root,OS_CONTROL_CONTAINER,OS_CONTROL_COLUMN,
        OS_CONTROL_FILL,OS_CONTROL_FILL,SETTINGS_CONTENT_BACKGROUND);
    g_nodes[page].flags|=OS_CONTROL_FLEX_GROW;
    g_nodes[page].padding=settings_scale(18U);g_nodes[page].gap=settings_scale(12U);
    uint32_t viewport=settings_box(page,OS_CONTROL_CONTAINER,OS_CONTROL_COLUMN,
        OS_CONTROL_FILL,OS_CONTROL_FILL,SETTINGS_CONTENT_BACKGROUND);
    g_nodes[viewport].flags|=OS_CONTROL_FLEX_GROW;
    if (g_page==SETTINGS_SOUND) settings_sound_content(viewport);
    else if (g_page==SETTINGS_SYSTEM) settings_system_content(viewport);
    else if (g_page==SETTINGS_APPEARANCE) settings_appearance_content(viewport);
    else settings_network_content(viewport);

    if (!user_caption_tree(g_nodes,&g_node_count,SETTINGS_NODE_CAPACITY,g_tree_text,&g_text_used,
            sizeof(g_tree_text),(user_caption_layout_t){.title="设置",.width=g_window.width,
                .height=user_caption_height(),.font=ui_text_size()})) return;
    settings_publish();
}

static bool refresh_status(void) {
    if (g_page==SETTINGS_APPEARANCE) {settings_appearance_refresh();render();return g_appearance_valid;}
    if (g_page==SETTINGS_SOUND) {settings_sound_refresh();render();return g_sound_valid;}
    if (g_page==SETTINGS_SYSTEM) {settings_system_refresh();render();return true;}
    settings_adapters_refresh();
    os_net_status_t status;
    if (settings_get_status(&status) < 0) {
        g_status_valid = false;
        render();
        return false;
    }
    g_status = status;
    g_status_valid = true;
    render();
    return true;
}

static bool create_window(void) {
    os_display_info_t display = {0};
    os_window_create_t request = {0};

    display.hdr.size = sizeof(display);
    display.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (settings_syscall_one(OS_SYS_DISPLAY_GET_INFO,
                           (uint64_t)&display) < 0 ||
        display.width < 320U || display.height < 240U) {
        return false;
    }

    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.width = display.width > 1008U ? 960U :
                    (display.width > 48U ? display.width - 48U :
                                           display.width);
    request.height = display.height > 630U ? 560U :
                     (display.height > 70U ? display.height - 70U :
                                             display.height);
    if (request.width < SETTINGS_MIN_WIDTH &&
        display.width >= SETTINGS_MIN_WIDTH) {
        request.width = SETTINGS_MIN_WIDTH;
    }
    if (request.height < SETTINGS_MIN_HEIGHT &&
        display.height >= SETTINGS_MIN_HEIGHT) {
        request.height = SETTINGS_MIN_HEIGHT;
    }
    request.x = display.width > request.width ?
        (int32_t)((display.width - request.width) / 2U) : 0;
    request.y = display.height > request.height ?
        (int32_t)((display.height - request.height) / 2U) : 0;
    request.flags = OS_WINDOW_VISIBLE |
                    OS_WINDOW_RESIZABLE |
                    OS_WINDOW_STANDARD_CAPTION_CONTROLS | OS_WINDOW_CAPTION_HEIGHT(user_caption_height());
    request.background = SETTINGS_CONTENT_BACKGROUND;
    user_caption_format_title(request.title, "设置", false);
    request.address = SETTINGS_MAP_BASE;

    if (settings_syscall_one(OS_SYS_WINDOW_CREATE,
                           (uint64_t)&request) != 0 ||
        request.window == OS_INVALID_HANDLE ||
        request.identifier == 0U ||
        request.address == 0U) {
        return false;
    }

    g_window.handle = request.window;
    g_window.identifier = request.identifier;
    g_window.width = request.width;
    g_window.height = request.height;
    g_window.pixels = (uint32_t *)(uintptr_t)request.address;
    return true;
}

static void handle_key(const os_window_event_t *event) {
    const os_input_event_t *input;

    if (event == 0 || event->type != OS_WINDOW_EVENT_INPUT) return;
    input = &event->input;
    if (input->type != OS_INPUT_EVENT_KEY) return;

    if (input->code == 0xE0U || input->code == 0xE4U) {
        g_ctrl = input->value != OS_INPUT_VALUE_RELEASE;
        return;
    }
    if (input->value == OS_INPUT_VALUE_RELEASE) return;

    if (g_ctrl &&
        (input->code == 0x14U || input->code == (uint32_t)'Q')) {
        settings_exit(0U);
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
}

static void handle_event(const os_window_event_t *event) {
    uint64_t pixels;

    if (event == 0) return;
    os_window_event_t translated;
    if (event->type==OS_WINDOW_EVENT_CONTROL) {
        if (!settings_control_valid(&event->control)) return;
        if (event->control.action==OS_CONTROL_CLICK) {
            if (g_page==SETTINGS_SOUND && settings_sound_click(event->control.target)) {
                render();return;
            }
            if (g_page==SETTINGS_APPEARANCE && settings_appearance_click(event->control.target)) {
                render();return;
            }
            if (event->control.target>=1001U && event->control.target<=1004U) {
                uint32_t page=event->control.target-1001U;
                if (page!=g_page) g_scroll=0;
                g_page=page;
                /* 再次点击当前栏目也读取一次，缩放和重绘只使用已采样数据。 */
                (void)refresh_status();
            }
            return;
        }
        translated=*event;translated.type=OS_WINDOW_EVENT_INPUT;
        translated.input=(os_input_event_t){.code=event->control.code,.value=event->control.value};
        if (event->control.action==OS_CONTROL_KEY) translated.input.type=OS_INPUT_EVENT_KEY;
        else if (event->control.action==OS_CONTROL_WHEEL) translated.input.type=OS_INPUT_EVENT_RELATIVE;
        else return;
        event=&translated;
    }

    /* 服务端统一处理窗口装饰，应用仅响应关闭请求。 */
    if (event->type ==
        OS_WINDOW_EVENT_CLOSE_REQUEST) {

        settings_exit(0U);
    }

    if (event->type == OS_WINDOW_EVENT_RESIZE) {
        if (event->resize.width == 0U ||
            event->resize.height == 0U) return;
        pixels = (uint64_t)event->resize.width *
                 event->resize.height;
        if (pixels >
            event->resize.buffer_size / sizeof(uint32_t)) return;
        g_window.width = event->resize.width;
        g_window.height = event->resize.height;
        render();
        return;
    }
    if (event->type == OS_WINDOW_EVENT_INPUT &&
        event->input.type == OS_INPUT_EVENT_RELATIVE &&
        event->input.code == OS_INPUT_REL_WHEEL &&
        event->input.value != 0 && g_ctrl) {
        (void)ui_text_adjust(event->input.value > 0 ? 1 : -1);
        render();
        return;
    }
    if (event->type==OS_WINDOW_EVENT_INPUT && event->input.type==OS_INPUT_EVENT_RELATIVE &&
        event->input.code==OS_INPUT_REL_WHEEL && event->input.value!=0) {
        uint32_t step=ui_text_line_height()*3U;
        if (event->input.value>0) g_scroll=g_scroll>step ? g_scroll-step : 0U;
        else g_scroll=g_scroll_limit-g_scroll<step ? g_scroll_limit : g_scroll+step;
        render();return;
    }
    handle_key(event);
}

static int settings_main(void) {
    if (!ui_text_init(OS_TEXT_DEFAULT_SIZE) ||
        !create_window()) return 1;
    (void)refresh_status();
    for (;;) {
        os_window_event_read_t request = {0};
        int64_t status;

        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_window.identifier;
        request.timeout_ns = UINT64_MAX;

        status = settings_syscall_one(OS_SYS_WINDOW_EVENT_READ,
                                    (uint64_t)&request);
        if (status == 0) {
            handle_event(&request.event);
        } else if (status != -4 && status != -11 && status != -110) return 1;
    }
}

int main(void) {
    return settings_main();
}

__attribute__((noreturn)) void settings_entry(void) {
    int status = settings_main();
    settings_exit(status == 0 ? 0U : 1U);
}
