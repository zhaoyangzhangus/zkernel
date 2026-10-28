#include "control_file.h"
#include <uapi/window_theme.h>
#include <uapi/control_validation.h>
#ifdef UI_FILE_KERNEL
#include <kernel/kmem.h>
#include <kernel/string.h>
#define malloc(bytes) kmalloc((bytes), 0U)
#define free(pointer) kfree(pointer)
#else
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#endif

typedef struct ui_span { const char *data; size_t size; } ui_span_t;
typedef struct ui_parser {
    const char *source;
    size_t size, position;
    uint32_t capacity, resource_capacity, text_capacity;
    ui_file_document_t *document;
    ui_file_error_t *error;
    bool failed;
} ui_parser_t;

static bool ui_equal(ui_span_t span, const char *word) {
    size_t n=0;while(word[n])++n;
    return span.size==n && !memcmp(span.data,word,n);
}
static bool ui_fail(ui_parser_t *p,const char *message) {
    p->failed=true;
    if(p->error){p->error->offset=p->position;p->error->message=message;}
    return false;
}
static void ui_space(ui_parser_t *p) {
    while(p->position<p->size){
        char c=p->source[p->position];
        if(c==' '||c=='\t'||c=='\r'||c=='\n'){++p->position;continue;}
        if(p->size-p->position>=4U&&!memcmp(p->source+p->position,"<!--",4)){
            p->position+=4;
            while(p->size-p->position>=3U&&memcmp(p->source+p->position,"-->",3))++p->position;
            if(p->size-p->position>=3U){p->position+=3;continue;}
            p->position=p->size;
            ui_fail(p,"unterminated comment");
        }
        break;
    }
}
static bool ui_take(ui_parser_t *p,char c) {
    if(p->position<p->size&&p->source[p->position]==c){++p->position;return true;}
    return false;
}
static bool ui_name(ui_parser_t *p,ui_span_t *out);
/* 只统计开始标签和包含资源属性的标签；语义仍由下方唯一的解析器负责。
 * 复用名称、空白和注释扫描，跳过引号内容，不为普通控件预留应用资源。 */
