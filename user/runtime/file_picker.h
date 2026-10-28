#pragma once
#include <stdlib.h>
#include <string.h>
#include <uapi/file.h>
#include <uapi/syscall.h>
#include "message_box.h"
#include "clipboard.h"

#define UI_PICKER_PATH 256U
#define UI_PICKER_ROWS 12U
#define UI_PICKER_LIMIT 4096U
typedef int64_t (*ui_picker_call_t)(uint64_t,uint64_t);
enum ui_file_picker_mode { UI_PICKER_OPEN_FILE,UI_PICKER_SAVE_FILE,UI_PICKER_DIRECTORY };
enum ui_file_picker_event_result { UI_PICKER_IDLE,UI_PICKER_CANCELLED,UI_PICKER_SELECTED,UI_PICKER_CHANGED };
typedef struct ui_file_picker {
    bool active,save,select_all,directory_only,ctrl,shift,accepted,ready;
    uint32_t generation,count,first;
    os_file_info_t *entries;
    char directory[UI_PICKER_PATH],name[UI_PICKER_PATH],error[96];
    ui_picker_call_t call;
} ui_file_picker_t;
typedef struct ui_file_picker_document {
    os_window_control_tree_t request;
    os_control_node_t nodes[64];
    char text[4096];
} ui_file_picker_document_t;

