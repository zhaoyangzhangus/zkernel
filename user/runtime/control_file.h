#pragma once

#include <uapi/control_tree.h>

/* 文件只描述界面；行为和应用指针通过加载后的命名绑定建立。 */
#define UI_FILE_MAX_BYTES (256U * 1024U)
enum { UI_RESOURCE_DIRECTORY, UI_RESOURCE_PROGRAM, UI_RESOURCE_ICON, UI_RESOURCE_FIELDS };
typedef struct ui_file_resource {
    uint32_t node_index;
    uint32_t offset[UI_RESOURCE_FIELDS], length[UI_RESOURCE_FIELDS];
} ui_file_resource_t;
typedef struct ui_file_document {
    os_window_control_tree_t request;
    os_control_node_t *nodes;
    /* 整份文档共用节点起始的一次分配，其他指针只是内部视图，不能单独释放。
     * 加载后节点顺序及标识符固定，样式和状态仍可修改。 */
    uint16_t *identifiers;
    char *text;
    ui_file_resource_t *resources;
    uint32_t resource_count;
    uint32_t wallpaper_offset,wallpaper_length;
    size_t wallpaper_source_offset,wallpaper_source_length;
} ui_file_document_t;

typedef struct ui_file_error {
    size_t offset;
    const char *message;
} ui_file_error_t;

uint32_t ui_file_identifier(const char *name);
os_control_node_t *ui_file_find(ui_file_document_t *document, const char *name);
/* 输出必须为空；失败释放候选文档，绝不更改当前已提交的界面。 */
bool ui_file_parse(const char *source, size_t size, ui_file_document_t *output, ui_file_error_t *error);
bool ui_file_load(const char *path, ui_file_document_t *output, ui_file_error_t *error);
void ui_file_destroy(ui_file_document_t *document);
/* 相对程序名和图标名只能在声明的绝对应用目录内解析。 */
bool ui_file_resource_path(const ui_file_document_t *document, const ui_file_resource_t *resource,
    uint32_t field, char *output, size_t capacity);

/* 根节点的壁纸资源使用绝对文件路径；替换只修改原属性，保留其余界面与注释。 */
bool ui_file_wallpaper_path(const ui_file_document_t *,char *,size_t);
bool ui_file_set_wallpaper(const char *,size_t,const char *,char **,size_t *);