static bool ui_node_capacity(ui_parser_t *p) {
    uint32_t count=0,resources=0,text_bytes=0;
    while(p->position<p->size){
        ui_space(p);
        if(p->failed)return false;
        if(p->position==p->size)break;
        if(!ui_take(p,'<')){++p->position;continue;}
        bool opening=!ui_take(p,'/'),resource=false,text_value=false;
        if(opening){
            if(count==OS_CONTROL_TREE_MAX_NODES)return ui_fail(p,"node limit exceeded");
            ++count;
        }
        while(p->position<p->size){
            ui_space(p);
            if(p->failed)return false;
            if(p->position==p->size)break;
            ui_span_t name;
            if(ui_name(p,&name)){
                bool binding=ui_equal(name,"app-directory") ||
                    ui_equal(name,"program") || ui_equal(name,"icon");
                text_value=opening && (binding || ui_equal(name,"text") || ui_equal(name,"wallpaper"));
                if(opening && !resource && binding){
                    resource=true;++resources;
                }
                continue;
            }
            char c=p->source[p->position++];
            if(c=='>')break;
            if(c=='\''||c=='"'){
                size_t start=p->position;
                while(p->position<p->size&&p->source[p->position]!=c)++p->position;
                if(text_value)text_bytes+=(uint32_t)(p->position-start);
                text_value=false;
                if(p->position<p->size)++p->position;
            }
        }
    }
    p->position=0;p->capacity=count;p->resource_capacity=resources;
    /* 转义解码只会缩短内容，原始属性字节数就是安全上界。空文档保留一字节。 */
    p->text_capacity=text_bytes>OS_CONTROL_TREE_MAX_TEXT?OS_CONTROL_TREE_MAX_TEXT:
        text_bytes?text_bytes:1U;
    return count!=0 || ui_fail(p,"expected control tag");
}
static bool ui_name(ui_parser_t *p,ui_span_t *out) {
    size_t start=p->position;
    while(p->position<p->size){
        char c=p->source[p->position];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='-'||
            (p->position>start&&c>='0'&&c<='9')))break;
        ++p->position;
    }
    *out=(ui_span_t){p->source+start,p->position-start};
    return out->size!=0;
}
static uint32_t ui_hash(ui_span_t name) {
    uint32_t hash=2166136261U;
    for(size_t i=0;i<name.size;++i)hash=(hash^(unsigned char)name.data[i])*16777619U;
    return hash?hash:1U;
}
uint32_t ui_file_identifier(const char *name) {
    if(!name)return 0;
    size_t size=0;while(name[size])++size;
    return ui_hash((ui_span_t){name,size});
}
os_control_node_t *ui_file_find(ui_file_document_t *d,const char *name) {
    if(!d||!d->nodes||!d->identifiers||!name)return 0;
    uint32_t id=ui_file_identifier(name);
    uint32_t first=0,last=d->request.count;
    while(first<last){
        uint32_t middle=first+(last-first)/2U,index=d->identifiers[middle];
        os_control_node_t *node=d->nodes+index;
        if(node->identifier<id)first=middle+1U;
        else if(node->identifier>id)last=middle;
        else return node;
    }
    return 0;
}
static bool ui_number(ui_span_t s,int32_t *out) {
    if(!s.size)return false;
    bool negative=s.data[0]=='-';size_t pos=negative?1U:0U;
    if(pos==s.size)return false;
    uint64_t value=0;
    for(;pos<s.size;++pos){
        unsigned digit=(unsigned char)s.data[pos]-'0';
        if(digit>9U)return false;
        value=value*10U+digit;
        if(value>(negative?2147483648ULL:2147483647ULL))return false;
    }
    *out=negative?(int32_t)(-(int64_t)value):(int32_t)value;return true;
}
typedef struct ui_word {const char *name;uint32_t value;} ui_word_t;
static bool ui_word(ui_span_t s,const ui_word_t *words,size_t count,uint32_t *out) {
    for(size_t i=0;i<count;++i)if(ui_equal(s,words[i].name)){*out=words[i].value;return true;}
    return false;
}
#define UI_WORD(s,table,out) ui_word((s),(table),sizeof(table)/sizeof((table)[0]),(out))
static bool ui_color(ui_span_t s,uint32_t *out) {
    static const ui_word_t colors[]={
        {"surface",OS_THEME_SURFACE},{"text",OS_THEME_TEXT},{"muted",OS_THEME_MUTED},
        {"accent",OS_THEME_ACCENT},{"on-accent",OS_THEME_ON_ACCENT},{"field",OS_THEME_FIELD},
        {"border",OS_THEME_BORDER},{"selected",OS_THEME_SELECTED},{"toolbar",OS_THEME_TOOLBAR},
        {"hover",OS_THEME_HOVER},{"danger",OS_THEME_DANGER}};
    if(UI_WORD(s,colors,out))return true;
    if((s.size!=7&&s.size!=9)||s.data[0]!='#')return false;
    uint32_t value=0;
    for(size_t i=1;i<s.size;++i){
        unsigned char c=s.data[i];unsigned d=c>='0'&&c<='9'?(unsigned)(c-'0'):
            c>='a'&&c<='f'?c-'a'+10U:c>='A'&&c<='F'?c-'A'+10U:16U;
        if(d==16)return false;
        value=value*16U+d;
    }
    *out=value;return true;
}
static bool ui_text(ui_parser_t *p,ui_span_t s,os_control_node_t *node) {
    node->text_offset=p->document->request.text_size;
    for(size_t i=0;i<s.size;++i){
        unsigned char c=s.data[i];
        if(c=='&'){
            static const ui_word_t entities[]={{"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''}};
            size_t start=++i;
            while(i<s.size&&s.data[i]!=';')++i;
            uint32_t decoded;
            if(i==s.size||!UI_WORD(((ui_span_t){s.data+start,i-start}),entities,&decoded))
                return ui_fail(p,"invalid text entity");
            c=(unsigned char)decoded;
        }
        if(!c||c=='\n'||c=='\r'||c=='\t')return ui_fail(p,"single-line text required");
        if(p->document->request.text_size>=p->text_capacity)return ui_fail(p,"text limit exceeded");
        p->document->text[p->document->request.text_size++]=(char)c;
    }
    node->text_length=p->document->request.text_size-node->text_offset;return true;
}
static bool ui_flags(ui_span_t s,uint32_t *out) {
    static const ui_word_t flags[]={
        {"hidden",OS_CONTROL_HIDDEN},{"disabled",OS_CONTROL_DISABLED},{"clip",OS_CONTROL_CLIP_CHILDREN},
        {"passthrough",OS_CONTROL_POINTER_PASSTHROUGH},{"alpha",OS_CONTROL_BACKGROUND_ALPHA},
        {"managed-focus",OS_CONTROL_MANAGED_FOCUS},{"default-focus",OS_CONTROL_DEFAULT_FOCUS},
        {"skip-tab",OS_CONTROL_SKIP_TAB},{"focus-arrows",OS_CONTROL_FOCUS_ARROWS},
        {"window-frame",OS_CONTROL_WINDOW_FRAME},
        {"no-hover",OS_CONTROL_NO_HOVER},
        {"checkerboard",OS_CONTROL_CHECKERBOARD},
        {"menu-panel",OS_CONTROL_MENU_PANEL},{"dialog-panel",OS_CONTROL_DIALOG_PANEL},
        {"dialog-actions",OS_CONTROL_DIALOG_ACTIONS},{"dialog-close",OS_CONTROL_DIALOG_CLOSE},
        {"grow",OS_CONTROL_FLEX_GROW},{"checked",OS_CONTROL_CHECKED},{"vertical",OS_CONTROL_VERTICAL},
        {"justify-center",OS_CONTROL_JUSTIFY_CENTER},{"justify-end",OS_CONTROL_JUSTIFY_END},
        {"justify-between",OS_CONTROL_JUSTIFY_BETWEEN},{"align-center",OS_CONTROL_ALIGN_CENTER},
        {"align-end",OS_CONTROL_ALIGN_END},{"align-stretch",OS_CONTROL_ALIGN_STRETCH}};
    uint32_t result=0;
    for(size_t pos=0;pos<s.size;){
        while(pos<s.size&&s.data[pos]==' ')++pos;
        size_t start=pos;
        while(pos<s.size&&s.data[pos]!=' ')++pos;
        if(start==pos)break;
        uint32_t bit;
        if(!UI_WORD(((ui_span_t){s.data+start,pos-start}),flags,&bit)||(result&bit))return false;
        result|=bit;
    }
    *out=result;return true;
}
static bool ui_attribute(ui_parser_t *p,os_control_node_t *node,ui_span_t key,ui_span_t value,uint64_t *seen) {
    static const char *const names[]={"id","text","layout","flags","x","y","width","height",
        "padding","gap","scroll-x","scroll-y","background","foreground","border-color",
        "border-width","radius","font-size","value","surface","app-directory","program","icon","symbol","page-size","icon-size","wallpaper"};
    unsigned field=0;
    while(field<sizeof(names)/sizeof(names[0])&&!ui_equal(key,names[field]))++field;
    if(field==sizeof(names)/sizeof(names[0]))return ui_fail(p,"unknown attribute");
    uint64_t bit=1ULL<<field;
    if(*seen&bit)return ui_fail(p,"duplicate attribute");
    *seen|=bit;
    if(field==26){
        if(node!=p->document->nodes)return ui_fail(p,"wallpaper must belong to root");
        os_control_node_t text={0};
        if(!ui_text(p,value,&text)||!text.text_length)return ui_fail(p,"empty wallpaper path");
        p->document->wallpaper_offset=text.text_offset;
        p->document->wallpaper_length=text.text_length;
        p->document->wallpaper_source_offset=(size_t)(value.data-p->source);
        p->document->wallpaper_source_length=value.size;
        return true;
    }
    if(field==23){
        static const ui_word_t symbols[]={{"back",OS_CONTROL_SYMBOL_BACK},{"forward",OS_CONTROL_SYMBOL_FORWARD},
            {"search",OS_CONTROL_SYMBOL_SEARCH},{"list",OS_CONTROL_SYMBOL_LIST},{"grid",OS_CONTROL_SYMBOL_GRID},
            {"sort-ascending",OS_CONTROL_SYMBOL_SORT_ASCENDING},{"sort-descending",OS_CONTROL_SYMBOL_SORT_DESCENDING},
            {"menu",OS_CONTROL_SYMBOL_MENU},{"home",OS_CONTROL_SYMBOL_HOME},{"drive",OS_CONTROL_SYMBOL_DRIVE},
            {"folder",OS_CONTROL_SYMBOL_FOLDER},{"file",OS_CONTROL_SYMBOL_FILE},{"close",OS_CONTROL_SYMBOL_CLOSE}};
        if(!UI_WORD(value,symbols,&node->value))return ui_fail(p,"unknown symbol");
        node->flags|=OS_CONTROL_SYMBOL;return true;
    }
    if(field>=20&&field<=22){
        ui_file_document_t *d=p->document;
        uint32_t index=(uint32_t)(node-d->nodes);
        ui_file_resource_t *r=d->resource_count?d->resources+d->resource_count-1U:0;
        if(!r||r->node_index!=index){
            if(d->resource_count>=p->resource_capacity)return ui_fail(p,"resource limit exceeded");
            r=d->resources+d->resource_count++;
            *r=(ui_file_resource_t){.node_index=index};
        }
        os_control_node_t text={0};
        if(!ui_text(p,value,&text)||!text.text_length)return ui_fail(p,"empty resource path");
        r->offset[field-20]=text.text_offset;r->length[field-20]=text.text_length;
        return true;
    }
    if(field==0){
        if(!value.size)return ui_fail(p,"empty identifier");
        for(size_t i=0;i<value.size;++i){
            char c=value.data[i];
            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='-'||
                (i&&c>='0'&&c<='9')))return ui_fail(p,"invalid identifier");
        }
        node->identifier=ui_hash(value);return true;
    }
    if(field==1)return ui_text(p,value,node);
    if(field==2){
        static const ui_word_t layouts[]={{"absolute",OS_CONTROL_ABSOLUTE},{"row",OS_CONTROL_ROW},{"column",OS_CONTROL_COLUMN}};
        return UI_WORD(value,layouts,&node->layout)||ui_fail(p,"invalid layout");
    }
    if(field==3){uint32_t flags;if(!ui_flags(value,&flags))return ui_fail(p,"invalid flags");node->flags|=flags;return true;}
    if(field>=12&&field<=14){
        uint32_t color;
        if(!ui_color(value,&color))return ui_fail(p,"invalid color");
        if(field==12){node->background=color;node->flags&=~OS_CONTROL_INHERIT_BACKGROUND;}
        if(field==13){node->foreground=color;node->flags&=~OS_CONTROL_INHERIT_FOREGROUND;}
        if(field==14){node->border_color=color;node->flags&=~OS_CONTROL_INHERIT_BORDER_COLOR;}
        return true;
    }
    int32_t number;
    if((field==6||field==7)&&ui_equal(value,"fill"))number=OS_CONTROL_FILL;
    else if(!ui_number(value,&number))return ui_fail(p,"invalid integer");
    if(number<0&&field!=4&&field!=5&&field!=10&&field!=11&&
        !((field==6||field==7)&&number==OS_CONTROL_FILL))return ui_fail(p,"negative size");
    switch(field){
        case 4:node->x=number;break;case 5:node->y=number;break;
        case 6:node->width=number;break;case 7:node->height=number;break;
        case 8:node->padding=(uint32_t)number;break;case 9:node->gap=(uint32_t)number;break;
        case 10:node->scroll_x=number;break;case 11:node->scroll_y=number;break;
        case 15:node->border_width=(uint32_t)number;node->flags&=~OS_CONTROL_INHERIT_BORDER_WIDTH;break;
        case 16:node->radius=(uint32_t)number;node->flags&=~OS_CONTROL_INHERIT_RADIUS;break;
        case 17:node->font_size=(uint32_t)number;node->flags&=~OS_CONTROL_INHERIT_FONT;break;
        case 18:node->value=(uint32_t)number;break;case 19:node->surface=(uint32_t)number;break;
        case 24:node->page_size=(uint32_t)number;break;
        case 25:node->icon_size=(uint32_t)number;break;
        default:return ui_fail(p,"unsupported attribute");
    }
    if(node->font_size>96U||node->radius>4096U||node->border_width>4096U||node->icon_size>4096U)
        return ui_fail(p,"style limit exceeded");
    return true;
}
static bool ui_node(ui_parser_t *p,uint32_t parent,uint32_t depth) {
    static const ui_word_t kinds[]={
        {"container",OS_CONTROL_CONTAINER},{"text",OS_CONTROL_TEXT},{"button",OS_CONTROL_BUTTON},
        {"surface",OS_CONTROL_SURFACE},{"checkbox",OS_CONTROL_CHECKBOX},{"radio",OS_CONTROL_RADIO},
        {"switch",OS_CONTROL_SWITCH},{"progress",OS_CONTROL_PROGRESS},{"slider",OS_CONTROL_SLIDER},
        {"scrollbar",OS_CONTROL_SCROLLBAR},{"list-item",OS_CONTROL_LIST_ITEM},{"tab",OS_CONTROL_TAB},
        {"separator",OS_CONTROL_SEPARATOR},{"window-button",OS_CONTROL_WINDOW_BUTTON}};
    ui_space(p);ui_span_t tag;
    if(depth==OS_CONTROL_TREE_MAX_DEPTH||!ui_take(p,'<')||!ui_name(p,&tag))return ui_fail(p,"expected control tag");
    uint32_t kind;
    if(!UI_WORD(tag,kinds,&kind)||(depth==0&&kind!=OS_CONTROL_CONTAINER))return ui_fail(p,"invalid control kind");
    uint32_t index=p->document->request.count;
    if(index==p->capacity)return ui_fail(p,"node limit exceeded");
    os_control_node_t *node=p->document->nodes+index;
    *node=(os_control_node_t){.parent=parent,.kind=kind,.width=OS_CONTROL_FILL,.height=32,
        .flags=depth?OS_CONTROL_INHERIT_STYLE:0U,.background=OS_THEME_SURFACE,
        .foreground=OS_THEME_TEXT,.border_color=OS_THEME_BORDER,.font_size=20};
    ++p->document->request.count;
    uint64_t seen=0;
    bool empty=false;
    for(;;){
        ui_space(p);
        if(ui_take(p,'/')){if(!ui_take(p,'>'))return ui_fail(p,"expected />");empty=true;break;}
        if(ui_take(p,'>'))break;
        ui_span_t key,value;
        if(!ui_name(p,&key))return ui_fail(p,"expected attribute");
        ui_space(p);if(!ui_take(p,'='))return ui_fail(p,"expected =");ui_space(p);
        char quote=p->position<p->size?p->source[p->position]:0;
        if(quote!='\''&&quote!='"')return ui_fail(p,"expected quoted value");
        ++p->position;size_t start=p->position;
        while(p->position<p->size&&p->source[p->position]!=quote){
            if(p->source[p->position]=='<'||!p->source[p->position])return ui_fail(p,"invalid attribute value");
            ++p->position;
        }
        value=(ui_span_t){p->source+start,p->position-start};
        if(!ui_take(p,quote)||!ui_attribute(p,node,key,value,&seen))return ui_fail(p,"invalid attribute");
    }
    if(!(seen&1U))return ui_fail(p,"control requires id");
    if(kind==OS_CONTROL_SURFACE&&node->surface!=1U)return ui_fail(p,"invalid Surface resource");
    if(((seen&(1ULL<<23))&&(seen&((1ULL<<18)|(1ULL<<19))))||
        ((seen&(1ULL<<18))&&(seen&(1ULL<<19)))||
        ((seen&(1ULL<<19))&&kind!=OS_CONTROL_SURFACE)||
        ((seen&(1ULL<<18))&&!os_control_is_range(kind)&&kind!=OS_CONTROL_PROGRESS&&kind!=OS_CONTROL_WINDOW_BUTTON))
        return ui_fail(p,"value does not match control kind");
    if(empty)return true;
    for(;;){
        ui_space(p);
        if(p->size-p->position>=2U&&!memcmp(p->source+p->position,"</",2)){
            p->position+=2;ui_span_t end;
            if(!ui_name(p,&end)||end.size!=tag.size||memcmp(end.data,tag.data,tag.size))
                return ui_fail(p,"mismatched closing tag");
            ui_space(p);return ui_take(p,'>')||ui_fail(p,"expected >");
        }
        if(kind!=OS_CONTROL_CONTAINER)return ui_fail(p,"only containers may contain controls");
        if(!ui_node(p,index,depth+1U))return false;
    }
}

