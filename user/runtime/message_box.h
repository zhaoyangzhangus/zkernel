#ifndef OS_MESSAGE_BOX_H
#define OS_MESSAGE_BOX_H

#include <uapi/window.h>
#include <uapi/window_theme.h>
#include <uapi/utf8.h>
#include "text.h"

/* 消息框只提交标题、正文与操作描述；公共模态层阻止底层输入但保留原色。 */
enum ui_message_buttons {
    OS_MESSAGE_OK, OS_MESSAGE_OK_CANCEL,
    OS_MESSAGE_YES_NO, OS_MESSAGE_SAVE_DISCARD_CANCEL, OS_MESSAGE_DELETE_CANCEL
};
enum ui_message_result {
    OS_MESSAGE_NONE, OS_MESSAGE_ACCEPT, OS_MESSAGE_CANCEL,
    OS_MESSAGE_YES, OS_MESSAGE_NO,
    OS_MESSAGE_SAVE, OS_MESSAGE_DISCARD
};
typedef struct ui_message_box {
    bool active;
    enum ui_message_buttons buttons;
    uint32_t generation;
    char title[64];
    char message[512];
} ui_message_box_t;

static inline void ui_message_copy(char *out, uint32_t capacity,
                                       const char *text) {
    if (!out || !capacity) return;
    uint32_t i = 0U;
    /* 按完整字符截断，损坏输入统一替换，不能留下半个中文或表情序列。 */
    while (text && *text && i+1U<capacity) {
        size_t available=0,used=0;
        while (available<4U && text[available]) ++available;
        uint32_t code=os_utf8_next(text,available,&used);
        char encoded[4];size_t bytes=os_utf8_encode(code,encoded);
        if (!used || bytes>capacity-i-1U) break;
        for (size_t j=0;j<bytes;++j) out[i++]=encoded[j];
        text+=used;
    }
    out[i] = '\0';
}
static inline uint32_t ui_message_count(enum ui_message_buttons buttons) {
    return buttons == OS_MESSAGE_OK ? 1U :
        buttons == OS_MESSAGE_SAVE_DISCARD_CANCEL ? 3U : 2U;
}
static inline void ui_message_show(ui_message_box_t *box,
    const char *title, const char *message, enum ui_message_buttons buttons) {
    if (!box) return;
    if ((uint32_t)buttons > OS_MESSAGE_DELETE_CANCEL) buttons = OS_MESSAGE_OK;
    box->buttons = buttons;
    box->generation = (box->generation + 1U) & 0x000fffffU;
    if (!box->generation) box->generation = 1U;
    ui_message_copy(box->title, sizeof(box->title), title);
    ui_message_copy(box->message, sizeof(box->message), message);
    box->active = true;
}
static inline const char *ui_message_label(enum ui_message_buttons b,
                                               uint32_t i) {
    if (b == OS_MESSAGE_SAVE_DISCARD_CANCEL)
        return i == 0U ? "保存" : i == 1U ? "不保存" : "取消";
    if (b == OS_MESSAGE_YES_NO) return i == 0U ? "是" : "否";
    if (b == OS_MESSAGE_DELETE_CANCEL) return i == 0U ? "删除" : "取消";
    return i == 0U ? "确定" : "取消";
}
static inline enum ui_message_result ui_message_choice(
    enum ui_message_buttons b, uint32_t i) {
    if (b == OS_MESSAGE_SAVE_DISCARD_CANCEL)
        return i == 0U ? OS_MESSAGE_SAVE : i == 1U ? OS_MESSAGE_DISCARD : OS_MESSAGE_CANCEL;
    if (b == OS_MESSAGE_YES_NO) return i == 0U ? OS_MESSAGE_YES : OS_MESSAGE_NO;
    return i == 0U ? OS_MESSAGE_ACCEPT : OS_MESSAGE_CANCEL;
}
#define OS_MESSAGE_NODE_CAPACITY 32U
#define OS_MESSAGE_TEXT_CAPACITY 1024U
typedef struct ui_message_document {
    os_window_control_tree_t request;
    os_control_node_t nodes[OS_MESSAGE_NODE_CAPACITY];
    char text[OS_MESSAGE_TEXT_CAPACITY];
} ui_message_document_t;