static inline uint32_t ui_file_picker_target(const ui_file_picker_t *p,uint32_t id) {
    /* 整棵子树共用会话前缀，装饰节点不能再占用应用自己的控件编号。 */
    return 0x40000000U|((p->generation&0xfffffU)<<8U)|id;
}
static inline int ui_file_picker_compare(const void *a,const void *b) {
    const os_file_info_t *x=a,*y=b;
    if ((x->type==OS_FILE_TYPE_DIRECTORY)!=(y->type==OS_FILE_TYPE_DIRECTORY))
        return x->type==OS_FILE_TYPE_DIRECTORY ? -1 : 1;
    return strcmp(x->name,y->name);
}
static inline void ui_file_picker_next_generation(ui_file_picker_t *p) {
    p->generation=(p->generation+1U)&0xfffffU;if(!p->generation)p->generation=1U;
}
/* 列表可见行换页后也换代，不能把排队中的旧行点击套用到新索引。 */
static inline void ui_file_picker_scroll(ui_file_picker_t *p,bool down) {
    uint32_t previous=p->first;
    if(down){if(p->first+1U<p->count)++p->first;}
    else if(p->first)--p->first;
    if(p->first!=previous)ui_file_picker_next_generation(p);
}
/* 组合路径时不修改编辑框，导航失败才能完整保留原选择和输入内容。 */
static inline bool ui_file_picker_join(const char *directory,const char *name,char out[UI_PICKER_PATH]) {
    size_t base=strlen(directory),length=strlen(name);
    bool absolute=name[0]=='/',slash=base>1U;
    if(!length || (absolute ? length : base+slash+length)>=UI_PICKER_PATH)return false;
    if(absolute)memcpy(out,name,length+1U);
    else {
        memcpy(out,directory,base);if(slash)out[base++]='/';
        memcpy(out+base,name,length+1U);
    }
    size_t used=strlen(out);while(used>1U && out[used-1U]=='/')out[--used]=0;
    return true;
}
/* 目录快照完整成功后再替换；失败不丢失当前目录，也不发布半份列表。 */
static inline bool ui_file_picker_directory(ui_file_picker_t *p,const char *directory) {
    os_file_info_t *entries=0;uint32_t count=0,capacity=0,index=0;
    if (!p || !p->call || !directory || directory[0]!='/' || strlen(directory)>=sizeof(p->directory)) return false;
    char path[UI_PICKER_PATH];memcpy(path,directory,strlen(directory)+1U);
    size_t length=strlen(path);while(length>1U && path[length-1U]=='/')path[--length]=0;
    p->accepted=false;
    /* 先确认目录存在，随后枚举的 ENOENT 才能安全解释为到达列表尾部。 */
    os_file_stat_t stat={.hdr={sizeof(stat),OS_SYSCALL_ABI_VERSION},.path=(uintptr_t)path};
    if(p->call(OS_SYS_FILE_STAT,(uintptr_t)&stat) || stat.info.type!=OS_FILE_TYPE_DIRECTORY)goto failed;
    for (;;) {
        os_file_info_t batch[OS_FILE_ENUMERATE_BATCH_MAX];
        os_file_enumerate_batch_t r={.hdr={sizeof(r),OS_SYSCALL_ABI_VERSION},
            .path=(uintptr_t)path,.entries=(uintptr_t)batch,
            .index=index,.capacity=OS_FILE_ENUMERATE_BATCH_MAX};
        int64_t status=p->call(OS_SYS_FILE_ENUMERATE_BATCH,(uintptr_t)&r);
        if(status==-2)break;
        if(status || !r.count || r.count>r.capacity || index>UI_PICKER_LIMIT-r.count)goto failed;
        index+=r.count;
        for(uint32_t i=0;i<r.count;++i) {
            if(!memchr(batch[i].name,0,sizeof(batch[i].name)))goto failed;
            if(!strcmp(batch[i].name,".") || !strcmp(batch[i].name,".."))continue;
            if(!batch[i].name[0] || strchr(batch[i].name,'/'))goto failed;
            if(batch[i].type!=OS_FILE_TYPE_DIRECTORY && batch[i].type!=OS_FILE_TYPE_REGULAR)continue;
            if(p->directory_only && batch[i].type!=OS_FILE_TYPE_DIRECTORY)continue;
            if(count==capacity) {
                uint32_t next=capacity ? capacity*2U : 32U;
                os_file_info_t *grown=realloc(entries,(size_t)next*sizeof(*entries));
                if(!grown)goto failed;
                entries=grown;capacity=next;
            }
            entries[count++]=batch[i];
        }
    }
    if(count>1U)qsort(entries,count,sizeof(*entries),ui_file_picker_compare);
    free(p->entries);p->entries=entries;p->count=count;p->first=0;
    p->ready=true;p->accepted=false;
    /* 目录切换也换代，旧列表排队的点击不能命中新目录中的同一行。 */
    ui_file_picker_next_generation(p);
    memcpy(p->directory,path,length+1U);p->error[0]=0;
    ui_message_copy(p->name,sizeof(p->name),p->directory);
    if(length>1U && p->name[length-1U]!='/' && length+1U<sizeof(p->name)) {
        p->name[length++]='/';p->name[length]=0;
    }
    return true;
failed:
    free(entries);ui_message_copy(p->error,sizeof(p->error),"目录读取失败或项目过多");return false;
}
static inline void ui_file_picker_close(ui_file_picker_t *p) {
    if(!p)return;
    /* 关闭即释放整个会话，仅保留代号，防止重开后接收旧事件。 */
    uint32_t generation=p->generation;
    free(p->entries);*p=(ui_file_picker_t){.generation=generation};
}
static inline bool ui_file_picker_show(ui_file_picker_t *p,enum ui_file_picker_mode mode,const char *path,ui_picker_call_t call) {
    char directory[UI_PICKER_PATH]="/",name[UI_PICKER_PATH]="";
    if(!p || !call || (uint32_t)mode>UI_PICKER_DIRECTORY)return false;
    /* 初始路径可能指向本控件的目录缓冲，必须先取副本再清理旧会话。 */
    if(path && path[0]=='/' && strlen(path)<sizeof(directory)) {
        const char *slash=strrchr(path,'/');size_t length=(size_t)(slash-path);
        if(length){memcpy(directory,path,length);directory[length]=0;}
        if(mode==UI_PICKER_DIRECTORY)memcpy(directory,path,strlen(path)+1U);
        else if(mode==UI_PICKER_SAVE_FILE)ui_message_copy(name,sizeof(name),slash+1);
    }
    /* 同次浏览失败保留快照，但重新打开是新会话，不能继承旧模式的目录列表。 */
    ui_file_picker_close(p);
    *p=(ui_file_picker_t){.generation=p->generation,.active=true,.select_all=true,
        .call=call,.save=mode==UI_PICKER_SAVE_FILE,.directory_only=mode==UI_PICKER_DIRECTORY,
        .directory="/",.name="/"};
    ui_file_picker_next_generation(p);
    bool loaded=ui_file_picker_directory(p,directory);
    if(!loaded && strcmp(directory,"/"))loaded=ui_file_picker_directory(p,"/");
    if(loaded && p->save && name[0]) {
        size_t used=strlen(p->name),extra=strlen(name);
        if(used+extra<sizeof(p->name))memcpy(p->name+used,name,extra+1U);
    }
    return loaded;
}
static inline bool ui_file_picker_path(ui_file_picker_t *p,char out[UI_PICKER_PATH]) {
    if(!ui_file_picker_join(p->directory,p->name,out)) {
        ui_message_copy(p->error,sizeof(p->error),"请输入有效完整路径，且路径不能过长");return false;
    }
    return true;
}
static inline void ui_file_picker_up(ui_file_picker_t *p) {
    char path[UI_PICKER_PATH];memcpy(path,p->directory,strlen(p->directory)+1U);
    char *slash=strrchr(path,'/');if(slash==path)path[1]=0;else if(slash)*slash=0;
    (void)ui_file_picker_directory(p,path);
}
/* 返回 1 表示选择文件，0 表示继续浏览；目录永远不能作为普通文件提交。 */
static inline bool ui_file_picker_choose(ui_file_picker_t *p) {
    p->accepted=false;
    if(!p->ready)return false;
    char path[UI_PICKER_PATH];if(!ui_file_picker_path(p,path))return false;
    os_file_stat_t stat={.hdr={sizeof(stat),OS_SYSCALL_ABI_VERSION},.path=(uintptr_t)path};
    int64_t status=p->call(OS_SYS_FILE_STAT,(uintptr_t)&stat);
    if(!status && stat.info.type==OS_FILE_TYPE_DIRECTORY) {
        if(!p->directory_only){(void)ui_file_picker_directory(p,path);return false;}
    } else if(p->directory_only || (status && !(p->save && status==-2)) ||
        (!status && stat.info.type!=OS_FILE_TYPE_REGULAR)) {
        ui_message_copy(p->error,sizeof(p->error),"所选路径不存在或无法访问");return false;
    } else if(status==-2) {
        char parent[UI_PICKER_PATH];memcpy(parent,path,strlen(path)+1U);
        char *slash=strrchr(parent,'/');
        if(!slash || !slash[1])return false;
        if(slash==parent)parent[1]=0;else *slash=0;
        stat.path=(uintptr_t)parent;
        if(p->call(OS_SYS_FILE_STAT,(uintptr_t)&stat) || stat.info.type!=OS_FILE_TYPE_DIRECTORY) {
            ui_message_copy(p->error,sizeof(p->error),"保存目录不存在或无法访问");return false;
        }
    }
    memcpy(p->name,path,strlen(path)+1U);return p->accepted=true;
}
/* 确认后仅复制完整路径；取消或容量不足返回 false，不改输出，也不读写所选文件。
 * 路径归调用方所有，关闭控件后仍有效；选择目录模式同样返回该目录的完整路径。 */