void ui_file_destroy(ui_file_document_t *d) {
    if(!d)return;
    free(d->nodes);*d=(ui_file_document_t){0};
}

static bool ui_path_piece(const char *text,size_t size,bool directory) {
    if(!size||(directory&&text[0]!='/'))return false;
    size_t start=directory?1U:0U;
    for(size_t pos=start;pos<=size;++pos){
        if(pos==size||text[pos]=='/'){
            size_t length=pos-start;
            if(!length||(!directory&&pos!=size)||
                (length==1&&text[start]=='.')||
                (length==2&&text[start]=='.'&&text[start+1]=='.'))return false;
            start=pos+1;
        }else{
            unsigned char c=text[pos];
            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||
                c=='_'||c=='-'||c=='.'))return false;
        }
    }
    return true;
}
bool ui_file_resource_path(const ui_file_document_t *d,const ui_file_resource_t *r,
    uint32_t field,char *output,size_t capacity) {
    if(!d||!d->text||!r||!output||field==0||field>=UI_RESOURCE_FIELDS)return false;
    for(unsigned i=0;i<UI_RESOURCE_FIELDS;++i)
        if(r->offset[i]>d->request.text_size||r->length[i]>d->request.text_size-r->offset[i])return false;
    size_t dir=r->length[0],name=r->length[field];
    const char *base=d->text+r->offset[0],*file=d->text+r->offset[field];
    if(!ui_path_piece(base,dir,true)||!ui_path_piece(file,name,false)||dir+name+2U>capacity)return false;
    memcpy(output,base,dir);output[dir]='/';memcpy(output+dir+1,file,name);output[dir+1+name]=0;
    return true;
}

