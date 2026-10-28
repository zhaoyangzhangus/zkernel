#pragma once

#include "abi.h"
#include <stdbool.h>

/* 应用提交声明式数据，不提交绘制代码或内核指针。节点按深度优先顺序排列。
 * 结构参考 Views 的父子关系、LayoutManager 与独立内容宿主，而不是引入浏览器 DOM。 */
#define OS_CONTROL_TREE_MAX_NODES 4096U
#define OS_CONTROL_TREE_MAX_DEPTH 64U
#define OS_CONTROL_NO_PARENT UINT32_MAX
#define OS_CONTROL_FILL (-1)
#define OS_CONTROL_TREE_MAX_TEXT (64U * 1024U)

/* 控件字体采用固定单元：ASCII 半宽，其他字符全宽。应用命中和光标须使用同一度量。 */
static inline uint32_t os_control_text_cell_width(uint32_t code, uint32_t height) {
    return code<128U ? height/2U : height;
}

enum os_control_kind {
    OS_CONTROL_CONTAINER,
    OS_CONTROL_TEXT,
    OS_CONTROL_BUTTON,
    OS_CONTROL_SURFACE,
    OS_CONTROL_CHECKBOX,
    OS_CONTROL_RADIO,
    OS_CONTROL_SWITCH,
    OS_CONTROL_PROGRESS,
    OS_CONTROL_SLIDER,
    OS_CONTROL_SCROLLBAR,
    OS_CONTROL_LIST_ITEM,
    OS_CONTROL_TAB,
    OS_CONTROL_SEPARATOR,
    /* 窗口操作也是应用声明的控件，位置和顺序由应用布局决定。 */
    OS_CONTROL_WINDOW_BUTTON,
};

enum os_control_window_action {
    OS_CONTROL_WINDOW_MINIMIZE=1U,
    OS_CONTROL_WINDOW_MAXIMIZE,
    OS_CONTROL_WINDOW_CLOSE,
};

#define OS_CONTROL_VALUE_MAX 10000U
/* 所有控件文字默认可选；编辑器正文由应用管理跨节点选区。 */
#define OS_CONTROL_TEXT_APP_SELECTION 2U

/* 共用能力判断，避免绘制、焦点和输入对同一种控件采用不同规则。 */
static inline bool os_control_is_range(uint32_t kind) {
    return kind == OS_CONTROL_SLIDER || kind == OS_CONTROL_SCROLLBAR;
}
static inline bool os_control_is_choice(uint32_t kind) {
    return kind == OS_CONTROL_CHECKBOX || kind == OS_CONTROL_RADIO || kind == OS_CONTROL_SWITCH;
}
static inline bool os_control_is_interactive(uint32_t kind) {
    return kind == OS_CONTROL_WINDOW_BUTTON || kind == OS_CONTROL_BUTTON || os_control_is_choice(kind) || os_control_is_range(kind) ||
        kind == OS_CONTROL_LIST_ITEM || kind == OS_CONTROL_TAB;
}

enum os_control_layout {
    OS_CONTROL_ABSOLUTE,
    OS_CONTROL_ROW,
    OS_CONTROL_COLUMN,
};

