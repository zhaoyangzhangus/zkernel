"""真实桌面 UI 的布局、命中、损坏和绘制共用控件树；用独立几何公式与双路径绘制对照。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/shell.c').read_text()
geometry = source[source.index('static Rect desktop_viewport('):source.index('static void desktop_blend_asset_pixel_locked(')]
hit = source[source.index('uint32_t desktop_app_at_locked('):source.index('void desktop_set_hovered_app_locked(')]
paint = source[source.index('void desktop_mark_icon_box_locked('):source.index('/*\n * 桌面保留后备表面。')]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
#include <kernel/desktop_interaction.h>
#include "user/runtime/control_file.h"
#define DESKTOP_APP_NONE 0
#define DESKTOP_ICON_IMAGE_WIDTH 64U
#define DESKTOP_ICON_IMAGE_HEIGHT 64U
typedef struct {Rect bounds;uint32_t hovered_app;bool wallpaper_only;} desktop_render_context_t;
typedef struct {const void *pixels;uint32_t width,height;} desktop_asset_image_t;
typedef struct {uint32_t node_index;} desktop_app_binding_t;
static desktop_app_binding_t g_desktop_programs[8];
static desktop_asset_image_t g_desktop_app_assets[8],g_desktop_wallpaper_asset={(void*)1,1,1};
static struct {uint32_t display_width,display_height;void *composite_framebuffer;desktop_interaction_t desktop;struct {const os_control_node_t *controls;uint32_t control_count;} scene;} g_window_server;
static bool available=true;
static bool desktop_shell_assets_available(void){return available;}
static unsigned draws,hovers,marks,wallpapers;
static Rect images[8],clips[8],marked,hover_rect;
static void window_mark_rect_locked(int32_t x,int32_t y,uint32_t w,uint32_t h){
 ++marks;marked=(Rect){x,y,x+(int32_t)w,y+(int32_t)h};
}
static void desktop_draw_hover_frame_locked(int32_t x,int32_t y,uint32_t w,uint32_t h,const desktop_render_context_t *c,const control_tree_style_t *style){
 (void)c;assert(style->radius==8&&style->border_width==2&&style->border_color==0x661a73e8U);
 ++hovers;hover_rect=(Rect){x,y,x+(int32_t)w,y+(int32_t)h};
}
static void desktop_draw_asset_icon_locked(const desktop_render_context_t *c,int32_t x,int32_t y,
 uint32_t w,uint32_t h,const desktop_asset_image_t *a,uint32_t l,uint32_t t,uint32_t sw,uint32_t sh){
 unsigned app=(unsigned)(a-g_desktop_app_assets);assert(app>=1&&app<=7);
 assert(!l&&!t&&sw==256&&sh==256);++draws;images[app]=(Rect){x,y,x+(int32_t)w,y+(int32_t)h};clips[app]=c->bounds;
}
static bool desktop_draw_wallpaper_asset_locked(const desktop_render_context_t *c){
 assert(c);++wallpapers;return true;
}
static void desktop_draw_running_locked(const desktop_render_context_t *c,unsigned app,const control_tree_box_t *b){(void)c;(void)app;(void)b;}
''' + geometry + hit + paint + r'''
typedef struct {uint32_t id;Rect bounds,clip,damage;uint32_t foreground,font;} record_t;
typedef struct {record_t records[32];unsigned count;} trace_t;
static void trace_paint(void *raw,const os_control_node_t *n,const control_tree_box_t *b,Rect damage){
 trace_t *t=raw;assert(t->count<32);
 t->records[t->count++]=(record_t){n->identifier,b->bounds,b->clip,damage,b->style.foreground,b->style.font_size};
}
static bool equal(Rect a,Rect b){return a.x0==b.x0&&a.y0==b.y0&&a.x1==b.x1&&a.y1==b.y1;}
static unsigned oracle(uint32_t w,uint32_t h,uint32_t x,uint32_t y){
 if(w<712||h<134)return 0;
 for(unsigned i=0;i<7;++i){int32_t left=(int32_t)((w-696)/2)+16+(int32_t)i*96;
  int32_t top=(int32_t)h-94;
  if(x>=(uint32_t)left&&x<(uint32_t)(left+88)&&y>=(uint32_t)top&&y<(uint32_t)(top+68))return i+1;
 }return 0;
}
int main(int argc,char **argv){
 assert(argc==2);ui_file_document_t d={0};ui_file_error_t e;
 assert(ui_file_load(argv[1],&d,&e));assert(d.request.count==9&&d.resource_count==7);
 for(unsigned i=0;i<7;++i){unsigned app=i+1,index=d.resources[i].node_index;
  d.nodes[index].user_data=app;g_desktop_programs[app].node_index=index;
  g_desktop_app_assets[app]=(desktop_asset_image_t){(void*)1,256,256};
 }
 g_window_server.scene.controls=d.nodes;g_window_server.scene.control_count=d.request.count;
 unsigned sizes[][2]={{712,134},{800,600},{1920,1080},{1921,1081},{3840,2160}};
 unsigned state=7;
 for(unsigned s=0;s<5;++s){
  unsigned w=sizes[s][0],h=sizes[s][1];g_window_server.display_width=w;g_window_server.display_height=h;
  g_window_server.composite_framebuffer=(void*)1;
  for(unsigned app=1;app<=7;++app){control_tree_box_t b;assert(desktop_icon_box_locked(app,&b));
   Rect expected={(int32_t)((w-696)/2+16+(app-1)*96),(int32_t)h-94,
    (int32_t)((w-696)/2+16+(app-1)*96+88),(int32_t)h-26};
   assert(equal(b.bounds,expected));marks=0;desktop_mark_app_locked(app);assert(marks==1&&equal(marked,expected));
   assert(desktop_app_at_locked((uint32_t)expected.x0,(uint32_t)expected.y0)==app);
   assert(desktop_app_at_locked((uint32_t)expected.x1-1,(uint32_t)expected.y1-1)==app);
   assert(desktop_app_at_locked((uint32_t)expected.x1,(uint32_t)expected.y1)==0);
  }
  for(unsigned i=0;i<2000;++i){state=state*1664525U+1013904223U;unsigned x=state%w;
   state=state*1664525U+1013904223U;unsigned y=state%h;
   assert(desktop_app_at_locked(x,y)==oracle(w,h,x,y));
  }
  draws=hovers=wallpapers=0;desktop_render_context_t c={.bounds={0,0,(int32_t)w,(int32_t)h},.hovered_app=3};
  assert(desktop_render_assets_locked(&c)&&draws==7&&hovers==1&&wallpapers==1);
  for(unsigned app=1;app<=7;++app){control_tree_box_t b;assert(desktop_icon_box_locked(app,&b));
   assert(equal(images[app],(Rect){b.bounds.x0+12,b.bounds.y0+2,b.bounds.x0+76,b.bounds.y0+66}));
  }
  c.bounds=(Rect){images[3].x0+20,images[3].y0+20,images[3].x0+22,images[3].y0+23};draws=hovers=0;
  assert(desktop_render_assets_locked(&c)&&draws==1&&hovers==1&&equal(clips[3],c.bounds));
 }
 /* 文件中的间距、内边距和按钮尺寸变化后，命中和绘制一起变化。 */
 d.nodes[0].padding=24;d.nodes[1].gap=20;
 for(unsigned i=2;i<9;++i){d.nodes[i].width=48;d.nodes[i].height=40;}
 g_window_server.display_width=800;g_window_server.display_height=600;
 for(unsigned app=1;app<=7;++app){control_tree_box_t b;assert(desktop_icon_box_locked(app,&b));
  int32_t x=(800-(7*48+6*20))/2+(int32_t)(app-1)*68;
  assert(equal(b.bounds,(Rect){x,514,x+48,554}));assert(desktop_app_at_locked((uint32_t)x,514)==app);
 }
 desktop_render_context_t c={.bounds={0,0,800,600},.hovered_app=1};draws=0;
 assert(desktop_render_assets_locked(&c)&&draws==7&&equal(images[1],(Rect){172,514,220,554}));
 /* 缓存布局绘制与直接遍历的节点、裁剪、顺序和继承样式必须完全一致。 */
 for(unsigned sample=0;sample<1000;++sample){
  state=state*1664525U+1013904223U;unsigned width=1+(state%2100);
  state=state*1664525U+1013904223U;unsigned height=1+(state%1200);
  Rect viewport={0,0,(int32_t)width,(int32_t)height},damage={10,7,(int32_t)width-3,(int32_t)height-5};
  d.nodes[2].flags=(d.nodes[2].flags&~OS_CONTROL_HIDDEN)|(sample%2?OS_CONTROL_HIDDEN:0);
  d.nodes[3].flags=(d.nodes[3].flags&~OS_CONTROL_DISABLED)|(sample%3?OS_CONTROL_DISABLED:0);
  d.nodes[1].scroll_x=(int32_t)(sample%101);control_tree_box_t boxes[9];
  assert(control_tree_layout(d.nodes,9,d.request.text_size,viewport,boxes));trace_t a={0},b={0};
  control_tree_paint(d.nodes,9,boxes,damage,trace_paint,&a);
  assert(control_tree_paint_view(d.nodes,9,viewport,damage,trace_paint,&b));assert(a.count==b.count);
  for(unsigned i=0;i<a.count;++i){record_t *x=a.records+i,*y=b.records+i;
   assert(x->id==y->id&&equal(x->bounds,y->bounds)&&equal(x->clip,y->clip)&&equal(x->damage,y->damage)&&x->foreground==y->foreground&&x->font==y->font);
  }
 }
 d.nodes[1].scroll_x=0;d.nodes[2].flags|=OS_CONTROL_HIDDEN;d.nodes[3].flags|=OS_CONTROL_DISABLED;
 control_tree_box_t disabled;assert(desktop_icon_box_locked(2,&disabled)&&!disabled.enabled);
 assert(!desktop_app_at_locked((uint32_t)disabled.bounds.x0,(uint32_t)disabled.bounds.y0));
 marks=0;desktop_mark_app_locked(1);assert(!marks);draws=0;
 assert(desktop_render_assets_locked(&c)&&draws==6);
 available=false;assert(!desktop_app_at_locked(220,520));marks=0;desktop_mark_app_locked(1);assert(!marks);
 assert(!desktop_icon_box_locked(UINT32_MAX,0));assert(!desktop_app_at_locked(UINT32_MAX,UINT32_MAX));
 ui_file_destroy(&d);
 puts("Desktop control scene: real UI, 5 independent geometry oracles, 10000 hit samples, clipped paint/damage, edited UI, 1000 cached/streaming paint comparisons PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='desktop-control-scene-') as directory:
    work = Path(directory)
    def compile_source(actual):
        (work/'test.c').write_text(actual)
        subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I'+str(root),'-I'+str(root/'include'),str(work/'test.c'),
                        str(root/'user/runtime/control_file.c'),str(root/'kernel/graphics/control_tree.c'),
                        '-o',str(work/'test')],check=True)
    compile_source(code)
    subprocess.run([str(work/'test'),str(root/'user/ui/desktop.ui')],check=True)
    assert 'context.bounds = damage;' in code
    compile_source(code.replace('context.bounds = damage;', '(void)damage; context.bounds = box->bounds;'))
    failed = subprocess.run([str(work/'test'),str(root/'user/ui/desktop.ui')],capture_output=True,text=True)
    assert failed.returncode != 0 and 'Assertion' in failed.stderr
    print('Negative control rejected: repainting the whole icon instead of the submitted damage')