bool ui_file_parse(const char *source,size_t size,ui_file_document_t *out,ui_file_error_t *error) {
    if(error)*error=(ui_file_error_t){0};
    if(!source||!out||out->nodes||out->identifiers||out->text||out->resources||!size||size>UI_FILE_MAX_BYTES){
        if(error)error->message="invalid input or nonempty output";
        return false;
    }
    ui_file_document_t candidate={0};
    ui_parser_t parser={.source=source,.size=size,.document=&candidate,.error=error};
    if(!ui_node_capacity(&parser))goto fail;
    uint32_t capacity=parser.capacity;
    /* 文档只分配一次：节点、ID 索引、对齐后的资源表、文字依次放入同一块内存。 */
    size_t resource_offset=(size_t)capacity*(sizeof(*candidate.nodes)+sizeof(*candidate.identifiers));
    size_t alignment=_Alignof(ui_file_resource_t);
    resource_offset=(resource_offset+alignment-1U)&~(alignment-1U);
    size_t text_offset=resource_offset+(size_t)parser.resource_capacity*sizeof(*candidate.resources);
    candidate.nodes=malloc(text_offset+parser.text_capacity);
    if(!candidate.nodes){ui_fail(&parser,"out of memory");goto fail;}
    candidate.identifiers=(uint16_t *)(candidate.nodes+capacity);
    candidate.resources=parser.resource_capacity?
        (ui_file_resource_t *)((char *)candidate.nodes+resource_offset):0;
    candidate.text=(char *)candidate.nodes+text_offset;
    if(!ui_node(&parser,OS_CONTROL_NO_PARENT,0U))goto fail;
    ui_space(&parser);
    if(parser.failed)goto fail;
    if(parser.position!=size){ui_fail(&parser,"trailing content");goto fail;}
    if(!os_control_tree_validate_indexed(candidate.nodes,candidate.request.count,candidate.request.text_size,
                                       candidate.identifiers)){
        ui_fail(&parser,"invalid control tree semantics or duplicate identifier");goto fail;
    }
    for(uint32_t i=0;i<candidate.resource_count;++i){
        char path[256];ui_file_resource_t *r=candidate.resources+i;
        if(candidate.nodes[r->node_index].kind!=OS_CONTROL_BUTTON||
            !ui_file_resource_path(&candidate,r,UI_RESOURCE_PROGRAM,path,sizeof(path))||
            !ui_file_resource_path(&candidate,r,UI_RESOURCE_ICON,path,sizeof(path))){
            ui_fail(&parser,"invalid application resource binding");goto fail;
        }
    }
    if(candidate.wallpaper_length){
        char path[256];
        if(!ui_file_wallpaper_path(&candidate,path,sizeof(path))){ui_fail(&parser,"invalid wallpaper path");goto fail;}
    }
    candidate.request.hdr=(os_versioned_header_t){.size=sizeof(candidate.request),.version=1};
    candidate.request.nodes=(uintptr_t)candidate.nodes;candidate.request.text=(uintptr_t)candidate.text;
    *out=candidate;return true;
fail:
    ui_file_destroy(&candidate);return false;
}

