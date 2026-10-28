#include <stdint.h>
#include <stdbool.h>

#include <fcntl.h>
#include <unistd.h>
#include <libc/libc.h>
#include <uapi/all.h>
#include <uapi/window_theme.h>

#include "../../runtime/text.h"
#include "../../runtime/caption.h"
#include "../../runtime/caption_tree.h"
#include "../../runtime/file_picker.h"
#include "../../runtime/modal_tree.h"

#define IMAGEVIEW_DEFAULT_PATH  "/usr/apps/desktop/wall.png"
#define IMAGEVIEW_PATH_CAPACITY 256U
#define IMAGEVIEW_MAP_BASE      0x08000000ULL
#define IMAGEVIEW_EVENT_WAIT OS_WAIT_INFINITE
#define IMAGEVIEW_STATUS_HEIGHT (ui_text_line_height()+12U)
#define IMAGEVIEW_MIN_WIDTH     640U
#define IMAGEVIEW_MIN_HEIGHT    420U

#define IMAGEVIEW_BACKGROUND     OS_THEME_SURFACE
#define IMAGEVIEW_CHECKER_LIGHT  OS_THEME_TOOLBAR
#define IMAGEVIEW_CHECKER_DARK   OS_THEME_HEADER_INACTIVE
#define IMAGEVIEW_STATUS_TEXT    OS_THEME_MUTED

typedef struct imageview_window {
    uint32_t identifier;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} imageview_window_t;

static imageview_window_t g_window = {0};
static char g_path[IMAGEVIEW_PATH_CAPACITY];
static uint8_t *g_image_pixels;
static uint32_t g_image_width;
static uint32_t g_image_height;
static uint32_t g_image_stride;
static uint32_t g_zoom_percent;
static int32_t g_pan_x;
static int32_t g_pan_y;
static bool g_shift;
static bool g_ctrl;
static bool g_running = true;
static ui_file_picker_t g_picker;
static os_control_node_t g_nodes[OS_CONTROL_TREE_MAX_NODES];
static os_window_control_tree_t g_tree;
static uint32_t g_drawn_width,g_drawn_height,g_drawn_zoom,g_caption_height;
static int32_t g_drawn_pan_x,g_drawn_pan_y;
static const uint8_t *g_drawn_image;
static bool g_content_valid;

static void copy_path(const char *source) {
    uint32_t index = 0U;

    if (source == 0) return;
    while (index + 1U < IMAGEVIEW_PATH_CAPACITY && source[index] != '\0') {
        g_path[index] = source[index];
        ++index;
    }
    g_path[index] = '\0';
}

static void imageview_cleanup(void) {
    if (g_image_pixels != 0) {
        free(g_image_pixels);
        g_image_pixels = 0;
    }
    ui_file_picker_close(&g_picker);
    ui_text_shutdown();
}