enum os_control_flags {
    OS_CONTROL_HIDDEN = 1U << 0,
    OS_CONTROL_DISABLED = 1U << 1,
    OS_CONTROL_CLIP_CHILDREN = 1U << 2,
    OS_CONTROL_POINTER_PASSTHROUGH = 1U << 3,
    /* 背景按 ARGB 进行 source-over；默认背景仍是不透明的 XRGB。 */
    OS_CONTROL_BACKGROUND_ALPHA = 1U << 4,
    /* 根节点选择服务器管理交互控件焦点；未迁移的应用仍可自行处理键盘。 */
    OS_CONTROL_MANAGED_FOCUS = 1U << 5,
    /* 默认焦点只在新树没有可保留焦点时采用；跳过 Tab 不影响鼠标点击。 */
    OS_CONTROL_DEFAULT_FOCUS = 1U << 6,
    OS_CONTROL_SKIP_TAB = 1U << 7,
    /* 简单对话框可让左右方向键复用按钮焦点循环。 */
    OS_CONTROL_FOCUS_ARROWS = 1U << 8,
    /* 逐属性选择继承，未选择时使用本节点值，零值也可显式覆盖父样式。 */
    OS_CONTROL_INHERIT_FOREGROUND = 1U << 9,
    OS_CONTROL_INHERIT_FONT = 1U << 10,
    OS_CONTROL_INHERIT_BACKGROUND = 1U << 11,
    OS_CONTROL_INHERIT_BORDER_COLOR = 1U << 12,
    OS_CONTROL_INHERIT_BORDER_WIDTH = 1U << 13,
    OS_CONTROL_INHERIT_RADIUS = 1U << 14,
    OS_CONTROL_INHERIT_TEXT = OS_CONTROL_INHERIT_FOREGROUND | OS_CONTROL_INHERIT_FONT,
    OS_CONTROL_INHERIT_STYLE = OS_CONTROL_INHERIT_TEXT | OS_CONTROL_INHERIT_BACKGROUND |
        OS_CONTROL_INHERIT_BORDER_COLOR | OS_CONTROL_INHERIT_BORDER_WIDTH | OS_CONTROL_INHERIT_RADIUS,
    /* 行列容器对应简化的 Flex 布局；布局不继承，伸缩标记作用于直接子项。 */
    OS_CONTROL_FLEX_GROW = 1U << 15,
    OS_CONTROL_JUSTIFY_CENTER = 1U << 16,
    OS_CONTROL_JUSTIFY_END = 1U << 17,
    OS_CONTROL_JUSTIFY_BETWEEN = 1U << 18,
    OS_CONTROL_ALIGN_CENTER = 1U << 19,
    OS_CONTROL_ALIGN_END = 1U << 20,
    OS_CONTROL_ALIGN_STRETCH = 1U << 21,
    /* 选择控件的开关状态，或列表项/标签的选中状态；单选分组互斥由应用更新。 */
    OS_CONTROL_CHECKED = 1U << 22,
    /* 进度/范围/分隔线采用纵向；范围值向下增加，横向向右增加。 */
    OS_CONTROL_VERTICAL = 1U << 23,
    /* 根节点覆盖服务端标题栏与内容区；事件仍用客户端坐标，标题栏 y 为负。
     * 应用定义整个标题栏及窗口操作控件，未声明的按钮不会由服务端补画。
     * 根节点只提供布局，背景与窗口边框仍由统一主题绘制。 */
    OS_CONTROL_WINDOW_FRAME = 1U << 24,
    /* 按钮使用公共符号库，value 为 os_control_symbol；不再由应用复制图标光栅。 */
    OS_CONTROL_SYMBOL = 1U << 25,
    /* 暂停悬停视觉但保留输入、按压反馈，用于键盘导航中的菜单等控件。 */
    OS_CONTROL_NO_HOVER = 1U << 26,
    /* 容器透明预览底纹：16 像素方格，background/foreground 分别为两种颜色。 */
    OS_CONTROL_CHECKERBOARD = 1U << 27,
    /* 公共弹层只描述内容和操作；合成器决定面板主题、菜单行布局及边界约束。 */
    OS_CONTROL_MENU_PANEL = 1U << 28,
    OS_CONTROL_DIALOG_PANEL = 1U << 29,
    OS_CONTROL_DIALOG_ACTIONS = 1U << 30,
    OS_CONTROL_DIALOG_CLOSE = 1U << 31,
};
/* 容器组合语义：阻止整棵子树接收输入，但保持原有外观；不占用新的 ABI 位。 */
#define OS_CONTROL_INERT (OS_CONTROL_DISABLED|OS_CONTROL_POINTER_PASSTHROUGH)

/* 公共弹层的逻辑度量；内容换行与服务端布局共享约束，不共享像素。 */
static inline uint32_t os_control_popup_font(uint32_t font) {
    return !font ? 16U : font>96U ? 96U : font;
}
static inline uint32_t os_control_menu_height(uint32_t font,uint32_t rows) {
    font=os_control_popup_font(font);
    return (rows>OS_CONTROL_TREE_MAX_NODES ? OS_CONTROL_TREE_MAX_NODES : rows)*font*2U+
        2U*((font+2U)/4U);
}
static inline uint32_t os_control_dialog_width(uint32_t font) {
    return os_control_popup_font(font)*34U;
}

enum os_control_symbol {
    OS_CONTROL_SYMBOL_NONE,
    OS_CONTROL_SYMBOL_BACK,
    OS_CONTROL_SYMBOL_FORWARD,
    OS_CONTROL_SYMBOL_SEARCH,
    OS_CONTROL_SYMBOL_LIST,
    OS_CONTROL_SYMBOL_GRID,
    OS_CONTROL_SYMBOL_SORT_ASCENDING,
    OS_CONTROL_SYMBOL_SORT_DESCENDING,
    OS_CONTROL_SYMBOL_MENU,
    OS_CONTROL_SYMBOL_HOME,
    OS_CONTROL_SYMBOL_DRIVE,
    OS_CONTROL_SYMBOL_FOLDER,
    OS_CONTROL_SYMBOL_FILE,
    OS_CONTROL_SYMBOL_CLOSE,
};

