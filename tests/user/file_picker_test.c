#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kernel/control_tree.h>
static int allocation_budget=-1;
static void *picker_realloc(void *memory,size_t size) {
    if(!allocation_budget)return 0;
    if(allocation_budget>0)--allocation_budget;
    return realloc(memory,size);
}
#define realloc picker_realloc
#include "../../user/runtime/file_picker.h"
#undef realloc

static uint32_t font=16,calls,total=100;
static bool fail_read,bad_name;
static char clipboard[UI_PICKER_PATH];static size_t clipboard_size;
uint32_t ui_text_size(void){return font;}
uint32_t ui_text_line_height(void){return font+4U;}
/* 枚举回调不允许出现打开、写入、删除等系统调用。 */
static int64_t enumerate(uint64_t number,uint64_t address) {
    if(number==OS_SYS_CLIPBOARD_SET || number==OS_SYS_CLIPBOARD_GET) {
        os_clipboard_text_t *r=(void *)(uintptr_t)address;
        if(number==OS_SYS_CLIPBOARD_SET) {
            assert(r->size<sizeof(clipboard));clipboard_size=r->size;
            memcpy(clipboard,(void *)(uintptr_t)r->data,clipboard_size);return 0;
        }
        size_t capacity=r->size;r->size=clipboard_size;
        if(!capacity)return 0;
        if(capacity<clipboard_size)return -28;
        memcpy((void *)(uintptr_t)r->data,clipboard,clipboard_size);return 0;
    }
    if(number==OS_SYS_FILE_STAT) {
        os_file_stat_t *stat=(void *)(uintptr_t)address;
        const char *path=(const void *)(uintptr_t)stat->path;
        if(!strcmp(path,"/") || !strcmp(path,"/folder")) {
            stat->info.type=OS_FILE_TYPE_DIRECTORY;return 0;
        }
        unsigned index=0;int length=0;
        if((sscanf(path,"/file-%u.txt%n",&index,&length)==1 ||
            sscanf(path,"/folder/file-%u.txt%n",&index,&length)==1) &&
            path[length]==0 && index>0 && index<total) {
            stat->info.type=OS_FILE_TYPE_REGULAR;return 0;
        }
        return -2;
    }
    assert(number==OS_SYS_FILE_ENUMERATE_BATCH);++calls;
    os_file_enumerate_batch_t *r=(void *)(uintptr_t)address;
    assert(r->hdr.size==sizeof(*r) && r->hdr.version==OS_SYSCALL_ABI_VERSION);
    assert(r->capacity==32 && r->path && r->entries);
    if(fail_read)return -5;
    if(strcmp((const char *)(uintptr_t)r->path,"/") &&
       strcmp((const char *)(uintptr_t)r->path,"/folder"))return -2;
    if(r->index>=total)return -2;
    r->count=total-r->index;if(r->count>r->capacity)r->count=r->capacity;
    os_file_info_t *entries=(void *)(uintptr_t)r->entries;
    for(uint32_t i=0;i<r->count;++i) {
        uint32_t index=r->index+i;
        entries[i]=(os_file_info_t){.type=index==0?OS_FILE_TYPE_DIRECTORY:OS_FILE_TYPE_REGULAR};
        if(!index)strcpy(entries[i].name,"folder");
        else snprintf(entries[i].name,sizeof(entries[i].name),"file-%03u.txt",index);
        if(bad_name)memset(entries[i].name,'a',sizeof(entries[i].name));
    }
    return 0;
}
static os_window_event_t click(const ui_file_picker_t *p,uint32_t target) {
    return (os_window_event_t){.type=OS_WINDOW_EVENT_CONTROL,
        .control={.action=OS_CONTROL_CLICK,.target=ui_file_picker_target(p,target)}};
}
static os_window_event_t key(uint32_t code) {
    return (os_window_event_t){.type=OS_WINDOW_EVENT_INPUT,
        .input={.type=OS_INPUT_EVENT_KEY,.code=code,.value=OS_INPUT_VALUE_PRESS}};
}
static void path_join_test(void) {
    char out[UI_PICKER_PATH],long_name[UI_PICKER_PATH];
    assert(ui_file_picker_join("/","file.txt",out) && !strcmp(out,"/file.txt"));
    assert(ui_file_picker_join("/folder","file.txt",out) && !strcmp(out,"/folder/file.txt"));
    assert(ui_file_picker_join("/folder","/elsewhere/file.txt",out) && !strcmp(out,"/elsewhere/file.txt"));
    assert(ui_file_picker_join("/folder","/",out) && !strcmp(out,"/"));
    memset(long_name,'x',sizeof(long_name));long_name[UI_PICKER_PATH-2U]=0;
    assert(ui_file_picker_join("/",long_name,out) && strlen(out)==UI_PICKER_PATH-1U);
    long_name[UI_PICKER_PATH-2U]='x';long_name[UI_PICKER_PATH-1U]=0;
    strcpy(out,"unchanged");assert(!ui_file_picker_join("/",long_name,out));
    assert(!strcmp(out,"unchanged"));
    assert(!ui_file_picker_join("/folder","",out) && !strcmp(out,"unchanged"));
}
int main(void) {
    path_join_test();
    ui_file_picker_t picker={0};ui_file_picker_document_t document;
    char result[UI_PICKER_PATH];
    assert(ui_file_picker_show(&picker,UI_PICKER_OPEN_FILE,"/notes.txt",enumerate));
    assert(picker.active && picker.count==100 && calls==5 && !strcmp(picker.directory,"/"));
    assert(!strcmp(picker.entries[0].name,"folder"));
    /* 枚举的 ENOENT 也表示列表结束，不能据此把不存在的目录当作空目录。 */
    os_file_info_t *initial_entries=picker.entries;
    uint32_t initial_generation=picker.generation,initial_count=picker.count;
    assert(!ui_file_picker_directory(&picker,"/missing"));
    assert(picker.entries==initial_entries && picker.count==initial_count &&
        picker.generation==initial_generation && !strcmp(picker.directory,"/"));
    strcpy(picker.name,"/file-002.txt");picker.select_all=false;picker.accepted=true;
    fail_read=true;
    os_window_event_t failed_navigation=click(&picker,16);
    assert(ui_file_picker_event(&picker,&failed_navigation,0)==UI_PICKER_IDLE);
    fail_read=false;
    assert(!strcmp(picker.name,"/file-002.txt") && !picker.select_all && !picker.accepted);
    assert(picker.entries==initial_entries && picker.generation==initial_generation && picker.ready);
    assert(!ui_file_picker_result(&picker,result,sizeof(result)));
    /* 滚动换代使旧行点击失效；新一代的相同行对应新的列表索引。 */
    os_window_event_t old_row=click(&picker,17),scroll=key(0x51);
    ui_file_picker_event(&picker,&scroll,0);
    assert(picker.first==1 && picker.generation!=initial_generation);
    ui_file_picker_event(&picker,&old_row,0);assert(!strcmp(picker.name,"/file-002.txt"));
    os_window_event_t current_row=click(&picker,17);
    strcpy(picker.name,"/file-003.txt");ui_file_picker_event(&picker,&current_row,0);
    assert(!strcmp(picker.name,"/file-002.txt"));
    ui_file_picker_scroll(&picker,false);assert(!picker.first);
    uint32_t boundary_generation=picker.generation;
    ui_file_picker_scroll(&picker,false);assert(picker.generation==boundary_generation);
    picker.generation=0xfffffU;ui_file_picker_scroll(&picker,true);
    assert(picker.generation==1 && picker.first==1);
    old_row=click(&picker,17);scroll=click(&picker,0);
    scroll.control.action=OS_CONTROL_WHEEL;scroll.control.value=1;
    ui_file_picker_event(&picker,&scroll,0);assert(!picker.first && picker.generation==2);
    strcpy(picker.name,"/file-003.txt");ui_file_picker_event(&picker,&old_row,0);
    assert(!strcmp(picker.name,"/file-003.txt"));
    assert(ui_file_picker_build(&picker,7,800,600,&document));
    unsigned selected_rows=0;
    for(unsigned i=0;i<document.request.count;++i)
        if(document.nodes[i].flags&OS_CONTROL_CHECKED) {
            assert(document.nodes[i].identifier==ui_file_picker_target(&picker,19));++selected_rows;
        }
    assert(selected_rows==1);
    os_window_event_t event=click(&picker,16);
    assert(ui_file_picker_event(&picker,&event,0)==UI_PICKER_CHANGED && !strcmp(picker.directory,"/folder"));
    uint32_t generation=picker.generation;
    ui_file_picker_event(&picker,&event,0);assert(picker.generation==generation);
    ui_file_picker_up(&picker);assert(!strcmp(picker.directory,"/"));
    event=click(&picker,17);ui_file_picker_event(&picker,&event,0);
    event=click(&picker,4);assert(ui_file_picker_event(&picker,&event,0)==UI_PICKER_SELECTED);
    assert(ui_file_picker_result(&picker,result,sizeof(result)));
    assert(!strcmp(result,"/file-001.txt"));
    picker.ctrl=picker.shift=picker.save=picker.directory_only=true;
    strcpy(picker.error,"old error");uint32_t closed_generation=picker.generation;
    ui_file_picker_close(&picker);assert(!strcmp(result,"/file-001.txt"));
    assert(picker.generation==closed_generation);
    assert(!picker.call && !picker.name[0] && !picker.directory[0] && !picker.error[0]);
    assert(!picker.ctrl && !picker.shift && !picker.select_all && !picker.save && !picker.directory_only);
    assert(ui_file_picker_show(&picker,UI_PICKER_SAVE_FILE,"/folder/new.txt",enumerate));
    assert(!strcmp(picker.name,"/folder/new.txt") && ui_file_picker_choose(&picker));
    assert(ui_file_picker_result(&picker,result,sizeof(result)) && !strcmp(result,"/folder/new.txt"));
    assert(!ui_file_picker_result(&picker,result,4) && !strcmp(result,"/folder/new.txt"));
    strcpy(picker.name,"../escape");assert(!ui_file_picker_choose(&picker));
    strcpy(picker.name,"中文😀.txt");picker.select_all=false;
    event=key(0x2a);ui_file_picker_event(&picker,&event,0);assert(!strcmp(picker.name,"中文😀.tx"));
    picker.select_all=true;event=key(0x04);ui_file_picker_event(&picker,&event,'x');
    assert(!strcmp(picker.name,"x"));
    event=click(&picker,2);event.control.code=0x2c;
    ui_file_picker_event(&picker,&event,0);assert(!strcmp(picker.name,"x "));
    event.control.code=0x28;assert(ui_file_picker_event(&picker,&event,0)==UI_PICKER_SELECTED);
    picker.active=true;strcpy(picker.name,"x");
    /* 错代的键盘、滚轮、点击不修改当前控件。 */
    event=(os_window_event_t){.type=OS_WINDOW_EVENT_CONTROL,.control={.action=OS_CONTROL_KEY,
        .target=ui_file_picker_target(&picker,2)-256,.code=0x04,.value=OS_INPUT_VALUE_PRESS}};
    ui_file_picker_event(&picker,&event,'y');assert(!strcmp(picker.name,"x"));
    /* 控制字符及 Ctrl+空格不能污染路径；实际编辑使旧确认失效。 */
    picker.accepted=true;ui_file_picker_insert(&picker,'\n');
    ui_file_picker_insert(&picker,127);assert(!strcmp(picker.name,"x") && picker.accepted);
    picker.ctrl=true;ui_file_picker_insert(&picker,' ');picker.ctrl=false;
    assert(!strcmp(picker.name,"x"));ui_file_picker_insert(&picker,'z');
    assert(!strcmp(picker.name,"xz") && !picker.accepted);
    /* 读取失败、损坏名称和超大目录都保留旧快照。 */
    os_file_info_t *previous=picker.entries;uint32_t count=picker.count;
    fail_read=true;assert(!ui_file_picker_directory(&picker,"/"));fail_read=false;
    assert(picker.entries==previous && picker.count==count && !strcmp(picker.directory,"/folder"));
    bad_name=true;assert(!ui_file_picker_directory(&picker,"/"));bad_name=false;
    assert(picker.entries==previous);total=4097;
    assert(!ui_file_picker_directory(&picker,"/") && picker.entries==previous);total=100;
    for(int budget=0;budget<3;++budget) {
        allocation_budget=budget;
        assert(!ui_file_picker_directory(&picker,"/") && picker.entries==previous && picker.count==count);
    }
    allocation_budget=-1;
    for(font=12;font<=96;font+=4)for(unsigned width=320;width<=1920;width+=320) {
        assert(ui_file_picker_build(&picker,7,width,1080,&document));
        assert(document.nodes[2].background==0 && (document.nodes[2].flags&OS_CONTROL_BACKGROUND_ALPHA));
        /* 返回与可编辑完整路径同排，底部不再创建第二个输入框。 */
        assert(document.nodes[5].y==document.nodes[6].y);
        assert(document.nodes[5].x+document.nodes[5].width<=document.nodes[6].x);
        unsigned path_fields=0;
        for(unsigned i=0;i<document.request.count;++i)
            path_fields+=document.nodes[i].identifier==ui_file_picker_target(&picker,2);
        assert(path_fields==1 && document.nodes[6].kind==OS_CONTROL_BUTTON);
        assert(control_tree_validate(document.nodes,document.request.count,document.request.text_size));
        control_tree_box_t boxes[OS_CONTROL_TREE_MAX_NODES];
        assert(control_tree_layout(document.nodes,document.request.count,document.request.text_size,
            (Rect){0,0,(int32_t)width,1080},boxes));
        /* 公共控件只能有调用方替换的底层占位，不能添加应用像素层。 */
        unsigned surfaces=0;
        for(unsigned i=0;i<document.request.count;++i)surfaces+=document.nodes[i].kind==OS_CONTROL_SURFACE;
        assert(surfaces==1);
    }
    font=16;assert(ui_file_picker_show(&picker,UI_PICKER_DIRECTORY,"/folder",enumerate));
    assert(ui_file_picker_show(&picker,UI_PICKER_DIRECTORY,picker.directory,enumerate));
    assert(!strcmp(picker.directory,"/folder"));
    assert(picker.count==1 && picker.entries[0].type==OS_FILE_TYPE_DIRECTORY);
    event=click(&picker,4);assert(ui_file_picker_event(&picker,&event,0)==UI_PICKER_SELECTED);
    assert(ui_file_picker_result(&picker,result,sizeof(result)) && !strcmp(result,"/folder"));
    ui_file_picker_close(&picker);
    assert(ui_file_picker_show(&picker,UI_PICKER_OPEN_FILE,"/notes.txt",enumerate));
    strcpy(picker.name,"missing.txt");assert(!ui_file_picker_choose(&picker));
    /* 聚焦取消按钮时回车不应被解释为提交文件。 */
    strcpy(picker.name,"file-001.txt");event=click(&picker,3);
    event.control.action=OS_CONTROL_KEY;event.control.code=0x28;event.control.value=OS_INPUT_VALUE_PRESS;
    assert(ui_file_picker_event(&picker,&event,0)==UI_PICKER_IDLE && picker.active);
    /* 非路径控件的文字、退格和全选不会偷偷修改路径编辑状态。 */
    for(uint32_t target=1;target<=17;++target) {
        if(target==2)continue;
        picker.select_all=false;
        event=click(&picker,target);event.control.action=OS_CONTROL_KEY;
        event.control.value=OS_INPUT_VALUE_PRESS;
        event.control.code=0x04;ui_file_picker_event(&picker,&event,'a');
        event.control.code=0x2a;ui_file_picker_event(&picker,&event,0);
        picker.ctrl=true;event.control.code=0x04;
        ui_file_picker_event(&picker,&event,'a');picker.ctrl=false;
        assert(!strcmp(picker.name,"file-001.txt") && !picker.select_all);
    }
    event=click(&picker,2);event.control.action=OS_CONTROL_KEY;
    event.control.value=OS_INPUT_VALUE_PRESS;event.control.code=0x04;
    ui_file_picker_event(&picker,&event,'a');assert(!strcmp(picker.name,"file-001.txta"));
    event.control.code=0x2a;ui_file_picker_event(&picker,&event,0);
    assert(!strcmp(picker.name,"file-001.txt"));
    event=key(0x29);assert(ui_file_picker_event(&picker,&event,0)==UI_PICKER_CANCELLED);
    assert(!picker.active && !picker.entries);
    assert(!ui_file_picker_result(&picker,result,sizeof(result)));
    assert(!ui_file_picker_show(&picker,(enum ui_file_picker_mode)-1,"/",enumerate));
    /* 换模式后读取失败不能保留旧会话的 ready、选择结果或文件列表。 */
    assert(ui_file_picker_show(&picker,UI_PICKER_SAVE_FILE,"/folder/new.txt",enumerate));
    assert(ui_file_picker_choose(&picker));fail_read=true;
    assert(!ui_file_picker_show(&picker,UI_PICKER_DIRECTORY,"/folder",enumerate));
    assert(picker.active && !picker.ready && !picker.accepted && !picker.entries && !picker.count);
    assert(!strcmp(picker.name,"/") && !strcmp(picker.directory,"/"));
    assert(!ui_file_picker_choose(&picker) && !ui_file_picker_result(&picker,result,sizeof(result)));
    fail_read=false;ui_file_picker_close(&picker);ui_file_picker_close(&picker);
    /* 初始目录不存在时回到根目录；存在的空目录仍应正常打开。 */
    assert(ui_file_picker_show(&picker,UI_PICKER_SAVE_FILE,"/missing/new.txt",enumerate));
    assert(!strcmp(picker.directory,"/") && !strcmp(picker.name,"/new.txt"));
    total=0;assert(ui_file_picker_directory(&picker,"/folder/"));
    assert(!picker.count && !strcmp(picker.directory,"/folder") && !strcmp(picker.name,"/folder/"));
    /* 导航参数可直接引用路径编辑框，提交快照后不能读到已改写的别名。 */
    strcpy(picker.name,"/folder/");assert(ui_file_picker_directory(&picker,picker.name));
    assert(!strcmp(picker.directory,"/folder") && !strcmp(picker.name,"/folder/"));
    ui_file_picker_close(&picker);
    /* 路径编辑复用系统文本剪贴板，粘贴不能注入控制字符。 */
    assert(ui_file_picker_show(&picker,UI_PICKER_OPEN_FILE,"/",enumerate));
    strcpy(picker.name,"/folder/photo.png");picker.select_all=true;picker.ctrl=true;
    event=key(0x06);ui_file_picker_event(&picker,&event,0);
    assert(clipboard_size==strlen(picker.name)&&!memcmp(clipboard,picker.name,clipboard_size));
    event=key(0x1b);ui_file_picker_event(&picker,&event,0);assert(!picker.name[0]);
    event=key(0x19);ui_file_picker_event(&picker,&event,0);assert(!strcmp(picker.name,"/folder/photo.png"));
    memcpy(clipboard,"bad\n",4);clipboard_size=4;picker.select_all=true;
    ui_file_picker_event(&picker,&event,0);assert(!strcmp(picker.name,"/folder/photo.png"));
    /* 只有可见状态变化才要求重绘：边界滚轮、复制、无效输入均不改变界面。 */
    event=key(0x06);assert(ui_file_picker_event(&picker,&event,0)==UI_PICKER_IDLE);
    picker.ctrl=false;picker.select_all=false;picker.first=0;
    event=click(&picker,2);event.control.action=OS_CONTROL_MOVE;
    assert(ui_file_picker_event(&picker,&event,0)==UI_PICKER_IDLE);
    event.control.action=OS_CONTROL_WHEEL;event.control.value=1;
    assert(ui_file_picker_event(&picker,&event,0)==UI_PICKER_IDLE);
    event=key(0x04);assert(ui_file_picker_event(&picker,&event,'a')==UI_PICKER_CHANGED);
    event.input.value=OS_INPUT_VALUE_RELEASE;
    assert(ui_file_picker_event(&picker,&event,'a')==UI_PICKER_IDLE);
    event=click(&picker,2);assert(ui_file_picker_event(&picker,&event,0)==UI_PICKER_CHANGED);
    assert(ui_file_picker_event(&picker,&event,0)==UI_PICKER_IDLE);
    strcpy(picker.name,"/missing");picker.error[0]=0;
    event=click(&picker,4);assert(ui_file_picker_event(&picker,&event,0)==UI_PICKER_CHANGED);
    assert(picker.error[0]&&ui_file_picker_event(&picker,&event,0)==UI_PICKER_IDLE);
    ui_file_picker_close(&picker);
    ui_file_picker_close(0);assert(!ui_file_picker_directory(0,"/"));
    assert(!ui_file_picker_directory(&picker,0));
    puts("File picker: common tree, open/save/directory results, pagination, stale events, UTF-8 editing, transactional failures and cancel PASS");
}