static bool load_image_file(void) {
    int descriptor;
    off_t file_size;
    uint8_t *encoded;
    uint8_t *pixels;
    uint64_t remaining;

    descriptor = open(g_path, O_RDONLY);
    if (descriptor < 0) return false;
    file_size = lseek(descriptor, 0, SEEK_END);
    if (file_size <= 0 || (uint64_t)file_size > OS_IMAGE_MAX_ENCODED_BYTES ||
        lseek(descriptor, 0, SEEK_SET) < 0) {
        (void)close(descriptor);
        return false;
    }
    encoded = (uint8_t *)malloc((size_t)file_size);
    if (encoded == 0) {
        (void)close(descriptor);
        return false;
    }
    remaining = (uint64_t)file_size;
    while (remaining != 0U) {
        ssize_t count = read(descriptor, encoded + ((uint64_t)file_size - remaining),
                             (size_t)remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || (uint64_t)count > remaining) {
            free(encoded);
            (void)close(descriptor);
            return false;
        }
        remaining -= (uint64_t)count;
    }
    (void)close(descriptor);

    os_image_info_t info = {0};
    info.hdr.size = sizeof(info);
    info.hdr.version = OS_SYSCALL_ABI_VERSION;
    info.encoded = (uint64_t)(uintptr_t)encoded;
    info.encoded_size = (uint64_t)file_size;
    if (__libc_native_syscall6(OS_SYS_IMAGE_INFO, (uint64_t)(uintptr_t)&info,
                        0U, 0U, 0U, 0U, 0U) != 0 ||
        info.format != OS_IMAGE_PIXEL_RGBA8888 || info.width == 0U ||
        info.height == 0U || (uint64_t)info.width * 4U != info.stride ||
        info.pixel_bytes != (uint64_t)info.stride * info.height ||
        info.pixel_bytes == 0U ||
        info.pixel_bytes > OS_IMAGE_MAX_PIXEL_BYTES) {
        free(encoded);
        return false;
    }

    pixels = (uint8_t *)malloc((size_t)info.pixel_bytes);
    if (pixels == 0) {
        free(encoded);
        return false;
    }
    os_image_decode_t decode = {0};
    decode.hdr.size = sizeof(decode);
    decode.hdr.version = OS_SYSCALL_ABI_VERSION;
    decode.encoded = (uint64_t)(uintptr_t)encoded;
    decode.encoded_size = (uint64_t)file_size;
    decode.pixels = (uint64_t)(uintptr_t)pixels;
    decode.capacity = info.pixel_bytes;
    if (__libc_native_syscall6(OS_SYS_IMAGE_DECODE, (uint64_t)(uintptr_t)&decode,
                        0U, 0U, 0U, 0U, 0U) != 0 ||
        decode.format != OS_IMAGE_PIXEL_RGBA8888 ||
        decode.width != info.width || decode.height != info.height ||
        decode.stride != info.stride || decode.bytes_written != info.pixel_bytes) {
        free(encoded);
        free(pixels);
        return false;
    }
    free(encoded);
    free(g_image_pixels);
    g_image_pixels = pixels;
    g_image_width = decode.width;
    g_image_height = decode.height;
    g_image_stride = decode.stride;
    return true;
}

static void clamp_pan(uint32_t content_width, uint32_t content_height) {
    uint32_t draw_width = (uint32_t)(((uint64_t)g_image_width *
                                      g_zoom_percent) / 100U);
    uint32_t draw_height = (uint32_t)(((uint64_t)g_image_height *
                                       g_zoom_percent) / 100U);
    int32_t limit_x = draw_width > content_width ?
        (int32_t)((draw_width - content_width) / 2U) : 0;
    int32_t limit_y = draw_height > content_height ?
        (int32_t)((draw_height - content_height) / 2U) : 0;

    if (g_pan_x < -limit_x) g_pan_x = -limit_x;
    if (g_pan_x > limit_x) g_pan_x = limit_x;
    if (g_pan_y < -limit_y) g_pan_y = -limit_y;
    if (g_pan_y > limit_y) g_pan_y = limit_y;
}

static uint32_t fit_zoom(uint32_t content_width, uint32_t content_height) {
    uint64_t horizontal;
    uint64_t vertical;
    uint64_t result;

    if (g_image_width == 0U || g_image_height == 0U ||
        content_width == 0U || content_height == 0U) return 1U;
    horizontal = (uint64_t)content_width * 100U / g_image_width;
    vertical = (uint64_t)content_height * 100U / g_image_height;
    result = horizontal < vertical ? horizontal : vertical;
    if (result == 0U) result = 1U;
    if (result > 100U) result = 100U;
    return (uint32_t)result;
}

static void reset_view(void) {
    uint32_t content_height = g_window.height >
        IMAGEVIEW_STATUS_HEIGHT ?
        g_window.height - IMAGEVIEW_STATUS_HEIGHT : 1U;
    g_zoom_percent = fit_zoom(g_window.width, content_height);
    g_pan_x = 0;
    g_pan_y = 0;
}

static void change_zoom(int32_t delta) {
    int32_t next = (int32_t)g_zoom_percent + delta;
    if (next < 10) next = 10;
    if (next > 400) next = 400;
    g_zoom_percent = (uint32_t)next;
    clamp_pan(g_window.width,
              g_window.height > IMAGEVIEW_STATUS_HEIGHT ?
                  g_window.height - IMAGEVIEW_STATUS_HEIGHT : 1U);
}

