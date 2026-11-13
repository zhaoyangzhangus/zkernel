"""Actual compositor outer-mask ownership, all corners, damage and clipping."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/render.c').read_text()
functions = source[source.index('static void compositor_window_corners('):
                   source.index('typedef struct compositor_surface_job')]
compositor = (root / 'kernel/graphics/compositor.c').read_text()
begin = compositor.index('static void compositor_render_region_bounded(')
banding = compositor[begin:compositor.index('static void compositor_render_snapshot(', begin)]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <kernel/rect.h>
#include <uapi/window.h>
#include <uapi/window_theme.h>
#include <uapi/chrome_caption.h>
typedef struct {int32_t x,y;uint32_t width,height,flags;bool maximized;void *controls;} compositor_window_view_t;
static uint32_t pixels[64*64],initial[64*64],foreground;
static struct {volatile uint32_t *composite_framebuffer;uint32_t display_width,display_height,display_stride;} g_window_server={pixels,64,64,64};
static struct {Rect damage_bounds;uint32_t theme;} g_compositor_snapshot;
typedef struct {unsigned unused;} compositor_render_span_t;
/* 在此小型像素测试夹具中强制生成条带，不修改生产代码。 */
#define WINDOW_COMPOSITOR_ATOMIC_PIXELS 64U
static const compositor_window_view_t *render_window;
static unsigned corner_checks,pumps,control_paints;
static bool window_client_decorations(uint32_t flags){return flags&OS_WINDOW_CLIENT_DECORATIONS;}
static bool window_control_has_caption(const void *document){(void)document;return false;}
static bool window_standard_caption(uint32_t flags){return flags&OS_WINDOW_STANDARD_CAPTION_CONTROLS;}
static uint32_t window_outer_width(uint32_t width,uint32_t flags){return width+(window_client_decorations(flags)?0:2);}
static uint32_t window_outer_height(uint32_t height,uint32_t flags){return height+(window_client_decorations(flags)?0:6);}
static bool compositor_damage_inside_surface_interior(Rect clip,const compositor_window_view_t *w){(void)clip;(void)w;return false;}
static void paint(Rect clip,const compositor_window_view_t *w){
 for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
  if((int)x>=w->x&&(int)y>=w->y&&(int64_t)x<(int64_t)w->x+window_outer_width(w->width,w->flags)&&
   (int64_t)y<(int64_t)w->y+window_outer_height(w->height,w->flags)&&
   (int)x>=clip.x0&&(int)x<clip.x1&&
   (int)y>=clip.y0&&(int)y<clip.y1)pixels[y*64+x]=foreground;
}
static void compositor_titlebar_locked(Rect clip,const compositor_window_view_t *w,uint32_t color){(void)color;paint(clip,w);}
static void compositor_surface_locked(Rect clip,const compositor_window_view_t *w){paint(clip,w);}
static void compositor_control_tree(Rect clip,const compositor_window_view_t *w){assert(w->controls);++control_paints;paint(clip,w);}
static void compositor_standard_caption_locked(Rect clip,const compositor_window_view_t *w){paint(clip,w);}
/* 只统计真实绘制函数的查询，参考像素计算仍直接使用原始实现。 */
static unsigned decoration_queries,theme_queries;
static bool counted_decorations(uint32_t flags){++decoration_queries;return window_client_decorations(flags);}
static uint32_t counted_theme(uint32_t color,uint32_t theme){++theme_queries;return os_theme_resolve(color,theme);}
#define window_client_decorations counted_decorations
#define os_theme_resolve counted_theme
''' + functions.replace('uint32_t alpha = chrome_corner_coverage[y][x];', '++corner_checks; uint32_t alpha = chrome_corner_coverage[y][x];') + r'''
#undef window_client_decorations
#undef os_theme_resolve
static void compositor_region_locked(Rect clip,const compositor_render_span_t *span) {(void)clip;
 (void)span;compositor_draw_window_locked(clip,render_window);
}
static void window_server_pump_publication_motion(void) {++pumps;}
''' + banding + r'''
static unsigned oracle_alpha(unsigned x,unsigned y,unsigned w,unsigned h){
 if(x>=w||y>=h)return 0;if(w<16||h<16)return 255;
 if(x>=w/2)x=w-1-x;if(y>=h/2)y=h-1-y;
 if(x>=8||y>=8)return 255;
 unsigned covered=0;
 for(int sy=0;sy<8;++sy)for(int sx=0;sx<8;++sx){int dx=128-(int)x*16-sx*2-1,dy=128-(int)y*16-sy*2-1;covered+=dx*dx+dy*dy<=128*128;}
 return (covered*255+32)/64;
}
static uint32_t blend(uint32_t a,uint32_t b,unsigned alpha){
 uint32_t out=b&0xFF000000;
 for(unsigned shift=0;shift<24;shift+=8)out|=((((a>>shift)&255)*alpha+((b>>shift)&255)*(255-alpha)+127)/255)<<shift;
 return out;
}
static unsigned oracle_inner(unsigned x,unsigned y,unsigned w,unsigned h) {
 if(x>=w/2)x=w-1-x;if(y>=h/2)y=h-1-y;
 if(x>=8||y>=8)return 255;
 unsigned covered=0;
 for(int sy=0;sy<8;++sy)for(int sx=0;sx<8;++sx) {
  int dx=128-(int)x*16-sx*2-1,dy=128-(int)y*16-sy*2-1;
  covered+=dx*dx+dy*dy<=112*112;
 }
 return (covered*255+32)/64;
}
int main(void){
 for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)assert(chrome_corner_coverage[y][x]==oracle_alpha(x,y,32,32));
 for(unsigned round=0;round<1000;++round){
  compositor_window_view_t w={.x=(int)(round%59)-12,.y=(int)(round%43)-10,
   .width=12+round%43,.height=12+round%39,.maximized=round%7==0,
   .controls=round%3==0?(void*)1:0,
   .flags=round%2?OS_WINDOW_CLIENT_DECORATIONS|OS_WINDOW_STANDARD_CAPTION_CONTROLS:0};
  unsigned width=window_outer_width(w.width,w.flags),height=window_outer_height(w.height,w.flags);
  g_compositor_snapshot.damage_bounds.x0=round%3?round%16:0;
  g_compositor_snapshot.damage_bounds.y0=round%4?round%13:0;
  g_compositor_snapshot.damage_bounds.x1=64-round%11;
  g_compositor_snapshot.damage_bounds.y1=64-round%9;
  g_compositor_snapshot.theme=(round%3)|((round%4)<<8);
  foreground=(round*0x53917)&0xFFFFFF;
  for(unsigned i=0;i<4096;++i)pixels[i]=initial[i]=(i*0x72954)&0xFFFFFF;
  compositor_draw_window_locked(g_compositor_snapshot.damage_bounds,&w);
  uint32_t unbanded[4096];memcpy(unbanded,pixels,sizeof(pixels));
  memcpy(pixels,initial,sizeof(pixels));render_window=&w;
  compositor_render_region_bounded(g_compositor_snapshot.damage_bounds,0,true);
  assert(!memcmp(unbanded,pixels,sizeof(pixels)));
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
   uint32_t expected=initial[y*64+x];
   if((int)x>=w.x&&(int)y>=w.y&&(int64_t)x<(int64_t)w.x+width&&(int64_t)y<(int64_t)w.y+height&&
    (int)x>=g_compositor_snapshot.damage_bounds.x0&&(int)x<g_compositor_snapshot.damage_bounds.x1&&
    (int)y>=g_compositor_snapshot.damage_bounds.y0&&(int)y<g_compositor_snapshot.damage_bounds.y1){
    unsigned alpha=w.maximized?255:oracle_alpha(x-w.x,y-w.y,width,height);
    unsigned xx=x-w.x,yy=y-w.y;
    unsigned cx=xx>=width/2?width-1-xx:xx,cy=yy>=height/2?height-1-yy:yy;
    if(!w.maximized && width>=16 && height>=16 && cx<8 && cy<8 && !window_client_decorations(w.flags)) {
     unsigned inner=oracle_inner(xx,yy,width,height);
     uint32_t frame=os_theme_resolve(OS_THEME_BORDER,g_compositor_snapshot.theme),out=expected&0xff000000;
     for(unsigned shift=0;shift<24;shift+=8)
      out|=((((foreground>>shift)&255)*inner+((frame>>shift)&255)*(alpha-inner)+((expected>>shift)&255)*(255-alpha)+127)/255)<<shift;
     expected=out;
    } else expected=blend(foreground,expected,alpha);
   }
   assert(pixels[y*64+x]==expected);
  }
 }
 /* 四个完整圆角只在捕获和恢复各判断一次装饰；仅服务器边框恢复时解析一次颜色。 */
 for(unsigned client=0;client<2;++client){
  compositor_window_view_t w={.x=8,.y=8,.flags=client?OS_WINDOW_CLIENT_DECORATIONS:0};
  uint32_t background[256];Rect clip={0,0,64,64};
  decoration_queries=theme_queries=0;
  compositor_window_corners(clip,&w,32,32,background,false);
  assert(decoration_queries==1&&theme_queries==0);
  compositor_window_corners(clip,&w,32,32,background,true);
  assert(decoration_queries==2&&theme_queries==(client?0U:1U));
 }
 /* 一像素宽损坏条仅访问一个圆角的八个可见像素。 */
 {
  compositor_window_view_t w={.x=8,.y=8,.flags=OS_WINDOW_CLIENT_DECORATIONS};
  uint32_t background[256];Rect clip={8,8,9,16};
  corner_checks=0;
  compositor_window_corners(clip,&w,32,32,background,false);
  assert(corner_checks==8);
  compositor_window_corners(clip,&w,32,32,background,true);
  assert(corner_checks==16);
 }
 compositor_window_view_t interior={.x=8,.y=8,.width=48,.height=48,
  .flags=OS_WINDOW_CLIENT_DECORATIONS};
 g_compositor_snapshot.damage_bounds=(Rect){16,16,48,48};
 corner_checks=0;compositor_draw_window_locked(g_compositor_snapshot.damage_bounds,&interior);
 assert(!corner_checks&&pumps&&control_paints);
 puts("Real corner renderer + production banding: 1000 whole-vs-banded pixel comparisons; interior repaint samples zero corner pixels; one-pixel damage strip samples only 8 pixels per pass passed");
 puts("Compositor corners: exact 8x8 coverage, 1000 native/client/maximized windows, four symmetric corners, saved underlying pixels, frame drawn before final mask, offscreen/damage clipping and no app mask required passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-corner-alpha-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-I' + str(root / 'include'),
                    '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=code, text=True, check=True)
    subprocess.run([binary], check=True)
    # 同时验证重复查询与裁剪范围退化，不能只比较最终颜色。
    for label, old, new in (
        ('逐像素重复查询装饰', 'uint32_t alpha = chrome_corner_coverage[y][x];',
         '(void)window_client_decorations(window->flags);\n'
         'uint32_t alpha = chrome_corner_coverage[y][x];'),
        ('窄损坏仍遍历整行圆角',
         'for (int32_t px = visible.x0; px < visible.x1; ++px)',
         'for (int32_t px = visible.x0; px < left + 8; ++px)'),
        ('右下角未镜像', 'if ((corner & 1U) != 0U) x = 7U - x;',
         '(void)x;'),
    ):
        broken = code.replace(old, new)
        assert broken != code
        subprocess.run(command, input=broken, text=True, check=True)
        assert subprocess.run([binary], stderr=subprocess.DEVNULL,
                              stdout=subprocess.DEVNULL).returncode != 0
        print('反例被拒绝：', label)
print('圆角绘制：装饰类型每次调用只查询一次，客户端及底层捕获不解析边框颜色，逐像素重复查询反例被拒绝')