enum os_control_action {
    OS_CONTROL_ENTER = 1U,
    OS_CONTROL_LEAVE,
    OS_CONTROL_MOVE,
    OS_CONTROL_DOWN,
    OS_CONTROL_UP,
    OS_CONTROL_CLICK,
    OS_CONTROL_KEY,
    OS_CONTROL_WHEEL,
    OS_CONTROL_FOCUS,
    OS_CONTROL_BLUR,
    /* 受控状态：应用收到建议值后重提交节点；服务器不修改已发布快照。
     * 选择控件为 0/1，滑块和滚动条为 0..OS_CONTROL_VALUE_MAX。
     * 范围事件是位置建议，可重复且不能将其视作增量；抬起时再次发送最终位置。 */
    OS_CONTROL_CHANGE,
};

/* 窗口事件的 pointer_x/y 仍使用客户端坐标，target 为控件稳定标识。
 * node_index/node_kind/tree_revision/user_data 描述产生事件时的不可变快照。
 * MOVE 是可合并的位置快照，移动距离应由 pointer_x/y 计算；code/value 保存最后一条输入。
 * 按键、按钮、滚轮、进入/离开与焦点事件均不可合并。 */
typedef struct os_control_event {
    uint64_t timestamp;
    uint32_t target;
    uint32_t code;
    int32_t value;
    uint16_t action;
    uint16_t flags;
    uint32_t node_index;
    uint32_t node_kind;
    uint64_t tree_revision;
    uint64_t user_data;
} os_control_event_t;

typedef struct os_control_node {
    uint32_t identifier;
    uint32_t parent;
    uint32_t kind;
    uint32_t flags;
    int32_t x, y;
    int32_t width, height;
    uint32_t layout;
    uint32_t padding;
    uint32_t gap;
    int32_t scroll_x, scroll_y;
    uint32_t background;
    uint32_t foreground;
    uint32_t border_color;
    uint32_t border_width;
    uint32_t radius;
    uint32_t font_size;
    /* 矢量图标尺寸独立于字体缓存；零表示跟随计算后的字号。 */
    uint32_t icon_size;
    uint32_t text_offset;
    uint32_t text_length;
    /* Surface 是内容宿主；标识只在所属窗口的资源表中解析，不直接解释为地址。 */
    union {
        uint32_t surface;
        /* 进度、滑块及滚动条的归一化值；符号按钮为 os_control_symbol。
         * 与 Surface 资源标识按 kind/flags 区分。 */
        uint32_t value;
    };
    /* 滚动条可见内容占比，归一化到 0..10000；零采用最小可拖动滑块。
     * 与内核及内置应用一并构建，不再由各应用计算私有滑块尺寸。 */
    uint32_t page_size;
    /* 应用自定义关联值，原样回传；内核绝不把它解释为可访问的指针。 */
    uint64_t user_data;
} os_control_node_t;

/* 绘制与指针换算共享滑块长度，避免实际命中范围与画面不一致。 */
static inline uint32_t os_control_range_thumb(const os_control_node_t *node,
    uint32_t length, uint32_t cross) {
    uint32_t size=node->kind==OS_CONTROL_SCROLLBAR ?
        (uint32_t)((uint64_t)length*node->page_size/OS_CONTROL_VALUE_MAX) : cross;
    if (size<12U) size=12U;
    return size>length ? length : size;
}

/* 零节点提交恢复旧 Surface 路径；非零提交整体替换，失败保留旧树。
 * 文档及视口完全相同时保持原版本和输入状态，不产生额外重绘。 */
typedef struct os_window_control_tree {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t count;
    uint64_t nodes;
    uint64_t text;
    uint32_t text_size;
    uint32_t reserved;
    /* 成功提交后由服务器写回，失败时不变；删除树也推进版本，防止标识复用。 */
    uint64_t revision;
} os_window_control_tree_t;

/* 直接索引为 O(1)，旧树事件不能在重排或复用标识后命中新对象。
 * request/nodes 必须是该次成功提交的本地快照，不可传入尚未提交的编辑副本。 */
static inline const os_control_node_t *os_control_event_node(const os_window_control_tree_t *request,
    const os_control_node_t *nodes, const os_control_event_t *event) {
    if (!request || !nodes || !event || !request->revision ||
        request->count > OS_CONTROL_TREE_MAX_NODES || event->tree_revision != request->revision ||
        event->node_index >= request->count || event->node_index >= OS_CONTROL_TREE_MAX_NODES) return 0;
    const os_control_node_t *node = nodes+event->node_index;
    return node->identifier == event->target && node->kind == event->node_kind &&
        node->user_data == event->user_data ? node : 0;
}
