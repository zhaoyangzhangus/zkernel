"""执行真实控件树，验证继承计算、嵌套 Flex 布局及输入几何一致性。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/control_tree.h>
#include <kernel/control_selection.h>
#include <uapi/window_theme.h>
static os_control_node_t nodes[64], saved[64];
static control_tree_box_t boxes[64];
static void init(unsigned count) {
 memset(nodes,0,sizeof(nodes));
 for(unsigned i=0;i<count;++i)nodes[i]=(os_control_node_t){.identifier=i+1,
  .parent=i?i-1:OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,.width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL};
}
static void layout(unsigned count,int w,int h) {
 memcpy(saved,nodes,sizeof(nodes));
 assert(control_tree_layout(nodes,count,0,(Rect){0,0,w,h},boxes));
 assert(!memcmp(saved,nodes,sizeof(nodes)));
 for(unsigned i=0;i<count;++i){
  control_tree_box_t found;
  assert(control_tree_find(nodes,count,(Rect){0,0,w,h},i+1,&found)==i);
  assert(!memcmp(&found.style,&boxes[i].style,sizeof(found.style)));
  assert(!memcmp(&found.bounds,&boxes[i].bounds,sizeof(Rect)));
 }
}
static void rect(unsigned i,int x,int y,int w,int h){
 assert(boxes[i].bounds.x0==x&&boxes[i].bounds.y0==y);
 assert(boxes[i].bounds.x1==x+w&&boxes[i].bounds.y1==y+h);
}
/* 对照逐字符中心公式，缓存路径故意不提供原文字，防止偷偷回到线性解码。 */
static void selection_case(const char *text,uint32_t length,uint32_t font) {
 control_text_cell_t *cells=calloc(length?length:1,sizeof(*cells));assert(cells);
 uint32_t count=0,width=0;
 for(uint32_t at=0;at<length;) {
  size_t used;uint32_t code=os_utf8_next(text+at,length-at,&used);
  if(!used){used=1;code=0xfffdU;}
  width+=code<128U?font/2U:font;
  cells[count++]=(control_text_cell_t){.end=width,.byte_offset=(uint16_t)at};at+=(uint32_t)used;
 }
 control_text_run_t run={.count=count,.height=font,.width=width,.cells=cells};
 for(unsigned sample=0;sample<200;++sample) {
  int64_t x=sample==0?INT64_MIN:sample==1?INT64_MAX:(int64_t)(sample*7919U%((uint64_t)width+9U))-4;
  uint32_t expected=length;
  for(uint32_t i=0;i<count;++i) {
   uint32_t left=i?cells[i-1].end:0,advance=cells[i].end-left;
   if(x<(int64_t)left+advance/2U){expected=cells[i].byte_offset;break;}
  }
  uint32_t actual=control_selection_prepared_offset(&run,0,length,font,x);
  assert(actual==expected);
  assert(control_selection_prepared_offset(&run,text,length,font+1,x)==
      control_selection_offset(text,length,font+1,x));
 }
 for(uint32_t i=0;i<=count;++i) {
  uint32_t bytes=i<count?cells[i].byte_offset:length;
  uint32_t expected=i?cells[i-1].end:0;
  assert(control_selection_prepared_prefix(&run,0,bytes,font)==expected);
  if(i<32U || i%1024U==0U || i==count)
   assert(control_selection_prepared_prefix(&run,text,bytes,font+1)==control_selection_width(text,bytes,font+1));
 }
 free(cells);
}
int main(void){
 _Static_assert(sizeof(control_text_cell_t)==8,"字符索引保持八字节");
 for(uint32_t font=0;font<=96;++font) {
  const char mixed[]={ 'A',(char)0xe4,(char)0xb8,(char)0xad,0,'B',(char)0xf0,(char)0x9f,
   (char)0x98,(char)0x80,(char)0xff,(char)0xc2 };
  selection_case(mixed,sizeof(mixed),font);
  selection_case("",0,font);
  for(uint32_t length=0;length<97;++length)for(int64_t x=-3;x<450;x+=7) {
   char ascii[97];memset(ascii,'A',sizeof(ascii));
   assert(control_selection_prepared_offset(0,0,length,font,x)==control_selection_offset(ascii,length,font,x));
  }
 }
 char *long_text=malloc(65536);assert(long_text);memset(long_text,'A',65536);
 long_text[0]=(char)0xe4;long_text[1]=(char)0xb8;long_text[2]=(char)0xad;
 selection_case(long_text,65536,16);selection_case(long_text,65536,95);free(long_text);
 /* 已准备的宽度不得读取文字；换字体后仍与 UTF-8 解码结果一致。 */
 const char mixed[]="A中B";
 for(uint32_t font=8;font<=96;++font) {
  assert(control_selection_prepared_width(0,0,65536,font)==65536U*os_control_text_cell_width(' ',font));
  control_text_run_t run={.height=font,.width=control_selection_width(mixed,5,font)};
  assert(control_selection_prepared_width(&run,0,5,font)==run.width);
  assert(control_selection_prepared_width(&run,mixed,5,font+1)==control_selection_width(mixed,5,font+1));
  os_control_node_t label={.kind=OS_CONTROL_BUTTON,.text_length=5};
  control_tree_box_t box={.bounds={-100,7,300,107}};
  uint32_t expected_width=2U*(font/2U)+font;
  assert(run.width==expected_width);
  int32_t left=-100+(400-(int32_t)expected_width)/2,top=7+(100-(int32_t)font)/2;
  Rect expected={left,top,left+(int32_t)expected_width,top+(int32_t)font};
  Rect prepared=control_text_bounds_width(&label,&box,run.width,font);
  assert(!memcmp(&expected,&prepared,sizeof(expected)));
 }
 /* 各类控件共用文字起点，独立公式覆盖小尺寸、字号和超长居中。 */
 const unsigned kinds[]={OS_CONTROL_TEXT,OS_CONTROL_BUTTON,OS_CONTROL_TAB,OS_CONTROL_LIST_ITEM,
  OS_CONTROL_CHECKBOX,OS_CONTROL_RADIO,OS_CONTROL_SWITCH};
 for(unsigned k=0;k<sizeof(kinds)/sizeof(*kinds);++k)for(unsigned sample=0;sample<1000;++sample) {
  unsigned font=1+sample%96,w=sample%101,h=sample%43,text_width=sample%307;
  os_control_node_t label={.kind=kinds[k]};
  control_tree_box_t box={.bounds={-83,9,-83+(int32_t)w,9+(int32_t)h},.style={.padding=sample%11}};
  int64_t expected_x=-83,expected_y=9;
  bool choice=kinds[k]==OS_CONTROL_CHECKBOX||kinds[k]==OS_CONTROL_RADIO||kinds[k]==OS_CONTROL_SWITCH;
  if(choice) {
   unsigned side=font<h?font:h,limit=kinds[k]==OS_CONTROL_SWITCH?w/2:w;
   if(side>limit)side=limit;
   unsigned mark=kinds[k]==OS_CONTROL_SWITCH?2*side:side;
   unsigned offset=mark<w?mark+8:w;if(offset>w)offset=w;
   expected_x+=offset;
  }
  if(kinds[k]==OS_CONTROL_BUTTON||kinds[k]==OS_CONTROL_TAB) {
   expected_x+=((int64_t)w-text_width)/2;expected_y+=((int64_t)h-font)/2;
  } else {
   expected_x+=box.style.padding;
   expected_y+=choice||kinds[k]==OS_CONTROL_LIST_ITEM?((int64_t)h-font)/2:(int64_t)box.style.padding;
  }
  control_text_position_t position=control_text_position(&label,&box,text_width,font);
  assert(position.x==expected_x&&position.y==expected_y);
 }
 {
  os_control_node_t label={.kind=OS_CONTROL_BUTTON};
  control_tree_box_t box={.bounds={INT32_MIN,INT32_MIN,INT32_MIN+20,INT32_MIN+20}};
  control_text_position_t position=control_text_position(&label,&box,UINT32_MAX,96);
  assert(position.x==(int64_t)INT32_MIN+((int64_t)20-UINT32_MAX)/2);
  assert(position.y==(int64_t)INT32_MIN-38);
 }
 /* 模态正文不接收输入，但不使用禁用配色；明确禁用的子控件保持原语义。 */
 init(4);nodes[1].flags=OS_CONTROL_INERT;
 nodes[2].kind=OS_CONTROL_BUTTON;nodes[2].parent=1;
 nodes[3].kind=OS_CONTROL_BUTTON;nodes[3].parent=1;nodes[3].flags=OS_CONTROL_DISABLED;
 layout(4,321,199);
 assert(!boxes[1].enabled&&boxes[1].inert);
 assert(!boxes[2].enabled&&boxes[2].inert);
 assert(!boxes[3].enabled&&!boxes[3].inert);
 unsigned hit=control_tree_hit_test(nodes,4,boxes,10,10);
 assert(hit!=1&&hit!=2&&hit!=3);
 init(64);nodes[0].flags=OS_CONTROL_INHERIT_STYLE;
 for(unsigned i=1;i<64;++i)nodes[i].flags=OS_CONTROL_INHERIT_STYLE;
 layout(64,321,199);
 assert(boxes[63].style.foreground==OS_THEME_TEXT&&boxes[63].style.background==OS_THEME_SURFACE);
 nodes[0].flags=OS_CONTROL_BACKGROUND_ALPHA;nodes[0].background=0x80010203;
 nodes[0].foreground=0x123456;nodes[0].font_size=24;nodes[0].radius=8;
 nodes[0].border_width=2;nodes[0].border_color=0xaabbcc;
 nodes[20].flags&=~OS_CONTROL_INHERIT_FONT;nodes[20].font_size=32;
 nodes[30].flags&=~OS_CONTROL_INHERIT_RADIUS;nodes[30].radius=0;
 nodes[40].flags|=OS_CONTROL_DISABLED;
 layout(64,321,199);
 for(unsigned i=0;i<64;++i){
  assert(boxes[i].style.font_size==(i<20?24:32));
  assert(boxes[i].style.radius==(i<30?8:0));
  assert(boxes[i].style.background==0x80010203&&boxes[i].style.background_alpha);
  assert(boxes[i].style.foreground==0x123456&&boxes[i].style.border_width==2);
  assert(boxes[i].style.border_color==0xaabbcc&&boxes[i].enabled==(i<40));
 }
 /* 同级不能继承前一子树的覆盖值；不请求继承时保留旧接口的显式零值。 */
 nodes[63].parent=0;nodes[63].flags=OS_CONTROL_INHERIT_TEXT;
 layout(64,321,199);
 assert(boxes[63].style.font_size==24&&boxes[63].style.radius==0);
 assert(boxes[63].style.background==0&&!boxes[63].style.background_alpha&&boxes[63].enabled);
 unsigned cases=0;
 for(unsigned axis=0;axis<2;++axis)for(int extent=80;extent<340;++extent){
  init(6);nodes[0].layout=axis?OS_CONTROL_COLUMN:OS_CONTROL_ROW;
  nodes[0].padding=5;nodes[0].gap=3;nodes[0].flags=OS_CONTROL_ALIGN_CENTER;
  for(unsigned i=1;i<6;++i){nodes[i].parent=0;nodes[i].width=10;nodes[i].height=10;}
  nodes[2].flags=OS_CONTROL_FLEX_GROW;
  nodes[3].flags=OS_CONTROL_HIDDEN;
  nodes[4].flags=OS_CONTROL_FLEX_GROW|OS_CONTROL_DISABLED;
  if(axis)nodes[2].height=nodes[4].height=OS_CONTROL_FILL;
  else nodes[2].width=nodes[4].width=OS_CONTROL_FILL;
  layout(6,axis?100:extent,axis?extent:100);
  int free=extent-10-20-9,first=free/2,second=free-first;
  if(!axis){rect(1,5,45,10,10);rect(2,18,45,first,10);
   rect(4,21+first,45,second,10);rect(5,extent-15,45,10,10);}
  else {rect(1,45,5,10,10);rect(2,45,18,10,first);
   rect(4,45,21+first,10,second);rect(5,45,extent-15,10,10);}
  assert(!boxes[3].visible&&!boxes[4].enabled);
  /* 输入遍历和缓存绘制使用同一布局，不允许伸缩后点击落到旧位置。 */
  for(int p=0;p<extent;++p){
   int x=axis?50:p,y=axis?p:50;control_tree_box_t hit;
   unsigned expected=control_tree_hit_test(nodes,6,boxes,x,y);
   assert(control_tree_locate(nodes,6,(Rect){0,0,axis?100:extent,axis?extent:100},x,y,&hit)==expected);
   if(expected!=OS_CONTROL_NO_PARENT)assert(!memcmp(&hit.bounds,&boxes[expected].bounds,sizeof(Rect)));
   ++cases;
  }
 }
 /* 对齐和均分间隔包含不能整除的像素，末项不能留下尾部误差。 */
 init(4);nodes[0].layout=OS_CONTROL_ROW;nodes[0].padding=5;nodes[0].gap=3;
 for(unsigned i=1;i<4;++i){nodes[i].parent=0;nodes[i].width=10;nodes[i].height=20;}
 nodes[0].flags=OS_CONTROL_JUSTIFY_BETWEEN|OS_CONTROL_ALIGN_END;
 layout(4,101,80);rect(1,5,55,10,20);rect(2,46,55,10,20);rect(3,86,55,10,20);
 nodes[0].flags=OS_CONTROL_JUSTIFY_CENTER|OS_CONTROL_ALIGN_STRETCH;
 layout(4,101,80);rect(1,32,5,10,70);rect(3,58,5,10,70);
 nodes[0].flags=OS_CONTROL_JUSTIFY_END;layout(4,101,80);rect(3,86,5,10,20);
 /* 混合布局嵌套：父行分配宽度，子列在分配后的区域对齐。 */
 init(4);nodes[0].layout=OS_CONTROL_ROW;nodes[0].padding=5;
 nodes[1].width=20;nodes[1].layout=OS_CONTROL_COLUMN;nodes[1].flags=OS_CONTROL_FLEX_GROW|OS_CONTROL_JUSTIFY_END;
 nodes[2].width=10;nodes[2].height=20;nodes[2].flags=OS_CONTROL_FLEX_GROW;
 nodes[3].parent=0;nodes[3].width=20;
 layout(4,100,80);rect(1,5,5,70,70);rect(2,5,5,10,70);rect(3,75,5,20,70);
 nodes[0].flags=OS_CONTROL_JUSTIFY_CENTER|OS_CONTROL_JUSTIFY_END;
 assert(!control_tree_validate(nodes,4,0));
 nodes[0].flags=0;nodes[1].flags=OS_CONTROL_ALIGN_CENTER|OS_CONTROL_ALIGN_END;
 assert(!control_tree_validate(nodes,4,0));
 nodes[1].flags=0;nodes[0].layout=OS_CONTROL_ABSOLUTE;nodes[1].flags=OS_CONTROL_FLEX_GROW;
 assert(!control_tree_validate(nodes,4,0));
 nodes[1].flags=1U<<31;assert(!control_tree_validate(nodes,4,0));
 printf("Control styles/layout: deep inheritance, zero overrides, alpha, sibling isolation, immutable source, nested flex and %u input geometry cases PASS\n",cases);
}
'''
source = (root/'kernel/graphics/control_document.c').read_text()
paint = source[source.index('static inline uint32_t control_button_background('):
               source.index('void compositor_control_tree(')]
paint_program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/control_tree.h>
#include <kernel/control_selection.h>
#include <kernel/control_icons.h>
#include <kernel/control_text.h>
#include <kernel/control_raster.h>
#include <uapi/window_theme.h>
static uint32_t ascii_font_height(void){return 16;}
typedef struct { uint8_t pixels[1]; } font_t;
typedef struct { os_control_node_t *nodes; char *text; font_t *fonts[97]; control_text_run_t **runs; } document_t;
typedef struct { bool maximized;uint32_t identifier,control_key_target,control_hover,control_pressed,control_focus,control_text_target,control_text_anchor,control_text_end; } view_t;
typedef struct { document_t *document; view_t *view; int64_t x,y; } control_paint_context_t;
static struct { Rect damage_bounds;uint32_t focused_identifier; } g_compositor_snapshot;
/* 此夹具验证样式继承；图标像素由 control_widgets_test 覆盖。 */
static struct { uint32_t display_stride; uint32_t *composite_framebuffer; } g_window_server;
static uint32_t colors[16],radius,seen_font,seen_text;
static unsigned calls,alphas;
static void control_fill(int64_t x,int64_t y,uint32_t w,uint32_t h,uint32_t r,uint32_t color){
 (void)x;(void)y;assert(w&&h&&calls<16);colors[calls++]=color;radius=r;
}
static void control_fill_alpha(int64_t x,int64_t y,uint32_t w,uint32_t h,uint32_t r,uint32_t color){
 ++alphas;control_fill(x,y,w,h,r,color);
}
static void control_checkerboard(int64_t x,int64_t y,uint32_t w,uint32_t h,uint32_t r,uint32_t light,uint32_t dark){
 (void)x;(void)y;(void)w;(void)h;(void)r;(void)light;(void)dark;assert(false);
}
static void control_border(int64_t x,int64_t y,uint32_t w,uint32_t h,uint32_t r,uint32_t b,uint32_t color){
 if(b)for(unsigned i=0;i<4;++i)control_fill(x,y,w,h,r,color);
}
static void compositor_control_caption_icon(int64_t x,int64_t y,uint32_t w,uint32_t h,uint32_t a,bool m,bool active){(void)x;(void)y;(void)w;(void)h;(void)a;(void)m;(void)active;}
static void control_paint_surface(control_paint_context_t *c,const control_tree_box_t *b,Rect r){
 (void)c;(void)b;(void)r;assert(0);
}
static void control_paint_text(const os_control_node_t *node,const char *s,int64_t x,int64_t y,const uint8_t *cache){
 (void)cache;
 assert(s&&s[0]=='A'&&node->text_length==1);(void)x;(void)y;seen_font=node->font_size;seen_text=node->foreground;
}
''' + paint + r'''
int main(void){
 os_control_node_t nodes[]={
  {.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
   .flags=OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_MANAGED_FOCUS,.background=0x80123456,
   .foreground=0xabcdef,.font_size=28,.radius=7,.border_color=0x778899,.border_width=1},
  {.identifier=2,.parent=0,.kind=OS_CONTROL_CONTAINER,.flags=OS_CONTROL_INHERIT_STYLE,.width=100,.height=50},
  {.identifier=3,.parent=1,.kind=OS_CONTROL_BUTTON,.flags=OS_CONTROL_INHERIT_STYLE,.width=100,.height=50,.text_length=1}};
 os_control_node_t saved[3];memcpy(saved,nodes,sizeof(nodes));
 control_tree_box_t boxes[3];
 assert(control_tree_layout(nodes,3,1,(Rect){0,0,100,50},boxes));
 document_t document={.nodes=nodes,.text="A"};view_t view={0};
 control_paint_context_t context={.document=&document,.view=&view};
 control_paint_node(&context,nodes+2,boxes+2,(Rect){0,0,100,50});
 assert(alphas==1&&calls==5&&colors[0]==0x80123456&&colors[4]==0x778899);
 assert(seen_font==28&&seen_text==0xabcdef);
 nodes[0].background=OS_THEME_SURFACE;nodes[0].flags&=~OS_CONTROL_BACKGROUND_ALPHA;
 assert(control_tree_layout(nodes,3,1,(Rect){0,0,100,50},boxes));
 calls=alphas=0;view.control_hover=3;view.control_focus=3;
 control_paint_node(&context,nodes+2,boxes+2,(Rect){0,0,100,50});
 assert(!alphas&&calls==9&&colors[0]==OS_THEME_HOVER&&colors[8]==OS_THEME_FOCUS);
 calls=0;view.control_pressed=3;
 control_paint_node(&context,nodes+2,boxes+2,(Rect){0,0,100,50});
 assert(colors[0]==OS_THEME_SELECTED);
 nodes[1].flags|=OS_CONTROL_DISABLED;
 assert(control_tree_layout(nodes,3,1,(Rect){0,0,100,50},boxes));
 calls=0;control_paint_node(&context,nodes+2,boxes+2,(Rect){0,0,100,50});
 assert(calls==5&&colors[0]==OS_THEME_TOOLBAR&&seen_text==OS_THEME_MUTED&&seen_font==28);
 assert(!memcmp(saved+2,nodes+2,sizeof(nodes[2])));
 puts("Computed style renderer: inherited alpha/font/text/border, hover/press/focus and disabled state PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='control-style-layout-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),
                    str(root/'kernel/graphics/control_tree.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
    (path/'paint.c').write_text(paint_program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'paint.c'),
                    str(root/'kernel/graphics/control_tree.c'),'-o',str(path/'paint')],check=True)
    subprocess.run([str(path/'paint')],check=True)
