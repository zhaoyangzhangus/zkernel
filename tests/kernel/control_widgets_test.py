"""执行真实统一绘制：控件状态、进度端点、选择反馈及小尺寸裁剪。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/control_document.c').read_text()
body = source[source.index('static void control_fill('):]
body = body.replace('    if (!text || !node->text_length) return;',
    '    ++ascii_paint_calls;\n    if (!text || !node->text_length) return;',1)
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
#include <kernel/control_selection.h>
#include <kernel/control_raster.h>
#include <kernel/control_icons.h>
#include <kernel/control_text.h>
#include <uapi/window_theme.h>
#include <ascii_font.h>
static uint32_t pixels[128*96+2];
static unsigned text_width_queries,text_position_queries,ascii_paint_calls;
static struct {Rect damage_bounds;uint32_t focused_identifier;} g_compositor_snapshot;
static struct {uint32_t display_stride;uint32_t *composite_framebuffer;uint32_t display_width,display_height;}
 g_window_server={128,pixels+1,128,96};
typedef struct {uint8_t pixels[1];} font_t;
typedef struct {os_control_node_t *nodes;uint32_t count;char *text;font_t *fonts[97];
 control_text_run_t **runs;
 uint32_t text_size,layout_width,layout_height,layout_flags;bool layout_valid,has_caption;control_tree_box_t *boxes;
} window_control_document_t;
typedef struct {uint32_t control_hover,control_pressed,control_key_target,control_focus,control_text_target,control_text_anchor,control_text_end;
 bool maximized;uint32_t identifier,width,height,flags,background;int32_t x,y;window_control_document_t *controls;
} compositor_window_view_t;
static uint32_t window_client_offset_x(uint32_t flags){(void)flags;return 1;}
static uint32_t window_client_offset_y(uint32_t flags){return 1+os_window_caption_height(flags);}
typedef struct {const compositor_window_view_t *view;window_control_document_t *document;int64_t x,y;} control_paint_context_t;
static void compositor_control_caption_icon(int64_t x,int64_t y,uint32_t w,uint32_t h,uint32_t a,bool m,bool active){(void)x;(void)y;(void)w;(void)h;(void)a;(void)m;(void)active;}
static void control_paint_surface(control_paint_context_t *c,const control_tree_box_t *b,Rect d){
 (void)c;(void)b;(void)d;assert(false);
}
uint32_t ascii_font_width(void){return 8;}
uint32_t ascii_font_height(void){return 16;}
const uint8_t *ascii_font_glyph(uint8_t c){static uint8_t glyph[128];(void)c;memset(glyph,255,sizeof(glyph));return glyph;}
static void compositor_fill_rounded_locked(int32_t x,int32_t y,uint32_t w,uint32_t h,uint32_t r,uint32_t color){
 assert(!r);Rect c=g_compositor_snapshot.damage_bounds;
 int64_t right=(int64_t)x+w,bottom=(int64_t)y+h;
 for(int64_t py=y>c.y0?y:c.y0;py<bottom&&py<c.y1;++py)
  for(int64_t px=x>c.x0?x:c.x0;px<right&&px<c.x1;++px){
   assert(px>=0&&px<128&&py>=0&&py<96);pixels[1+py*128+px]=color;
  }
}
static void compositor_fill_locked(int32_t x,int32_t y,uint32_t w,uint32_t h,uint32_t color){
 compositor_fill_rounded_locked(x,y,w,h,0,color);
}
static uint32_t counted_text_width(const control_text_run_t *run,const char *text,unsigned length,unsigned font){
 ++text_width_queries;return control_selection_prepared_width(run,text,length,font);
}
static control_text_position_t counted_text_position(const os_control_node_t *node,const control_tree_box_t *box,unsigned width,unsigned font){
 ++text_position_queries;return control_text_position(node,box,width,font);
}
#define control_selection_prepared_width counted_text_width
#define control_text_position counted_text_position
'''
main = r'''
static os_control_node_t root_node={.flags=OS_CONTROL_MANAGED_FOCUS};
static window_control_document_t doc={.nodes=&root_node};
static compositor_window_view_t view;
static control_paint_context_t ctx={.view=&view,.document=&doc};
static const uint32_t sentinel=0x12345678;
static uint32_t pixel(unsigned x,unsigned y){return pixels[1+y*128+x]&0xffffff;}
static bool test_inert;
static void draw(os_control_node_t n,int x,int y,unsigned w,unsigned h,Rect damage,bool enabled){
 for(unsigned i=0;i<128*96+2;++i)pixels[i]=sentinel;
 control_tree_box_t box={.bounds={x,y,x+(int)w,y+(int)h},.clip={0,0,128,96},
  .visible=true,.enabled=enabled,.inert=test_inert,.style={.background=OS_THEME_SURFACE,.foreground=OS_THEME_TEXT,
   .border_color=OS_THEME_BORDER,.font_size=20,.radius=n.radius}};
 Rect saved={0,0,128,96};g_compositor_snapshot.damage_bounds=saved;
 text_width_queries=text_position_queries=ascii_paint_calls=0;
 control_paint_node(&ctx,&n,&box,damage);
 if(!n.text_length || !w || !h || x>=damage.x1 || y>=damage.y1 ||
    (int64_t)x+w<=damage.x0 || (int64_t)y+h<=damage.y0)
  assert(!text_width_queries&&!text_position_queries&&!ascii_paint_calls);
 assert(!memcmp(&saved,&g_compositor_snapshot.damage_bounds,sizeof(saved)));
 assert(pixels[0]==sentinel&&pixels[128*96+1]==sentinel);
 for(int py=0;py<96;++py)for(int px=0;px<128;++px)
  if(px<damage.x0||px>=damage.x1||py<damage.y0||py>=damage.y1)
   assert(pixels[1+py*128+px]==sentinel);
}
int main(void){
 /* 模态背景只阻止输入，像素必须与原外观完全一致；真正禁用仍需变灰。 */
 uint32_t original[128*96+2];
 const unsigned backdrop_kinds[]={OS_CONTROL_BUTTON,OS_CONTROL_LIST_ITEM,OS_CONTROL_TAB,
  OS_CONTROL_CHECKBOX,OS_CONTROL_RADIO,OS_CONTROL_SWITCH,OS_CONTROL_SLIDER,OS_CONTROL_SCROLLBAR};
 for(unsigned i=0;i<sizeof(backdrop_kinds)/sizeof(*backdrop_kinds);++i)
  for(unsigned checked=0;checked<2;++checked) {
   os_control_node_t background={.identifier=2,.kind=backdrop_kinds[i],
    .flags=checked?OS_CONTROL_CHECKED:0,.background=OS_THEME_FIELD,
    .foreground=OS_THEME_TEXT,.value=OS_CONTROL_VALUE_MAX/2U};
   draw(background,10,10,100,30,(Rect){0,0,128,96},true);
   memcpy(original,pixels,sizeof(original));test_inert=true;
   draw(background,10,10,100,30,(Rect){0,0,128,96},false);
   assert(!memcmp(original,pixels,sizeof(original)));test_inert=false;
  }
 os_control_node_t backdrop={.identifier=2,.kind=OS_CONTROL_BUTTON,.flags=OS_CONTROL_CHECKED};
 draw(backdrop,10,10,100,30,(Rect){0,0,128,96},true);
 memcpy(original,pixels,sizeof(original));
 draw(backdrop,10,10,100,30,(Rect){0,0,128,96},false);
 assert(memcmp(original,pixels,sizeof(original)));
 /* 弹出层必须经过真实文档桥接与真实绘制，不能用绘制替身掩盖根节点空指针。 */
 os_control_node_t menu[3]={
  {.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
   .layout=OS_CONTROL_COLUMN,.padding=6,.radius=8,.background=OS_THEME_SURFACE},
  {.identifier=2,.parent=0,.kind=OS_CONTROL_LIST_ITEM,.width=OS_CONTROL_FILL,
   .height=32,.text_length=4,.font_size=16,.background=OS_THEME_SURFACE},
  {.identifier=3,.parent=0,.kind=OS_CONTROL_LIST_ITEM,.width=OS_CONTROL_FILL,
   .height=32,.text_offset=4,.text_length=12,.font_size=16,.background=OS_THEME_SURFACE}};
 control_tree_box_t menu_boxes[3];Rect menu_view={5,6,125,90};
 assert(control_tree_layout(menu,3,16,menu_view,menu_boxes));
 for(unsigned focus=0;focus<2;++focus)for(unsigned hover=0;hover<4;++hover)
  for(unsigned pressed=0;pressed<4;++pressed){
   menu[0].flags=focus?OS_CONTROL_MANAGED_FOCUS:0;
   for(unsigned i=0;i<128*96+2;++i)pixels[i]=sentinel;
   g_compositor_snapshot.damage_bounds=(Rect){0,0,128,96};
   compositor_control_nodes(menu,3,menu_boxes,"OpenAuto arrange",hover,pressed,0);
   assert(pixels[0]==sentinel&&pixels[128*96+1]==sentinel);
   assert(pixel(10,50)!=(sentinel&0xffffff));
  }
 compositor_control_nodes(0,3,menu_boxes,0,0,0,0);
 compositor_control_nodes(menu,0,menu_boxes,0,0,0,0);
 compositor_control_nodes(menu,3,0,0,0,0,0);
 Rect full={0,0,128,96};os_control_node_t n={.identifier=2,.kind=OS_CONTROL_PROGRESS};
 /* 无文字快捷路径必须位于装饰绘制之后，不能丢失圆角、焦点框或按压背景。 */
 view.control_focus=2;
 os_control_node_t empty_button={.identifier=2,.kind=OS_CONTROL_BUTTON,.radius=8};
 draw(empty_button,10,10,100,40,full,true);
 assert(pixel(14,14)==OS_THEME_FOCUS&&pixel(40,30)==OS_THEME_SURFACE);
 assert(pixel(10,10)==(sentinel&0xffffff));
 view.control_hover=view.control_pressed=2;
 draw(empty_button,10,10,100,40,full,true);
 assert(pixel(14,14)==OS_THEME_FOCUS&&pixel(40,30)==OS_THEME_SELECTED);
 view.control_focus=view.control_hover=view.control_pressed=0;
 puts("Paint work: empty/clipped text performs zero width/position/font calls; rounded focus and pressed backgrounds retained PASS");

 /* 棋盘格按控件原点定位；负坐标、局部重画和圆角不能改变相位或越界。 */
 for(int origin=-19;origin<=17;origin+=9)
  for(unsigned radius=0;radius<=12;radius+=4)
   for(unsigned small=0;small<3;++small)for(unsigned partial=0;partial<2;++partial){
    unsigned w=small==0?0:small==1?7:83,h=small==0?0:small==1?5:71;
    Rect damage=partial?(Rect){13,11,69,58}:full;
    os_control_node_t checker={.identifier=2,.kind=OS_CONTROL_CONTAINER,
     .flags=OS_CONTROL_CHECKERBOARD,.radius=radius};
    draw(checker,origin,origin,w,h,damage,true);
    for(int py=0;py<96;++py)for(int px=0;px<128;++px){
     bool inside=px>=origin&&py>=origin&&px<origin+(int)w&&py<origin+(int)h;
     if(inside){unsigned inset=control_round_inset((unsigned)(py-origin),w,h,radius);
      inside=px>=origin+(int)inset&&px<origin+(int)w-(int)inset;}
     inside=inside&&px>=damage.x0&&px<damage.x1&&py>=damage.y0&&py<damage.y1;
     uint32_t expected=sentinel&0xffffff;
     if(inside)expected=(((px-origin)/16+(py-origin)/16)&1)?OS_THEME_TEXT:OS_THEME_SURFACE;
     assert(pixel(px,py)==(expected&0xffffff));
    }
   }
 /* 框架根的颜色不能涂掉公共标题栏，子控件仍走常规绘制。 */
 os_control_node_t frame_root={.identifier=1,.kind=OS_CONTROL_CONTAINER,.flags=OS_CONTROL_WINDOW_FRAME};
 draw(frame_root,0,0,128,96,full,true);
 for(unsigned i=0;i<128*96+2;++i)assert(pixels[i]==sentinel);
 /* 真实合成入口必须保留边框并开放完整标题栏，且标题栏局部损坏不能扩为整栏。 */
 os_control_node_t frame_nodes[]={
  {.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
   .flags=OS_CONTROL_WINDOW_FRAME,.width=-1,.height=-1,.background=0xff0000},
  {.identifier=2,.parent=0,.kind=OS_CONTROL_BUTTON,.x=4,.y=4,.width=80,.height=32,
   .background=0x112233}};
 control_tree_box_t frame_boxes[2];
 window_control_document_t frame_doc={.nodes=frame_nodes,.count=2,.boxes=frame_boxes};
 compositor_window_view_t frame_view={.width=126,.height=54,.controls=&frame_doc,
  .flags=OS_WINDOW_STANDARD_CAPTION_CONTROLS};
 for(unsigned clipped=0;clipped<2;++clipped){
  for(unsigned i=0;i<128*96+2;++i)pixels[i]=sentinel;
  g_compositor_snapshot.damage_bounds=clipped?(Rect){10,6,11,8}:full;
  compositor_control_tree(&frame_view);
  assert(frame_boxes[1].bounds.y0==-36);
  for(unsigned y=0;y<96;++y)for(unsigned x=0;x<128;++x){
   bool painted=x>=5&&x<85&&y>=5&&y<37;
   if(clipped)painted=painted&&x==10&&y>=6&&y<8;
   assert(pixel(x,y)==(painted?0x112233:sentinel&0xffffff));
  }
  assert(pixels[0]==sentinel&&pixels[128*96+1]==sentinel);
 }
 frame_view.flags|=OS_WINDOW_CAPTION_HEIGHT(80);
 compositor_control_tree(&frame_view);assert(frame_boxes[1].bounds.y0==-76);
 /* 符号按钮执行真实绘制，逐像素确认居中、主题颜色、禁用和损坏裁剪。 */
 for(uint32_t symbol=OS_CONTROL_SYMBOL_BACK;symbol<=OS_CONTROL_SYMBOL_CLOSE;++symbol)
  for(unsigned size=1;size<=40;++size)for(unsigned disabled=0;disabled<2;++disabled){
   os_control_node_t button={.identifier=2,.kind=OS_CONTROL_BUTTON,
    .flags=OS_CONTROL_SYMBOL|OS_CONTROL_BACKGROUND_ALPHA,.value=symbol};
   control_tree_box_t box={.bounds={10,10,60,60},.clip=full,.visible=true,.enabled=!disabled,
    .style={.background_alpha=1,.foreground=0x336699,.font_size=size}};
   if(size&1U){button.icon_size=size;box.style.font_size=8U;}
   Rect damage={15,14,55,57};g_compositor_snapshot.damage_bounds=full;
   for(unsigned i=0;i<128*96+2;++i)pixels[i]=sentinel;
   view.control_hover=view.control_pressed=view.control_focus=view.control_key_target=0;
   control_paint_node(&ctx,&button,&box,damage);
   int origin=10+(50-(int)size)/2;
   for(int py=0;py<96;++py)for(int px=0;px<128;++px){
    uint32_t alpha=0;
    if(px>=damage.x0&&px<damage.x1&&py>=damage.y0&&py<damage.y1&&px>=origin&&py>=origin)
     alpha=control_symbol_alpha(symbol,(uint32_t)(px-origin),(uint32_t)(py-origin),size);
    uint32_t expected=sentinel&0xffffff,foreground=disabled?OS_THEME_MUTED:0x336699;
    if(alpha){expected=0;for(unsigned shift=0;shift<24;shift+=8){
     uint32_t channel=(((sentinel>>shift)&255)*(255-alpha)+((foreground>>shift)&255)*alpha+127)/255;
     expected|=channel<<shift;}}
    assert(pixel(px,py)==expected);
   }
   assert(pixels[0]==sentinel&&pixels[128*96+1]==sentinel);
  }
 for(unsigned y=0;y<1024;y+=7)for(unsigned x=0;x<1024;x+=7){
  assert(control_icon_vector_sample(OS_CONTROL_SYMBOL_BACK,x,y)==
   control_icon_vector_sample(OS_CONTROL_SYMBOL_FORWARD,1023-x,y));
  assert(control_icon_vector_sample(OS_CONTROL_SYMBOL_SORT_ASCENDING,x,y)==
   control_icon_vector_sample(OS_CONTROL_SYMBOL_SORT_DESCENDING,x,1023-y));
 }
 assert(control_icon_vector_sample(OS_CONTROL_SYMBOL_GRID,200,200)==255);
 assert(!control_icon_vector_sample(OS_CONTROL_SYMBOL_GRID,512,512));
 assert(!control_icon_vector_sample(OS_CONTROL_SYMBOL_SEARCH,0,0));
 assert(control_icon_vector_sample(OS_CONTROL_SYMBOL_SEARCH,416,152)==255);
 assert(control_icon_vector_sample(OS_CONTROL_SYMBOL_HOME,512,128)==255);
 assert(!control_icon_vector_sample(OS_CONTROL_SYMBOL_HOME,512,500));
 assert(control_icon_vector_sample(OS_CONTROL_SYMBOL_DRIVE,288,736)==255);
 assert(!control_icon_vector_sample(OS_CONTROL_SYMBOL_DRIVE,0,0));
 assert(!control_symbol_alpha(OS_CONTROL_SYMBOL_MENU,0,0,0));
 for(unsigned vertical=0;vertical<2;++vertical)for(unsigned v=0;v<=10000;v+=100){
  n.flags=vertical?OS_CONTROL_VERTICAL:0;n.value=v;
  draw(n,10,10,vertical?20:100,vertical?60:20,full,true);
  unsigned length=vertical?60:100,filled=length*v/10000;
  for(unsigned p=0;p<length;++p){
   uint32_t actual=vertical?pixel(20,10+p):pixel(10+p,20);
   /* 端部圆角另由轮廓回归覆盖；中心线必须严格保留归一化的填充范围。 */
   assert(actual==(p<filled?OS_THEME_ACCENT:OS_THEME_BORDER));
  }
 }
 for(unsigned k=OS_CONTROL_SLIDER;k<=OS_CONTROL_SCROLLBAR;++k)for(unsigned vertical=0;vertical<2;++vertical){
  n.kind=k;n.flags=vertical?OS_CONTROL_VERTICAL:0;n.value=0;
  draw(n,10,10,vertical?20:100,vertical?60:20,full,true);
  assert((vertical?pixel(20,14):pixel(14,20))==OS_THEME_ACCENT);
  n.value=10000;draw(n,10,10,vertical?20:100,vertical?60:20,full,true);
  assert((vertical?pixel(20,65):pixel(105,20))==OS_THEME_ACCENT);
 }
 /* 可见比例决定滑块长度；绘制中心线与独立计算的轨道范围逐像素核对。 */
 const unsigned pages[]={0,1,2500,5000,10000};
 for(unsigned vertical=0;vertical<2;++vertical)for(unsigned i=0;i<5;++i){
  unsigned length=vertical?60:100,thumb=length*pages[i]/10000;
  if(thumb<12)thumb=12;
  n.kind=OS_CONTROL_SCROLLBAR;n.flags=vertical?OS_CONTROL_VERTICAL:0;
  n.page_size=pages[i];n.value=5000;
  draw(n,10,10,vertical?20:100,vertical?60:20,full,true);
  unsigned offset=(length-thumb)/2;
  for(unsigned p=0;p<length;++p){
   uint32_t actual=vertical?pixel(20,10+p):pixel(10+p,20);
   assert(actual==(p>=offset&&p<offset+thumb?OS_THEME_ACCENT:OS_THEME_BORDER));
  }
 }
 n.page_size=0;
 for(unsigned kind=OS_CONTROL_CHECKBOX;kind<=OS_CONTROL_SWITCH;++kind){
  n.kind=kind;n.flags=0;n.value=0;draw(n,10,10,100,30,full,true);
  uint32_t before[128*96+2];memcpy(before,pixels,sizeof(pixels));
  n.flags=OS_CONTROL_CHECKED;draw(n,10,10,100,30,full,true);
  assert(memcmp(before,pixels,sizeof(pixels)));
 }
 for(unsigned kind=OS_CONTROL_LIST_ITEM;kind<=OS_CONTROL_TAB;++kind){
  n.kind=kind;n.flags=OS_CONTROL_CHECKED;draw(n,10,10,100,30,full,true);
  assert(pixel(11,11)==OS_THEME_SELECTED);
  n.flags=0;view.control_hover=2;draw(n,10,10,100,30,full,true);
  assert(pixel(11,11)==OS_THEME_HOVER);
  n.flags=OS_CONTROL_NO_HOVER;draw(n,10,10,100,30,full,true);
  assert(pixel(11,11)==OS_THEME_SURFACE);
  view.control_pressed=2;draw(n,10,10,100,30,full,true);assert(pixel(11,11)==OS_THEME_SELECTED);
  view.control_pressed=0;view.control_hover=0;
  draw(n,10,10,100,30,full,false);assert(pixel(11,11)==OS_THEME_TOOLBAR);
 }
 unsigned cases=0;
 for(unsigned k=OS_CONTROL_CHECKBOX;k<=OS_CONTROL_SEPARATOR;++k)
  for(unsigned sample=0;sample<250;++sample){
   n=(os_control_node_t){.identifier=2,.kind=k,.flags=sample&1?OS_CONTROL_CHECKED:0,
    .value=sample*40};
   if((os_control_is_range(k)||k==OS_CONTROL_PROGRESS||k==OS_CONTROL_SEPARATOR)&&sample&2)
    n.flags=OS_CONTROL_VERTICAL;
   doc.text="AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";n.text_length=32;
   int x=(int)(sample%31)-15,y=(int)(sample%23)-10;
   unsigned w=sample%111,h=sample%41;
   draw(n,x,y,w,h,(Rect){3,4,117,89},sample%3!=0);
   /* 长文本和任意微小选择标记不得越过自身宽度。 */
   for(int py=4;py<89;++py)for(int px=3;px<117;++px)
    if(px<x||px>=x+(int)w||py<y||py>=y+(int)h)assert(pixels[1+py*128+px]==sentinel);
   ++cases;
  }
 /* 实际按钮绘制必须使用 Unicode 运行缓存，并按字符宽度而非字节数居中。 */
 os_control_node_t label={.identifier=2,.kind=OS_CONTROL_BUTTON,.text_length=7};
 control_text_cell_t cells[]={{16,0,0},{24,1,3},{40,0,4}};
 control_text_glyph_t glyphs[]={{0x4e2d,16,0},{'A',8,256}};
 uint8_t ink[384];memset(ink,255,sizeof(ink));
 control_text_run_t run={.count=3,.glyph_count=2,.width=40,.height=16,
  .cells=cells,.glyphs=glyphs,.pixels=ink};
 control_text_run_t *runs[]={&run};doc.nodes=&label;doc.runs=runs;doc.text="中A中";
 control_tree_box_t text_box={.bounds={10,10,90,42},.clip={0,0,128,96},.visible=true,.enabled=true,
  .style={.background=OS_THEME_SURFACE,.foreground=OS_THEME_TEXT,.font_size=16}};
 for(unsigned i=0;i<128*96+2;++i)pixels[i]=sentinel;
 g_compositor_snapshot.damage_bounds=(Rect){0,0,128,96};
 control_paint_node(&ctx,&label,&text_box,(Rect){0,0,128,96});
 for(unsigned py=10;py<42;++py)for(unsigned px=10;px<90;++px)
  assert(pixel(px,py)==(px>=30&&px<70&&py>=18&&py<34?OS_THEME_TEXT:OS_THEME_SURFACE));
 assert(pixels[0]==sentinel&&pixels[128*96+1]==sentinel);
 /* 选区在按压背景上仍须可见，并按中文字节边界绘制。 */
 assert(OS_THEME_TEXT_SELECTION!=OS_THEME_SELECTED&&OS_THEME_TEXT_SELECTION!=OS_THEME_HOVER);
 memset(ink,0,sizeof(ink));view.control_text_target=2;view.control_text_anchor=3;view.control_text_end=0;
 for(unsigned pressed=0;pressed<2;++pressed) {
  view.control_hover=view.control_pressed=pressed?2:0;
  for(unsigned i=0;i<128*96+2;++i)pixels[i]=sentinel;
  control_paint_node(&ctx,&label,&text_box,(Rect){0,0,128,96});
  for(unsigned py=10;py<42;++py)for(unsigned px=10;px<90;++px)
   assert(pixel(px,py)==(px>=30&&px<46&&py>=18&&py<34?OS_THEME_TEXT_SELECTION:
       pressed?OS_THEME_SELECTED:OS_THEME_SURFACE));
  assert(pixels[0]==sentinel&&pixels[128*96+1]==sentinel);
 }
 view.control_hover=view.control_pressed=0;
 view.control_text_target=0;
 doc.runs=0;doc.nodes=&root_node;
 printf("Common widget paint: Unicode run centering, progress centerline oracle, endpoint thumbs, selection/hover/disable, %u clipped/tiny/long-label cases PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='control-widgets-') as directory:
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root/'include'), '-x', 'c', '-',
                    str(root/'kernel/graphics/control_tree.c'), '-o', binary],
                   input=prefix+body+main, text=True, check=True)
    subprocess.run([binary], check=True)
    # 负向对照重现此次 QEMU 崩溃，证明回归实际覆盖了根节点遗漏。
    broken = body.replace('.nodes = (os_control_node_t *)nodes', '.nodes = 0')
    assert broken != body
    subprocess.run(['cc', '-std=c11', '-O2', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-I'+str(root/'include'), '-x', 'c', '-',
                    str(root/'kernel/graphics/control_tree.c'), '-o', binary],
                   input=prefix+broken+main, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, text=True)
    assert result.returncode != 0 and 'null pointer' in result.stderr
    print('Popup real paint: missing-root negative control rejected; hover/press/focus and empty input PASS')
    broken = body.replace('runs[source_node-context->document->nodes]',
                          'runs[node-context->document->nodes]')
    assert broken != body
    subprocess.run(['cc', '-std=c11', '-O2', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-I'+str(root/'include'), '-x', 'c', '-',
                    str(root/'kernel/graphics/control_tree.c'), '-o', binary],
                   input=prefix+broken+main, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, text=True)
    assert result.returncode != 0, result.stderr
    print('Unicode paint: resolved-style stack pointer index negative control rejected')

    for name,guard in {
        'empty-text-work': 'if (!node->text_length) {',
        'clipped-text-work': 'if (rect_is_empty(text_clip)) {',
    }.items():
        assert body.count(guard)==1
        broken=body.replace(guard,'if (false) {',1)
        subprocess.run(['cc','-std=c11','-O2','-fsanitize=address,undefined',
                        '-fno-pie','-no-pie','-I'+str(root/'include'),'-x','c','-',
                        str(root/'kernel/graphics/control_tree.c'),'-o',binary],
                       input=prefix+broken+main,text=True,check=True)
        result=subprocess.run([binary],capture_output=True,text=True)
        assert result.returncode!=0 and 'text_width_queries' in result.stderr,name
        print('Negative control rejected:',name)