static void draw_image(uint32_t top, uint32_t bottom) {
    uint32_t content_height = bottom > top ? bottom - top : 0U;
    uint32_t draw_width;
    uint32_t draw_height;
    int64_t left;
    int64_t image_top;
    int64_t right;
    int64_t image_bottom;

    if (g_image_pixels == 0 || content_height == 0U ||
        g_zoom_percent == 0U) return;
    clamp_pan(g_window.width, content_height);
    draw_width = (uint32_t)(((uint64_t)g_image_width * g_zoom_percent) / 100U);
    draw_height = (uint32_t)(((uint64_t)g_image_height * g_zoom_percent) / 100U);
    if (draw_width == 0U || draw_height == 0U) return;
    left = ((int64_t)g_window.width - draw_width) / 2 + g_pan_x;
    image_top = (int64_t)top + ((int64_t)content_height - draw_height) / 2 + g_pan_y;
    right = left + draw_width;
    image_bottom = image_top + draw_height;

    for (int64_t y = image_top > (int64_t)top ? image_top : top;
         y < image_bottom && y < (int64_t)bottom; ++y) {
        uint32_t source_y = (uint32_t)(((uint64_t)(y - image_top) * 100U) /
                                       g_zoom_percent);
        if (source_y >= g_image_height) continue;
        for (int64_t x = left > 0 ? left : 0;
             x < right && x < (int64_t)g_window.width; ++x) {
            uint32_t source_x = (uint32_t)(((uint64_t)(x - left) * 100U) /
                                           g_zoom_percent);
            const uint8_t *source;
            uint32_t color;
            if (source_x >= g_image_width) continue;
            source = g_image_pixels + (uint64_t)source_y * g_image_stride +
                     (uint64_t)source_x * 4U;
            color = ((uint32_t)source[0] << 16) |
                    ((uint32_t)source[1] << 8) | source[2];
            /* 只提交预乘图片内容；透明底纹由合成器容器绘制，不烘焙到图片中。 */
            uint32_t alpha=source[3];
            uint32_t pixel=alpha<<24U;
            for (uint32_t shift=0;shift<24U;shift+=8U)
                pixel|=((((color>>shift)&255U)*alpha+127U)/255U)<<shift;
            g_window.pixels[(uint64_t)(uint32_t)y*g_window.width+(uint32_t)x]=pixel;
        }
    }
}