#ifndef UI_FILE_KERNEL
bool ui_file_load(const char *path,ui_file_document_t *out,ui_file_error_t *error) {
    if(error)*error=(ui_file_error_t){0};
    if(!path||!out||out->nodes||out->identifiers||out->text||out->resources){if(error)error->message="invalid output";return false;}
    /* 先避免特殊文件在打开时阻塞，再验证类型；界面句柄不传给启动的应用。 */
    int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){if(error)error->message="cannot open UI file";return false;}
    struct stat info;
    if(fstat(fd,&info)<0||!S_ISREG(info.st_mode)||info.st_size<=0||
        (uint64_t)info.st_size>UI_FILE_MAX_BYTES){
        if(error)error->message="invalid UI file size or type";
        close(fd);return false;
    }
    size_t capacity=(size_t)info.st_size+1U;
    char *source=malloc(capacity);size_t size=0;bool ok=false;
    if(!source){if(error)error->message="out of memory";goto done;}
    while(size<capacity){
        ssize_t n=read(fd,source+size,capacity-size);
        if(n<0){if(errno==EINTR)continue;if(error)error->message="cannot read UI file";goto done;}
        if(!n)break;
        size+=(size_t)n;
    }
    if(size==capacity){if(error)error->message="UI file changed while reading";goto done;}
    ok=ui_file_parse(source,size,out,error);