static inline uint32_t ui_message_target(const ui_message_box_t *box, uint32_t index) {
    /* 标题、面板等非交互节点也归属本会话，避免与宿主应用的低编号或文件卡片冲突。 */
    return 0x50000000U | ((box->generation & 0x000fffffU) << 8U) | index;
}

static inline bool ui_message_append(ui_message_document_t *document,
    os_control_node_t node, const char *text, uint32_t length) {
    if (document->request.count == OS_MESSAGE_NODE_CAPACITY ||
        length > OS_MESSAGE_TEXT_CAPACITY - document->request.text_size) return false;
    node.text_offset = document->request.text_size;
    node.text_length = length;
    for (uint32_t i=0U; i<length; ++i) document->text[document->request.text_size++]=text[i];
    document->nodes[document->request.count++]=node;
    return true;
}

/* 正文按公共文字单元拆成行描述；面板、按钮和关闭控件的几何全部由合成器决定。 */
static inline bool ui_message_build(const ui_message_box_t *box, uint32_t identifier,
    uint32_t width, uint32_t height, ui_message_document_t *document) {
    if (!box || !box->active || !document || !identifier || !width || !height ||
        width > INT32_MAX || height > INT32_MAX) return false;
    *document=(ui_message_document_t){0};
    uint32_t font=ui_text_size();
    if (!font || font>96U) return false;
    os_control_node_t root={.identifier=ui_message_target(box,0),.parent=OS_CONTROL_NO_PARENT,
        .kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_MANAGED_FOCUS|OS_CONTROL_FOCUS_ARROWS,
        .background=OS_THEME_SURFACE,.foreground=OS_THEME_TEXT,.font_size=font};
    if (!ui_message_append(document,root,0,0)) return false;
    os_control_node_t surface={.identifier=ui_message_target(box,5),.parent=0,.kind=OS_CONTROL_SURFACE,
        .flags=OS_CONTROL_DISABLED,.width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL,.surface=1};
    if (!ui_message_append(document,surface,0,0)) return false;
    os_control_node_t shade={.identifier=ui_message_target(box,6),.parent=0,.kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_INHERIT_TEXT,
        .width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL,.background=OS_THEME_SCRIM};
    if (!ui_message_append(document,shade,0,0)) return false;
    os_control_node_t panel={.identifier=ui_message_target(box,7),.parent=2,.kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_DIALOG_PANEL|OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_INHERIT_TEXT};
    if (!ui_message_append(document,panel,0,0)) return false;
    uint32_t n=0;while(n<sizeof(box->title) && box->title[n])++n;
    if (n==sizeof(box->title)) return false;
    os_control_node_t label={.identifier=ui_message_target(box,8),.parent=3,.kind=OS_CONTROL_TEXT,
        .flags=OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_INHERIT_TEXT};
    if (!ui_message_append(document,label,box->title,n)) return false;
    uint32_t content_width=os_control_dialog_width(font);
    if (content_width>width) content_width=width;
    uint32_t inset=font;
    if (inset>content_width/2U) inset=content_width/2U;
    if (inset>height/2U) inset=height/2U;
    content_width-=2U*inset;
    const char *text=box->message;
    size_t remaining=0;
    while (remaining<sizeof(box->message) && text[remaining]) ++remaining;
    if (remaining==sizeof(box->message)) return false;
    uint32_t button_count=ui_message_count(box->buttons);
    while (*text && content_width && document->request.count+button_count+2U<OS_MESSAGE_NODE_CAPACITY) {
        size_t count=0,word=0;uint32_t used=0;
        while(count<remaining && text[count]!='\n') {
            /* 长度只扫描一次，逐字符推进，避免每个字符重新扫描整段后缀。 */
            size_t bytes=0;
            uint32_t code=os_utf8_next(text+count,remaining-count,&bytes);
            if (!bytes) break;
            uint32_t cell=os_control_text_cell_width(code,font);
            if (used+cell>content_width && count) break;
            if(text[count]==' ')word=count;
            used+=cell;count+=bytes;
            if (used>=content_width) break;
        }
        if(text[count]&&text[count]!='\n'&&word)count=word;
        if(!count&&*text!='\n')break;
        label.identifier=ui_message_target(box,16U+document->request.count);
        if(!ui_message_append(document,label,text,(uint32_t)count))return false;
        text+=count;remaining-=count;
        if(remaining && *text=='\n'){++text;--remaining;}
        while(remaining && *text==' '){++text;--remaining;}
    }
    /* 只定义按钮条的约束，具体宽度、间隔和余数分配由合成器统一计算。 */
    uint32_t button_parent=document->request.count;
    os_control_node_t buttons={.identifier=ui_message_target(box,9),.parent=3,.kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_DIALOG_ACTIONS|OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_INHERIT_TEXT,
        .layout=OS_CONTROL_ROW};
    if(!ui_message_append(document,buttons,0,0))return false;
    for(uint32_t i=0;i<button_count;++i) {
        const char *caption=ui_message_label(box->buttons,i);
        n=0;while(caption[n])++n;
        os_control_node_t button={.identifier=ui_message_target(box,2U+i),.parent=button_parent,
            .kind=OS_CONTROL_BUTTON,.flags=OS_CONTROL_INHERIT_FONT|OS_CONTROL_FLEX_GROW |
                (i+1U==button_count?OS_CONTROL_DEFAULT_FOCUS:0U),
            .background=box->buttons==OS_MESSAGE_DELETE_CANCEL && !i ? OS_THEME_DANGER :
                i+1U==button_count?OS_THEME_ACCENT:OS_THEME_FIELD,
            .foreground=(box->buttons==OS_MESSAGE_DELETE_CANCEL && !i) || i+1U==button_count?
                OS_THEME_ON_ACCENT:OS_THEME_TEXT};
        if(!ui_message_append(document,button,caption,n))return false;
    }
    {
        os_control_node_t close={.identifier=ui_message_target(box,1),.parent=3,
            .kind=OS_CONTROL_BUTTON,.flags=OS_CONTROL_DIALOG_CLOSE|OS_CONTROL_SYMBOL|
                OS_CONTROL_SKIP_TAB|OS_CONTROL_INHERIT_FONT,
            .value=OS_CONTROL_SYMBOL_CLOSE,.foreground=OS_THEME_MUTED};
        if(!ui_message_append(document,close,0,0))return false;
    }
    document->request.hdr=(os_versioned_header_t){.size=sizeof(document->request),.version=1};
    document->request.identifier=identifier;
    document->request.nodes=(uintptr_t)document->nodes;
    document->request.text=(uintptr_t)document->text;
    return true;
}