static void render(void) {
    /* 文本随控件树保留，容量覆盖完整路径与尺寸、快捷键提示。 */
    static char status[8192];
    uint32_t content_bottom = g_window.height > IMAGEVIEW_STATUS_HEIGHT ?
        g_window.height - IMAGEVIEW_STATUS_HEIGHT : g_window.height;

    if (g_window.pixels == 0 || g_window.height == 0U) return;
    clamp_pan(g_window.width,content_bottom);
    bool content_dirty=!g_content_valid || g_drawn_image!=g_image_pixels ||
        g_drawn_width!=g_window.width || g_drawn_height!=content_bottom ||
        g_drawn_zoom!=g_zoom_percent || g_drawn_pan_x!=g_pan_x || g_drawn_pan_y!=g_pan_y;
    if (content_dirty) {
        memset(g_window.pixels,0,(size_t)g_window.width*content_bottom*sizeof(uint32_t));
        draw_image(0,content_bottom);
    }
    (void)snprintf(status, sizeof(status),
                    "%ux%u  %u%%   +/- IMAGE  CTRL +/- FONT  Q CLOSE  %s",
                    g_image_width, g_image_height, g_zoom_percent, g_path);
    uint32_t length=(uint32_t)strlen(status);
    g_nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
        .background=IMAGEVIEW_BACKGROUND,.foreground=IMAGEVIEW_STATUS_TEXT,.font_size=ui_text_size()};
    g_nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_CHECKERBOARD|OS_CONTROL_CLIP_CHILDREN,.width=OS_CONTROL_FILL,.height=(int32_t)content_bottom,
        .background=IMAGEVIEW_CHECKER_LIGHT,.foreground=IMAGEVIEW_CHECKER_DARK};
    g_nodes[2]=(os_control_node_t){.identifier=3,.parent=1,.kind=OS_CONTROL_SURFACE,.surface=1,
        .flags=OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_POINTER_PASSTHROUGH,
        .width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL};
    g_nodes[3]=(os_control_node_t){.identifier=4,.parent=0,.kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_INHERIT_TEXT,.y=(int32_t)content_bottom,
        .width=OS_CONTROL_FILL,.height=(int32_t)(g_window.height-content_bottom),.background=OS_THEME_TOOLBAR};
    g_nodes[4]=(os_control_node_t){.identifier=5,.parent=3,.kind=OS_CONTROL_TEXT,
        .flags=OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_INHERIT_TEXT,
        .x=10,.y=4,.width=g_window.width>20U?(int32_t)g_window.width-20:0,
        .height=(int32_t)ui_text_size(),.text_length=length};
    /* 全窗口根包含标题栏，图片和状态栏仍使用客户端坐标。 */
    uint32_t caption=user_caption_height(),font=ui_text_size();
    for (uint32_t i=5U;i>0U;--i) {
        g_nodes[i]=g_nodes[i-1U];
        g_nodes[i].parent=i==1U?0U:g_nodes[i].parent+1U;
    }
    g_nodes[1].identifier=6U;
    g_nodes[1].y=(int32_t)caption;
    g_nodes[1].width=OS_CONTROL_FILL;g_nodes[1].height=(int32_t)g_window.height;
    g_nodes[0]=(os_control_node_t){.identifier=1U,.parent=OS_CONTROL_NO_PARENT,
        .kind=OS_CONTROL_CONTAINER,.flags=OS_CONTROL_WINDOW_FRAME|OS_CONTROL_CLIP_CHILDREN|
            OS_CONTROL_POINTER_PASSTHROUGH,.width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL,.font_size=font};
    uint32_t size=os_window_caption_button_width(OS_WINDOW_CAPTION_HEIGHT(caption));
    uint32_t width=2U*font+24U,gap=8U;
    bool fits=g_window.width>=3U*size+width+2U*gap;
    memcpy(status+length,"打开",6U);
    g_nodes[6]=(os_control_node_t){.identifier=7U,.parent=0U,.kind=OS_CONTROL_BUTTON,
        .flags=fits?0U:OS_CONTROL_HIDDEN,.x=fits?(int32_t)(g_window.width-3U*size-width-gap):0,
        .y=4,.width=(int32_t)width,.height=(int32_t)caption-8,.radius=6,.font_size=font,
        .foreground=OS_THEME_TEXT,.background=OS_THEME_FIELD,.text_offset=length,.text_length=6U};
    length+=6U;
    uint32_t count=7U;
    if (!user_caption_tree(g_nodes,&count,OS_CONTROL_TREE_MAX_NODES,status,&length,sizeof(status),
            (user_caption_layout_t){.title="图片查看器",.width=g_window.width,.height=caption,
                .font=font,.title_width=fits?g_window.width-3U*size-width-2U*gap:0U})) return;
    if (g_picker.active) {
        ui_file_picker_document_t document;
        if (!ui_file_picker_build(&g_picker,g_window.identifier,g_window.width,g_window.height,&document) ||
            !ui_modal_tree_attach(g_nodes,&count,OS_CONTROL_TREE_MAX_NODES,status,&length,sizeof(status),&document.request)) {
            g_tree.count=0;g_content_valid=false;return;
        }
    }
    os_window_control_tree_t tree={.hdr={sizeof(tree),OS_SYSCALL_ABI_VERSION},
        .identifier=g_window.identifier,.count=count,.nodes=(uintptr_t)g_nodes,
        .text=(uintptr_t)status,.text_size=length};
    if (__libc_native_syscall6(OS_SYS_WINDOW_CONTROL_TREE,(uintptr_t)&tree,0,0,0,0,0)!=0) {
        g_tree.count=0;g_content_valid=false;return;
    }
    g_tree=tree;
    if (!content_dirty && caption==g_caption_height) return;

    os_window_update_t update = {0};
    update.flags = OS_WINDOW_CAPTION_HEIGHT(user_caption_height());
    update.hdr.size = sizeof(update);
    update.hdr.version = OS_SYSCALL_ABI_VERSION;
    update.identifier = g_window.identifier;
    update.width = g_window.width;
    update.height = g_window.height;
    if (__libc_native_syscall6(OS_SYS_WINDOW_UPDATE,(uintptr_t)&update,0,0,0,0,0)==0) {
        g_content_valid=true;g_drawn_image=g_image_pixels;g_drawn_width=g_window.width;
        g_drawn_height=content_bottom;g_drawn_zoom=g_zoom_percent;
        g_drawn_pan_x=g_pan_x;g_drawn_pan_y=g_pan_y;g_caption_height=caption;
    } else g_content_valid=false;
}