static inline bool ui_file_picker_result(ui_file_picker_t *p,char *path,size_t capacity) {
    if(!p || !p->accepted || !path || !capacity)return false;
    char selected[UI_PICKER_PATH];
    if(!ui_file_picker_path(p,selected))return false;
    size_t length=strlen(selected);
    if(length>=capacity)return false;
    memcpy(path,selected,length+1U);return true;
}
/* 普通按键与焦点系统转发的空格共用路径编辑，拒绝控制字符。 */
static inline void ui_file_picker_insert(ui_file_picker_t *p,char character) {
    if(p->ctrl || (unsigned char)character<32U || (unsigned char)character==127U)return;
    size_t n=p->select_all?0U:strlen(p->name);
    if(n+1U>=sizeof(p->name))return;
    p->name[n]=character;p->name[n+1U]=0;
    p->select_all=false;p->accepted=false;p->error[0]=0;
}
/* 控件自身消费导航、取消和文件名编辑；应用只接收选择结果，不重写命中规则。 */
static inline enum ui_file_picker_event_result ui_file_picker_apply_event(ui_file_picker_t *p,
    const os_window_event_t *event,char character) {
    if(event->type==OS_WINDOW_EVENT_CLOSE_REQUEST) {
        ui_file_picker_close(p);return UI_PICKER_CANCELLED;
    }
    bool key=event->type==OS_WINDOW_EVENT_CONTROL && event->control.action==OS_CONTROL_KEY;
    bool raw=event->type==OS_WINDOW_EVENT_INPUT && event->input.type==OS_INPUT_EVENT_KEY;
    uint32_t code=raw?event->input.code:event->control.code;
    int32_t value=raw?event->input.value:event->control.value;
    if(key || raw) {
        if(code==0xe0U || code==0xe4U){p->ctrl=value!=OS_INPUT_VALUE_RELEASE;return UI_PICKER_IDLE;}
        if(code==0xe1U || code==0xe5U){p->shift=value!=OS_INPUT_VALUE_RELEASE;return UI_PICKER_IDLE;}
        if(value==OS_INPUT_VALUE_RELEASE)return UI_PICKER_IDLE;
        if(code==0x29U){ui_file_picker_close(p);return UI_PICKER_CANCELLED;}
        if(code==0x28U || code==0x58U) {
            /* 普通按钮的回车由公共焦点系统生成点击，不能抢先提交文件。 */
            if(key && event->control.target!=ui_file_picker_target(p,2))return UI_PICKER_IDLE;
            if(ui_file_picker_choose(p)){p->active=false;return UI_PICKER_SELECTED;}
        } else if(code==0x4bU || code==0x52U)ui_file_picker_scroll(p,false);
        else if(code==0x4eU || code==0x51U)ui_file_picker_scroll(p,true);
        /* 语义按键携带真实焦点；按钮收到的文字和退格不能修改路径。 */
        else if(key && event->control.target!=ui_file_picker_target(p,2))return UI_PICKER_IDLE;
        else if(p->ctrl && code==0x04U)p->select_all=true;
        else if(p->ctrl && (code==0x06U || code==0x1bU)) {
            if(p->select_all && ui_clipboard_copy(p->call,p->name,strlen(p->name)) && code==0x1bU) {
                p->name[0]=0;p->select_all=false;p->accepted=false;
            }
        } else if(p->ctrl && code==0x19U) {
            size_t used=p->select_all?0U:strlen(p->name),length=0;
            char *text=ui_clipboard_paste(p->call,sizeof(p->name)-1U-used,&length);
            if(text) {
                bool valid=true;
                for(size_t i=0;i<length;++i)if((unsigned char)text[i]<32U || text[i]==127)valid=false;
                if(valid){memcpy(p->name+used,text,length+1U);p->select_all=false;p->accepted=false;p->error[0]=0;}
                free(text);
            }
        }
        else if(code==0x2aU) {
            size_t n=strlen(p->name);
            p->name[p->select_all?0U:os_utf8_previous(p->name,n,n)]=0;p->select_all=false;
            p->accepted=false;p->error[0]=0;
        } else ui_file_picker_insert(p,character);
        return UI_PICKER_IDLE;
    }
    if(event->type!=OS_WINDOW_EVENT_CONTROL)return UI_PICKER_IDLE;
    if(event->control.action==OS_CONTROL_WHEEL) {
        if(event->control.value)ui_file_picker_scroll(p,event->control.value<0);
        return UI_PICKER_IDLE;
    }
    if(event->control.action!=OS_CONTROL_CLICK)return UI_PICKER_IDLE;
    uint32_t target=event->control.target;
    if(target==ui_file_picker_target(p,1))ui_file_picker_up(p);
    else if(target==ui_file_picker_target(p,2)) {
        /* 公共按钮焦点把回车、空格转成点击，保留文件名输入语义。 */
        if(event->control.code==0x28U || event->control.code==0x58U) {
            if(ui_file_picker_choose(p)){p->active=false;return UI_PICKER_SELECTED;}
        } else if(event->control.code==0x2cU)ui_file_picker_insert(p,' ');
        else p->select_all=true;
    }
    else if(target==ui_file_picker_target(p,3)) {
        ui_file_picker_close(p);return UI_PICKER_CANCELLED;
    } else if(target==ui_file_picker_target(p,4)) {
        if(ui_file_picker_choose(p)){p->active=false;return UI_PICKER_SELECTED;}
    } else for(uint32_t slot=0;slot<UI_PICKER_ROWS && p->first+slot<p->count;++slot) {
        if(target!=ui_file_picker_target(p,16U+slot))continue;
        const os_file_info_t *entry=p->entries+p->first+slot;
        char path[UI_PICKER_PATH];
        p->accepted=false;
        if(!ui_file_picker_join(p->directory,entry->name,path)) {
            ui_message_copy(p->error,sizeof(p->error),"所选路径过长");break;
        }
        if(entry->type==OS_FILE_TYPE_DIRECTORY) {
            if(ui_file_picker_directory(p,path))p->select_all=true;
        } else {
            memcpy(p->name,path,strlen(path)+1U);p->select_all=true;p->error[0]=0;
        }
        break;
    }
    return UI_PICKER_IDLE;
}
/* 应用只在可见状态变化时重建界面；修饰键、松开及复制仍更新状态但不触发重绘。 */
static inline enum ui_file_picker_event_result ui_file_picker_event(ui_file_picker_t *p,
    const os_window_event_t *event,char character) {
    if(!p || !p->active || !event)return UI_PICKER_IDLE;
    if(event->type==OS_WINDOW_EVENT_CONTROL &&
        (event->control.target&~255U)!=ui_file_picker_target(p,0))return UI_PICKER_IDLE;
    bool key=event->type==OS_WINDOW_EVENT_CONTROL && event->control.action==OS_CONTROL_KEY;
    bool raw=event->type==OS_WINDOW_EVENT_INPUT && event->input.type==OS_INPUT_EVENT_KEY;
    uint32_t code=raw?event->input.code:event->control.code;
    int32_t value=raw?event->input.value:event->control.value;
    if((key || raw) && (value==OS_INPUT_VALUE_RELEASE || code==0xe0U || code==0xe4U ||
        code==0xe1U || code==0xe5U))return ui_file_picker_apply_event(p,event,character);
    if(event->type!=OS_WINDOW_EVENT_CLOSE_REQUEST && !key && !raw &&
        !(event->type==OS_WINDOW_EVENT_CONTROL &&
            (event->control.action==OS_CONTROL_CLICK || event->control.action==OS_CONTROL_WHEEL)))return UI_PICKER_IDLE;
    ui_file_picker_t previous=*p;
    enum ui_file_picker_event_result result=ui_file_picker_apply_event(p,event,character);
    if(result!=UI_PICKER_IDLE)return result;
    if(previous.active!=p->active || previous.ready!=p->ready || previous.count!=p->count ||
        previous.generation!=p->generation || previous.first!=p->first || previous.select_all!=p->select_all ||
        strcmp(previous.directory,p->directory) || strcmp(previous.name,p->name) || strcmp(previous.error,p->error))
        return UI_PICKER_CHANGED;
    return UI_PICKER_IDLE;
}
static inline bool ui_file_picker_append(ui_file_picker_document_t *d,os_control_node_t node,const char *text) {
    size_t n=text?strlen(text):0;
    if(d->request.count>=64U || n>sizeof(d->text)-d->request.text_size)return false;
    node.text_offset=d->request.text_size;node.text_length=(uint32_t)n;
    if(n)memcpy(d->text+d->request.text_size,text,n);
    d->request.text_size+=(uint32_t)n;d->nodes[d->request.count++]=node;return true;
}
/* 所有面板、控件和图标只提交描述，绘制和指针命中仍由合成器完成。 */
static inline bool ui_file_picker_build(const ui_file_picker_t *p,uint32_t identifier,
    uint32_t width,uint32_t height,ui_file_picker_document_t *d) {
    if(!p || !d || !p->active || !identifier || !width || !height ||
        width>INT32_MAX || height>INT32_MAX)return false;
    *d=(ui_file_picker_document_t){0};uint32_t font=ui_text_size(),pad=font;
    if(!font || font>96U)return false;
    uint32_t w=font*42U,h=font*28U;
    if(w>width)w=width;
    if(h>height)h=height;
    if(pad>w/4U)pad=w/4U;
    if(pad>h/12U)pad=h/12U;
    uint32_t row=font*2U,inner=w-2U*pad;
    uint32_t bottom=h>row+pad ? h-row-pad : 0;
    uint32_t error_y=bottom>row ? bottom-row : 0;
    uint32_t top=pad+row*2U;
    uint32_t visible=error_y>top ? (error_y-top)/row : 0;
    if(visible>UI_PICKER_ROWS)visible=UI_PICKER_ROWS;
    os_control_node_t node={.identifier=ui_file_picker_target(p,0),.parent=OS_CONTROL_NO_PARENT,
        .kind=OS_CONTROL_CONTAINER,.flags=OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_MANAGED_FOCUS|OS_CONTROL_FOCUS_ARROWS,
        .font_size=font,.foreground=OS_THEME_TEXT,.background=OS_THEME_SURFACE};
    if(!ui_file_picker_append(d,node,0))return false;
    node=(os_control_node_t){.identifier=ui_file_picker_target(p,5),.parent=0,.kind=OS_CONTROL_SURFACE,.flags=OS_CONTROL_DISABLED,
        .width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL,.surface=1};
    if(!ui_file_picker_append(d,node,0))return false;
    node=(os_control_node_t){.identifier=ui_file_picker_target(p,6),.parent=0,.kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_CLIP_CHILDREN,.width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL,
        .background=OS_THEME_SCRIM};
    if(!ui_file_picker_append(d,node,0))return false;
    node=(os_control_node_t){.identifier=ui_file_picker_target(p,7),.parent=2,.kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_DIALOG_PANEL|OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_INHERIT_TEXT,.width=(int32_t)w,.height=(int32_t)h};
    if(!ui_file_picker_append(d,node,0))return false;
    node=(os_control_node_t){.identifier=ui_file_picker_target(p,8),.parent=3,.kind=OS_CONTROL_TEXT,.x=(int32_t)pad,.y=(int32_t)pad,
        .width=(int32_t)inner,.height=(int32_t)row,.flags=OS_CONTROL_INHERIT_TEXT|OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_BACKGROUND_ALPHA};
    if(!ui_file_picker_append(d,node,p->directory_only?"选择目录":p->save?"保存文件":"打开文件"))return false;
    node=(os_control_node_t){.identifier=ui_file_picker_target(p,1),.parent=3,.kind=OS_CONTROL_BUTTON,
        .x=(int32_t)pad,.y=(int32_t)(pad+row),.width=(int32_t)row,.height=(int32_t)row,
        .flags=OS_CONTROL_SYMBOL|OS_CONTROL_INHERIT_TEXT,.value=OS_CONTROL_SYMBOL_BACK,.background=OS_THEME_FIELD,.radius=pad/2U};
    if(!ui_file_picker_append(d,node,0))return false;
    node=(os_control_node_t){.identifier=ui_file_picker_target(p,2),.parent=3,.kind=OS_CONTROL_BUTTON,
        .x=(int32_t)(pad+row+pad/2U),.y=(int32_t)(pad+row),.width=inner>row+pad/2U?(int32_t)(inner-row-pad/2U):0,
        .height=(int32_t)row,.flags=OS_CONTROL_INHERIT_TEXT|OS_CONTROL_DEFAULT_FOCUS,
        .background=p->select_all?OS_THEME_SELECTED:OS_THEME_FIELD,
        .border_width=1,.border_color=OS_THEME_BORDER,.radius=font/4U};
    if(!ui_file_picker_append(d,node,p->name))return false;
    char selected_path[UI_PICKER_PATH];
    bool has_selection=ui_file_picker_join(p->directory,p->name,selected_path);
    for(uint32_t slot=0;slot<visible && p->first+slot<p->count;++slot) {
        uint32_t index=p->first+slot,parent=d->request.count;
        const os_file_info_t *entry=p->entries+index;
        char entry_path[UI_PICKER_PATH];
        bool selected=has_selection && ui_file_picker_join(p->directory,entry->name,entry_path) &&
            !strcmp(selected_path,entry_path);
        node=(os_control_node_t){.identifier=ui_file_picker_target(p,16U+slot),.parent=3,.kind=OS_CONTROL_LIST_ITEM,
            .x=(int32_t)pad,.y=(int32_t)(top+slot*row),.width=(int32_t)inner,.height=(int32_t)row,
            .flags=OS_CONTROL_INHERIT_TEXT|OS_CONTROL_CLIP_CHILDREN|
                (selected?OS_CONTROL_CHECKED:0U),.background=OS_THEME_SURFACE,.radius=font/4U,.user_data=index};
        if(!ui_file_picker_append(d,node,0))return false;
        node=(os_control_node_t){.identifier=ui_file_picker_target(p,64U+slot),.parent=parent,.kind=OS_CONTROL_TEXT,
            .x=(int32_t)(font*2U),.y=(int32_t)(font/2U),
            .width=inner>font*2U?(int32_t)(inner-font*2U):0,.height=(int32_t)font,
            .flags=OS_CONTROL_INHERIT_TEXT|OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_BACKGROUND_ALPHA};
        if(!ui_file_picker_append(d,node,entry->name))return false;
        node=(os_control_node_t){.identifier=ui_file_picker_target(p,128U+slot),.parent=parent,.kind=OS_CONTROL_BUTTON,
            .x=(int32_t)(font/2U),.y=(int32_t)(font/2U),.width=(int32_t)font,.height=(int32_t)font,
            .flags=OS_CONTROL_SYMBOL|OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_INHERIT_TEXT,
            .value=entry->type==OS_FILE_TYPE_DIRECTORY?OS_CONTROL_SYMBOL_FOLDER:OS_CONTROL_SYMBOL_FILE};
        if(!ui_file_picker_append(d,node,0))return false;
    }
    node=(os_control_node_t){.identifier=ui_file_picker_target(p,9),.parent=3,.kind=OS_CONTROL_TEXT,.x=(int32_t)pad,.y=(int32_t)error_y,
        .width=(int32_t)inner,.height=(int32_t)row,.flags=OS_CONTROL_INHERIT_FONT|OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_POINTER_PASSTHROUGH,
        .foreground=OS_THEME_DANGER};
    if(!ui_file_picker_append(d,node,p->error))return false;
    for(uint32_t i=0;i<2U;++i) {
        uint32_t gap=pad/2U,button=inner>gap?(inner-gap)/2U:0;
        node=(os_control_node_t){.identifier=ui_file_picker_target(p,3U+i),.parent=3,.kind=OS_CONTROL_BUTTON,
            .x=(int32_t)(pad+i*(button+gap)),.y=(int32_t)bottom,.width=(int32_t)button,.height=(int32_t)row,
            .flags=OS_CONTROL_INHERIT_FONT,.background=i?OS_THEME_ACCENT:OS_THEME_FIELD,
            .foreground=i?OS_THEME_ON_ACCENT:OS_THEME_TEXT,.radius=font/3U};
        if(!ui_file_picker_append(d,node,i?(p->directory_only?"选择目录":p->save?"保存":"打开"):"取消"))return false;
    }
    d->request.hdr=(os_versioned_header_t){.size=sizeof(d->request),.version=OS_SYSCALL_ABI_VERSION};
    d->request.identifier=identifier;d->request.nodes=(uintptr_t)d->nodes;d->request.text=(uintptr_t)d->text;
    return true;
}
