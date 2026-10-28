#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
#include "../../user/runtime/message_box.h"

static uint32_t font=20U;
uint32_t ui_text_line_height(void){return font;}
uint32_t ui_text_size(void){return font;}
uint32_t ui_text_measure_range(const char *text,size_t n){(void)text;return (uint32_t)n*(font/2U);}

static os_window_event_t key(const ui_message_box_t *box,uint32_t code,int value) {
    return (os_window_event_t){.type=OS_WINDOW_EVENT_CONTROL,
        .control={.action=OS_CONTROL_KEY,.target=ui_message_target(box,0),.code=code,.value=value}};
}
static os_window_event_t click(const ui_message_box_t *box,uint32_t index) {
    return (os_window_event_t){.type=OS_WINDOW_EVENT_CONTROL,
        .control={.action=OS_CONTROL_CLICK,.target=ui_message_target(box,index)}};
}
static void utf8_boundaries(void) {
    const char *samples[]={"中文标题与正文", "a中b文", "😀窗口📁", "\xff\xe4\xb8", "\xf0\x80\x80\x80", "�"};
    for(unsigned s=0;s<sizeof(samples)/sizeof(samples[0]);++s)
        for(unsigned capacity=0;capacity<=32;++capacity) {
            char buffer[34];memset(buffer,0x55,sizeof(buffer));
            ui_message_copy(buffer+1,capacity,samples[s]);
            assert(buffer[0]==0x55 && buffer[capacity+1]==0x55);
            if(!capacity){assert(buffer[1]==0x55);continue;}
            size_t length=strlen(buffer+1);assert(length<capacity);
            for(size_t at=0;at<length;) {
                size_t used=0;uint32_t code=os_utf8_next(buffer+1+at,length-at,&used);
                assert(used && (code!=0xfffdU || used==3U));at+=used;
            }
        }
    ui_message_copy(0,0,"ignored");
    char empty[1];ui_message_copy(empty,1,0);assert(!empty[0]);
}
static void verify_buttons(const ui_message_box_t *box,const ui_message_document_t *document,
    const control_tree_box_t *boxes,uint32_t width,uint32_t height) {
    uint32_t count=ui_message_count(box->buttons);
    assert(document->nodes[3].flags&OS_CONTROL_DIALOG_PANEL);
    assert(boxes[3].style.radius==(font*OS_THEME_RADIUS+8U)/16U);
    assert(boxes[3].style.border_width==1U&&boxes[3].style.border_color==OS_THEME_BORDER);
    assert(boxes[3].style.background==OS_THEME_SURFACE);
    assert(document->nodes[0].foreground==OS_THEME_TEXT);
    uint32_t row=OS_CONTROL_NO_PARENT;
    for(uint32_t i=0;i<document->request.count;++i)
        if(document->nodes[i].identifier==ui_message_target(box,9))row=i;
    assert(row!=OS_CONTROL_NO_PARENT&&document->nodes[row].layout==OS_CONTROL_ROW);
    const control_tree_box_t *bar=boxes+row;
    for(uint32_t i=0;i<document->request.count;++i)
        if((document->nodes[i].flags&OS_CONTROL_DIALOG_CLOSE)&&boxes[i].visible)
            assert(boxes[i].bounds.y1<=bar->bounds.y0);
    assert(boxes[3].bounds.x0>=0 && boxes[3].bounds.y0>=0);
    assert(boxes[3].bounds.x1<=(int32_t)width && boxes[3].bounds.y1<=(int32_t)height);
    assert(bar->bounds.x0>=boxes[3].bounds.x0 && bar->bounds.x1<=boxes[3].bounds.x1);
    uint32_t gap=font/2U;
    uint32_t extent=(uint32_t)(bar->bounds.x1-bar->bounds.x0);
    uint32_t gaps=(count-1U)*gap;
    uint32_t remaining=extent>gaps ? extent-gaps : 0;
    int32_t x=bar->bounds.x0;
    for(uint32_t n=0;n<count;++n){
        uint32_t index=OS_CONTROL_NO_PARENT;
        for(uint32_t i=0;i<document->request.count;++i)
            if(document->nodes[i].identifier==ui_message_target(box,n+2))index=i;
        assert(index!=OS_CONTROL_NO_PARENT);
        const os_control_node_t *node=document->nodes+index;
        const control_tree_box_t *b=boxes+index;
        uint32_t allocated=remaining/(count-n);remaining-=allocated;
        assert(node->parent==row&&node->x==0&&node->y==0&&node->width==0);
        assert((node->flags&OS_CONTROL_FLEX_GROW)&&b->style.font_size==font);
        if(box->buttons==OS_MESSAGE_DELETE_CANCEL) {
            assert(node->background==(n?OS_THEME_ACCENT:OS_THEME_DANGER));
            assert(!!(node->flags&OS_CONTROL_DEFAULT_FOCUS)==(n==1));
        }
        assert(b->bounds.x0==x&&b->bounds.x1==x+(int32_t)allocated);
        assert(b->bounds.y0==bar->bounds.y0&&b->bounds.y1==bar->bounds.y1);
        control_tree_box_t found;
        assert(control_tree_find(document->nodes,document->request.count,
            (Rect){0,0,(int32_t)width,(int32_t)height},node->identifier,&found)==index);
        assert(!memcmp(&found.bounds,&b->bounds,sizeof(Rect)));
        x=b->bounds.x1+(int32_t)gap;
    }
    if(extent>=gaps) assert(x-(int32_t)gap==bar->bounds.x1);
    assert(!remaining);
}
int main(void) {
    utf8_boundaries();
    ui_message_box_t box={0};ui_message_document_t document;
    control_tree_box_t boxes[OS_MESSAGE_NODE_CAPACITY];
    for(unsigned mode=0;mode<=OS_MESSAGE_DELETE_CANCEL;++mode) {
        for(unsigned i=0;i<ui_message_count(mode);++i) {
            ui_message_show(&box,"Title","Some message",mode);
            assert(ui_message_build(&box,7,800,600,&document));
            assert(document.request.identifier==7&&document.request.nodes==(uintptr_t)document.nodes);
            assert(control_tree_layout(document.nodes,document.request.count,document.request.text_size,
                (Rect){0,0,800,600},boxes));
            verify_buttons(&box,&document,boxes,800,600);
            uint32_t button=control_tree_find(document.nodes,document.request.count,(Rect){0,0,800,600},
                ui_message_target(&box,2+i),0);
            assert(button!=OS_CONTROL_NO_PARENT);
            int32_t x=boxes[button].bounds.x0+2,y=boxes[button].bounds.y0+2;
            uint32_t hit=control_tree_hit_test(document.nodes,document.request.count,boxes,x,y);
            assert(hit!=OS_CONTROL_NO_PARENT&&document.nodes[hit].identifier==ui_message_target(&box,2+i));
            /* 应用不接受原始坐标伪造的点击，只消费窗口系统生成的控件语义。 */
            os_window_event_t raw={.type=OS_WINDOW_EVENT_INPUT,.pointer_x=x,.pointer_y=y,
                .input={.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_RELEASE}};
            assert(ui_message_event(&box,&raw)==OS_MESSAGE_NONE&&box.active);
            os_window_event_t event=click(&box,2+i);
            assert(ui_message_event(&box,&event)==ui_message_choice(mode,i)&&!box.active);
        }
        ui_message_show(&box,"Title","Message",mode);
        os_window_event_t stale=click(&box,2);
        os_window_event_t stale_escape=key(&box,0x29,OS_INPUT_VALUE_PRESS);
        ui_message_show(&box,"New title","New message",mode);
        assert(ui_message_event(&box,&stale)==OS_MESSAGE_NONE&&box.active);
        assert(ui_message_event(&box,&stale_escape)==OS_MESSAGE_NONE&&box.active);
        const uint32_t managed[]={0x28,0x58,0x2c,0x2b,0x50,0x4f};
        for(unsigned j=0;j<sizeof(managed)/sizeof(managed[0]);++j){
            os_window_event_t raw_key=key(&box,managed[j],OS_INPUT_VALUE_PRESS);
            assert(ui_message_event(&box,&raw_key)==OS_MESSAGE_NONE&&box.active);
        }
        os_window_event_t event=key(&box,0x29,OS_INPUT_VALUE_PRESS);
        assert(ui_message_event(&box,&event)==OS_MESSAGE_CANCEL&&!box.active);
        ui_message_show(&box,"Title","Message",mode);event=click(&box,1);
        assert(ui_message_event(&box,&event)==OS_MESSAGE_CANCEL);
        ui_message_show(&box,"Title","Message",mode);event.type=OS_WINDOW_EVENT_CLOSE_REQUEST;
        assert(ui_message_event(&box,&event)==OS_MESSAGE_CANCEL);
    }
    char long_text[1024];memset(long_text,'x',1023);long_text[1023]=0;
    ui_message_show(&box,long_text,long_text,OS_MESSAGE_SAVE_DISCARD_CANCEL);
    assert(strlen(box.title)==63&&strlen(box.message)==511);
    for(unsigned size=0;size<800;++size) {
        uint32_t w=size,h=size/2U;font=16+size%49;
        if(!w||!h){assert(!ui_message_build(&box,7,w,h,&document));continue;}
        assert(ui_message_build(&box,7,w,h,&document));
        assert(control_tree_layout(document.nodes,document.request.count,document.request.text_size,
            (Rect){0,0,(int32_t)w,(int32_t)h},boxes));
        assert(document.request.count<=OS_MESSAGE_NODE_CAPACITY&&document.request.text_size<=OS_MESSAGE_TEXT_CAPACITY);
        verify_buttons(&box,&document,boxes,w,h);
        assert(document.nodes[1].kind==OS_CONTROL_SURFACE&&document.nodes[1].surface==1&&
            (document.nodes[1].flags&OS_CONTROL_DISABLED));
        assert(document.nodes[2].flags&OS_CONTROL_BACKGROUND_ALPHA);
        assert(document.nodes[0].flags&OS_CONTROL_MANAGED_FOCUS);
        unsigned defaults=0,skipped=0;
        for(uint32_t i=0;i<document.request.count;++i){
            if(document.nodes[i].flags&OS_CONTROL_DEFAULT_FOCUS){
                ++defaults;assert(document.nodes[i].identifier==ui_message_target(&box,1U+ui_message_count(box.buttons)));
            }
            if(document.nodes[i].flags&OS_CONTROL_SKIP_TAB){
                ++skipped;assert(document.nodes[i].identifier==ui_message_target(&box,1));
            }
        }
        assert(defaults==1&&skipped==1);
        /* 对话框外部不能命中被遮罩的编辑器 Surface。 */
        uint32_t hit=control_tree_locate(document.nodes,document.request.count,
            (Rect){0,0,(int32_t)w,(int32_t)h},0,0,0);
        assert(hit==OS_CONTROL_NO_PARENT||document.nodes[hit].kind!=OS_CONTROL_SURFACE);
    }
    ui_message_show(&box,"中文标题😀","第一行中文内容与表情📁\n第二行文字 abc def ghi",OS_MESSAGE_OK_CANCEL);
    for(unsigned size=12;size<=96;size+=4)for(unsigned width=80;width<=1000;width+=29) {
        font=size;
        assert(ui_message_build(&box,7,width,700,&document));
        assert(control_tree_layout(document.nodes,document.request.count,document.request.text_size,
            (Rect){0,0,(int32_t)width,700},boxes));
        for(unsigned i=0;i<document.request.count;++i) {
            const os_control_node_t *node=document.nodes+i;
            for(size_t at=0;at<node->text_length;) {
                size_t used;uint32_t code=os_utf8_next(document.text+node->text_offset+at,node->text_length-at,&used);
                assert(used && code!=0xfffdU);at+=used;
            }
        }
    }
    memset(box.title,'x',sizeof(box.title));assert(!ui_message_build(&box,7,800,600,&document));
    ui_message_show(&box,"Title","Message",OS_MESSAGE_OK);
    memset(box.message,'x',sizeof(box.message));assert(!ui_message_build(&box,7,800,600,&document));
    puts("Message controls: UTF-8 truncation/normalization, Chinese/emoji wrapping, bounded inputs, semantic choices, stale events and 800 font/viewport sizes PASS");
}
