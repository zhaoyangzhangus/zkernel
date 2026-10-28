#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
#include "../../user/runtime/file_picker.h"
#include "../../user/runtime/modal_tree.h"

uint32_t ui_text_size(void){return 16;}
uint32_t ui_text_line_height(void){return 24;}
static os_control_node_t nodes[128],saved[128];
static char text[4096],saved_text[4096];
static uint32_t count,bytes;

static void base(void) {
    memset(nodes,0,sizeof(nodes));memset(text,0,sizeof(text));
    count=9;bytes=4;memcpy(text,"base",4);
    nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,
        .kind=OS_CONTROL_CONTAINER,.flags=OS_CONTROL_WINDOW_FRAME|OS_CONTROL_MANAGED_FOCUS};
    for(unsigned i=1;i<count;++i)
        nodes[i]=(os_control_node_t){.identifier=i==8?0x10000100U:i+1U,.parent=0,
            .kind=OS_CONTROL_BUTTON,.width=100,.height=20,
            .flags=i==1?OS_CONTROL_DEFAULT_FOCUS:0};
    assert(os_control_tree_validate(nodes,count,bytes));
}
static void attach(const os_window_control_tree_t *modal,uint32_t prefix) {
    for(unsigned i=0;i<modal->count;++i)
        assert(( ((const os_control_node_t *)(uintptr_t)modal->nodes)[i].identifier & ~255U)==prefix);
    base();unsigned base_count=count;
    assert(ui_modal_tree_attach(nodes,&count,128,text,&bytes,sizeof(text),modal));
    assert(count==base_count+modal->count-1U);
    assert(bytes==4U+modal->text_size&&!memcmp(text,"base",4));
    assert(nodes[0].flags&OS_CONTROL_WINDOW_FRAME);
    assert((nodes[1].flags&OS_CONTROL_INERT)==OS_CONTROL_INERT);
    assert(!(nodes[1].flags&(OS_CONTROL_MANAGED_FOCUS|OS_CONTROL_WINDOW_FRAME)));
    assert(!nodes[1].background&&(nodes[1].flags&OS_CONTROL_BACKGROUND_ALPHA));
    for(unsigned i=1;i<=base_count;++i) {
        assert(!(nodes[i].flags&OS_CONTROL_DEFAULT_FOCUS));
        assert(nodes[i].kind!=OS_CONTROL_SURFACE);
    }
    assert(os_control_tree_validate(nodes,count,bytes));
    control_tree_box_t boxes[128];
    assert(control_tree_layout(nodes,count,bytes,(Rect){0,0,800,600},boxes));
    for(unsigned i=1;i<=base_count;++i)assert(!boxes[i].enabled&&boxes[i].inert);
}
static void rejected(const os_window_control_tree_t *modal,uint32_t capacity,uint32_t text_capacity) {
    memcpy(saved,nodes,sizeof(nodes));memcpy(saved_text,text,sizeof(text));
    unsigned old_count=count,old_bytes=bytes;
    assert(!ui_modal_tree_attach(nodes,&count,capacity,text,&bytes,text_capacity,modal));
    assert(count==old_count&&bytes==old_bytes);
    assert(!memcmp(saved,nodes,sizeof(nodes))&&!memcmp(saved_text,text,sizeof(text)));
}
int main(void) {
    ui_message_box_t message={0};ui_message_document_t document;
    ui_file_picker_document_t picker_document;
    os_file_info_t entries[2]={{.name="目录",.type=OS_FILE_TYPE_DIRECTORY},
        {.name="文件.txt",.type=OS_FILE_TYPE_REGULAR}};
    ui_file_picker_t picker={.active=true,.ready=true,.count=2,.entries=entries,
        .directory="/",.name="/"};
    const uint32_t generations[]={1,2,0x000fffffU};
    for(unsigned i=0;i<3;++i) {
        ui_message_show(&message,"公共消息框","正文",OS_MESSAGE_OK_CANCEL);
        message.generation=picker.generation=generations[i];
        assert(ui_message_build(&message,7,800,600,&document));
        assert(ui_file_picker_build(&picker,7,800,600,&picker_document));
        attach(&document.request,ui_message_target(&message,0));
        attach(&picker_document.request,ui_file_picker_target(&picker,0));
    }
    /* 递增、逆序及乱序标识复用同一校验索引；冲突和重复均须无副作用。 */
    for(unsigned variant=0;variant<96;++variant) {
        base();count=96;
        for(unsigned i=1;i<count;++i)nodes[i]=(os_control_node_t){
            .identifier=2U+(i*37U+variant)%95U,.parent=0,
            .kind=OS_CONTROL_BUTTON,.width=100,.height=20};
        assert(os_control_tree_validate(nodes,count,bytes));
        uint32_t old_identifier=nodes[42].identifier;
        nodes[42].identifier=nodes[17].identifier;
        rejected(&document.request,128,sizeof(text));
        nodes[42].identifier=document.nodes[0].identifier;
        rejected(&document.request,128,sizeof(text));
        nodes[42].identifier=old_identifier;
        memcpy(saved,nodes,sizeof(nodes));
        assert(ui_modal_tree_attach(nodes,&count,128,text,&bytes,sizeof(text),&document.request));
        assert(os_control_tree_validate(nodes,count,bytes));
        for(unsigned i=1;i<96;++i)assert(nodes[i+1U].identifier==saved[i].identifier);
    }
    /* 容量、冲突、重叠和过深输入必须整体拒绝，不留下半次合并。 */
    base();rejected(&document.request,count+document.request.count-2U,sizeof(text));
    rejected(&document.request,128,bytes+document.request.text_size-1U);
    uint32_t identifier=document.nodes[0].identifier;
    document.nodes[0].identifier=nodes[0].identifier;
    rejected(&document.request,128,sizeof(text));document.nodes[0].identifier=identifier;
    uint32_t parent=document.nodes[2].parent;document.nodes[2].parent=1;
    rejected(&document.request,128,sizeof(text));document.nodes[2].parent=parent;
    os_window_control_tree_t alias=document.request;alias.nodes=(uintptr_t)nodes;
    rejected(&alias,128,sizeof(text));alias=document.request;alias.text=(uintptr_t)text;
    rejected(&alias,128,sizeof(text));
    base();count=OS_CONTROL_TREE_MAX_DEPTH;
    for(unsigned i=1;i<count;++i)nodes[i]=(os_control_node_t){.identifier=i+1U,
        .parent=i-1U,.kind=OS_CONTROL_CONTAINER};
    assert(os_control_tree_validate(nodes,count,bytes));
    rejected(&document.request,128,sizeof(text));
    --count;
    assert(ui_modal_tree_attach(nodes,&count,128,text,&bytes,sizeof(text),&document.request));
    assert(os_control_tree_validate(nodes,count,bytes));
    puts("Modal tree: complete namespace isolation, fileman-like IDs, frame/input inheritance, depth boundary and atomic failure PASS");
}