static inline enum ui_message_result ui_message_event(
    ui_message_box_t *box, const os_window_event_t *event) {
    if(!box||!box->active||!event)return OS_MESSAGE_NONE;
    enum ui_message_result result=OS_MESSAGE_NONE;
    if(event->type==OS_WINDOW_EVENT_CLOSE_REQUEST)result=OS_MESSAGE_CANCEL;
    if(event->type==OS_WINDOW_EVENT_CONTROL&&event->control.action==OS_CONTROL_CLICK) {
        if(event->control.target==ui_message_target(box,1))result=OS_MESSAGE_CANCEL;
        for(uint32_t i=0;i<ui_message_count(box->buttons);++i)
            if(event->control.target==ui_message_target(box,2U+i))result=ui_message_choice(box->buttons,i);
    }
    /* 按钮切换和激活由服务器处理；仅保留本对话框自身的取消语义，拒绝旧代按键。 */
    if(event->type==OS_WINDOW_EVENT_CONTROL&&event->control.action==OS_CONTROL_KEY&&
        event->control.value==OS_INPUT_VALUE_PRESS&&event->control.code==0x29U&&
        (event->control.target&~0xffU)==ui_message_target(box,0)&&
        (event->control.target&0xffU)<=ui_message_count(box->buttons)+1U)
        result=OS_MESSAGE_CANCEL;
    if(result!=OS_MESSAGE_NONE)box->active=false;
    return result;
}
#endif