static bool create_window(void) {
    os_display_info_t display = {0};
    os_window_create_t request = {0};
    uint32_t width;
    uint32_t height;

    display.hdr.size = sizeof(display);
    display.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (__libc_native_syscall6(OS_SYS_DISPLAY_GET_INFO, (uint64_t)(uintptr_t)&display,
                        0U, 0U, 0U, 0U, 0U) < 0 || display.width < 320U ||
        display.height < 240U) return false;
    width = display.width * 3U / 4U;
    height = display.height * 3U / 4U;
    if (width < IMAGEVIEW_MIN_WIDTH && display.width > IMAGEVIEW_MIN_WIDTH) {
        width = IMAGEVIEW_MIN_WIDTH;
    }
    if (height < IMAGEVIEW_MIN_HEIGHT && display.height > IMAGEVIEW_MIN_HEIGHT) {
        height = IMAGEVIEW_MIN_HEIGHT;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.x = (int32_t)((display.width - width) / 2U);
    request.y = (int32_t)((display.height - height) / 2U);
    request.width = width;
    request.height = height;
    request.flags = OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE |
                    OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_PREMULTIPLIED_ALPHA|OS_WINDOW_CAPTION_HEIGHT(user_caption_height());
    request.background = IMAGEVIEW_BACKGROUND;
    memcpy(request.title, "图片查看器", sizeof("图片查看器"));
    request.address = IMAGEVIEW_MAP_BASE;
    if (__libc_native_syscall6(OS_SYS_WINDOW_CREATE, (uint64_t)(uintptr_t)&request,
                        0U, 0U, 0U, 0U, 0U) != 0 ||
        request.window == OS_INVALID_HANDLE || request.address == 0U) {
        return false;
    }
    g_window.identifier = request.identifier;
    g_window.width = request.width;
    g_window.height = request.height;
    g_window.pixels = (uint32_t *)(uintptr_t)request.address;
    return true;
}

static bool key_matches(const os_input_event_t *input, uint32_t hid,
                        uint32_t ascii) {
    return input != 0 && (input->code == hid || input->code == ascii);
}

static bool handle_key(const os_window_event_t *event) {
    const os_input_event_t *input = event != 0 ? &event->input : 0;

    if (event == 0 || event->type != OS_WINDOW_EVENT_INPUT || input == 0 ||
        input->type != OS_INPUT_EVENT_KEY) return false;
    if (input->code == 0xE1U || input->code == 0xE5U) {
        g_shift = input->value != OS_INPUT_VALUE_RELEASE;
        return false;
    }
    if (input->code == 0xE0U || input->code == 0xE4U) {
        g_ctrl = input->value != OS_INPUT_VALUE_RELEASE;
        return false;
    }
    if (input->value == OS_INPUT_VALUE_RELEASE) return false;
    if ((g_ctrl && key_matches(input, 0x14U, 'Q')) ||
        key_matches(input, 0x29U, 0x1BU)) {
        g_running = false;
        return true;
    }
    if (input->code == 0x4FU) {
        g_pan_x -= 32;
    } else if (input->code == 0x50U) {
        g_pan_x += 32;
    } else if (input->code == 0x52U) {
        g_pan_y -= 32;
    } else if (input->code == 0x51U) {
        g_pan_y += 32;
    } else if (g_ctrl && input->code == 0x2EU) {
        (void)ui_text_adjust(1);
    } else if (g_ctrl && input->code == 0x2DU) {
        (void)ui_text_adjust(-1);
    } else if ((input->code == 0x2EU && g_shift) || input->code == '+') {
        change_zoom(25);
    } else if ((input->code == 0x2DU && !g_shift) || input->code == '-') {
        change_zoom(-25);
    } else if (key_matches(input, 0x27U, '0')) {
        reset_view();
    } else {
        return false;
    }
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

static int64_t imageview_picker_call(uint64_t number,uint64_t address) {
    return __libc_native_syscall6(number,address,0,0,0,0,0);
}

/* 解码成功才替换当前图片；失败留在选择框中，保留旧图片与路径。 */
static void imageview_open_selected(void) {
    char selected[IMAGEVIEW_PATH_CAPACITY],previous[IMAGEVIEW_PATH_CAPACITY];
    if (!ui_file_picker_result(&g_picker,selected,sizeof(selected))) return;
    memcpy(previous,g_path,sizeof(previous));copy_path(selected);
    if (load_image_file()) {
        ui_file_picker_close(&g_picker);g_content_valid=false;reset_view();
    } else {
        memcpy(g_path,previous,sizeof(g_path));
        g_picker.active=true;g_picker.accepted=false;
        ui_message_copy(g_picker.error,sizeof(g_picker.error),"无法打开图片或不支持此格式");
    }
    g_ctrl=g_shift=false;
}

static bool handle_event(const os_window_event_t *event) {
    const os_input_event_t *input;
    os_window_event_t translated;

    if (event == 0) return false;
    if (g_picker.active && (event->type==OS_WINDOW_EVENT_CLOSE_REQUEST ||
        event->type==OS_WINDOW_EVENT_INPUT || event->type==OS_WINDOW_EVENT_CONTROL)) {
        if (event->type==OS_WINDOW_EVENT_CONTROL &&
            !os_control_event_node(&g_tree,g_nodes,&event->control)) return false;
        bool raw=event->type==OS_WINDOW_EVENT_INPUT && event->input.type==OS_INPUT_EVENT_KEY;
        bool key=event->type==OS_WINDOW_EVENT_CONTROL && event->control.action==OS_CONTROL_KEY;
        uint32_t code=raw?event->input.code:event->control.code;
        enum ui_file_picker_event_result result=ui_file_picker_event(&g_picker,event,
            (raw || key)?key_to_ascii(code,g_picker.shift):0);
        if (result==UI_PICKER_SELECTED) imageview_open_selected();
        g_ctrl=g_shift=false;
        return result!=UI_PICKER_IDLE;
    }
    if (event->type == OS_WINDOW_EVENT_CLOSE_REQUEST) {
        g_running = false;
        return true;
    }
    if (event->type == OS_WINDOW_EVENT_RESIZE) {
        if (event->resize.width == 0U || event->resize.height == 0U ||
            (uint64_t)event->resize.width * event->resize.height *
                sizeof(uint32_t) > event->resize.buffer_size) return false;
        g_window.width = event->resize.width;
        g_window.height = event->resize.height;
        reset_view();
        return true;
    }
    if (event->type==OS_WINDOW_EVENT_INPUT && event->input.type==OS_INPUT_EVENT_KEY)
        return handle_key(event);
    if (event->type!=OS_WINDOW_EVENT_CONTROL ||
        !os_control_event_node(&g_tree,g_nodes,&event->control)) return false;
    if (event->control.target==7U) {
        if (event->control.action==OS_CONTROL_CLICK)
            (void)ui_file_picker_show(&g_picker,UI_PICKER_OPEN_FILE,g_path,imageview_picker_call);
        return true;
    }
    translated=*event;translated.type=OS_WINDOW_EVENT_INPUT;
    translated.input=(os_input_event_t){.code=event->control.code,.value=event->control.value};
    if (event->control.action==OS_CONTROL_KEY) translated.input.type=OS_INPUT_EVENT_KEY;
    else if (event->control.action==OS_CONTROL_WHEEL) translated.input.type=OS_INPUT_EVENT_RELATIVE;
    else return false;
    event=&translated;
    input = &event->input;
    if (input->type == OS_INPUT_EVENT_RELATIVE &&
        input->code == OS_INPUT_REL_WHEEL && input->value != 0) {
        if (g_ctrl) {
            (void)ui_text_adjust(input->value > 0 ? 1 : -1);
            return true;
        }
        change_zoom(input->value > 0 ? 25 : -25);
        return true;
    }
    return handle_key(event);
}

int main(int argc, char **argv) {
    copy_path(IMAGEVIEW_DEFAULT_PATH);
    if (argc > 1 && argv != 0 && argv[1] != 0 && argv[1][0] != '\0') {
        copy_path(argv[1]);
    }
    if (!ui_text_init(OS_TEXT_DEFAULT_SIZE)) {
        printf("IMAGEVIEW_FAIL stage=font\n");
        return 1;
    }
    if (!load_image_file()) {
        printf("IMAGEVIEW_FAIL stage=decode path=%s\n", g_path);
        imageview_cleanup();
        return 1;
    }
    if (!create_window()) {
        printf("IMAGEVIEW_FAIL stage=window\n");
        imageview_cleanup();
        return 1;
    }
    reset_view();
    render();
    while (g_running) {
        os_window_event_read_t request = {0};
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_window.identifier;
        request.timeout_ns = IMAGEVIEW_EVENT_WAIT;
        int64_t status = __libc_native_syscall6(
            OS_SYS_WINDOW_EVENT_READ, (uint64_t)(uintptr_t)&request,
            0U, 0U, 0U, 0U, 0U);
        if (status == 0) {
            if (handle_event(&request.event) && g_running) render();
        } else if (status != -11 && status != -110) {
            __asm__ volatile("pause");
        }
    }
    imageview_cleanup();
    return 0;
}