done:
    free(source);close(fd);return ok;
}
#endif

/* 文件名允许中文、空格与实体字符；拒绝控制字符及非规范路径片段。 */
static bool ui_wallpaper_valid(const char *path,size_t length) {
    if(length<2U || length>=256U || path[0]!='/')return false;
    size_t begin=1U;
    for(size_t i=1U;i<=length;++i) {
        if(i==length || path[i]=='/') {
            size_t n=i-begin;
            if(!n || (n==1U&&path[begin]=='.') ||
                (n==2U&&path[begin]=='.'&&path[begin+1U]=='.'))return false;
            begin=i+1U;
        } else if((unsigned char)path[i]<32U || (unsigned char)path[i]==127U)return false;
    }
    return true;
}
bool ui_file_wallpaper_path(const ui_file_document_t *d,char *out,size_t capacity) {
    if(!d || !out || !d->text || d->wallpaper_offset>d->request.text_size ||
       d->wallpaper_length>d->request.text_size-d->wallpaper_offset || d->wallpaper_length>=capacity)return false;
    const char *path=d->text+d->wallpaper_offset;
    if(!ui_wallpaper_valid(path,d->wallpaper_length))return false;
    memcpy(out,path,d->wallpaper_length);out[d->wallpaper_length]=0;return true;
}
bool ui_file_set_wallpaper(const char *source,size_t size,const char *path,char **out,size_t *out_size) {
    if(!source || !path || !out || *out || !out_size)return false;
    size_t n=0;while(n<256U&&path[n])++n;
    if(!ui_wallpaper_valid(path,n))return false;
    ui_file_document_t d={0};
    if(!ui_file_parse(source,size,&d,0))return false;
    size_t offset=d.wallpaper_source_offset,old=d.wallpaper_source_length;
    bool ok=d.wallpaper_length && offset<=size && old<=size-offset;
    ui_file_destroy(&d);
    if(!ok)return false;
    size_t escaped=0;
    for(size_t i=0;i<n;++i)escaped+=path[i]=='&'?5U:path[i]=='"'||path[i]=='\''?6U:path[i]=='<'||path[i]=='>'?4U:1U;
    if(size-old>UI_FILE_MAX_BYTES-escaped)return false;
    size_t total=size-old+escaped;
    char *next=malloc(total+1U);
    if(!next)return false;
    memcpy(next,source,offset);size_t used=offset;
    for(size_t i=0;i<n;++i) {
        const char *entity=path[i]=='&'?"&amp;":path[i]=='"'?"&quot;":path[i]=='\''?"&apos;":path[i]=='<'?"&lt;":path[i]=='>'?"&gt;":0;
        if(entity){size_t bytes=0;while(entity[bytes])++bytes;memcpy(next+used,entity,bytes);used+=bytes;}
        else next[used++]=path[i];
    }
    memcpy(next+used,source+offset+old,size-offset-old);next[total]=0;
    *out=next;*out_size=total;return true;
}
