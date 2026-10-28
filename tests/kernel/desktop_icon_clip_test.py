"""Compile production icon cache/render paths and count actual pixel visits."""
from pathlib import Path
import subprocess
import tempfile
import resource
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/shell.c').read_text()
# 缓存槽数由实际应用资源表决定，宿主夹具同步使用生产声明，而非过时的独立图集模型。
assets = (root/'kernel/graphics/assets.c').read_text()
asset_start = assets.index('desktop_asset_image_t g_desktop_app_assets[')
asset_declaration = assets[asset_start:assets.index(';',asset_start)+1]
functions = source[source.index('typedef struct desktop_icon_clip {'):
                   source.index('static bool desktop_draw_wallpaper_asset_locked(')]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#define DESKTOP_ICON_IMAGE_WIDTH 64U
#define DESKTOP_ICON_IMAGE_HEIGHT 64U
typedef struct {int32_t x0,y0,x1,y1;} Rect;
typedef struct {Rect bounds;uint32_t target_width,target_height;} desktop_render_context_t;
typedef struct {const uint8_t *pixels;uint32_t stride,width,height;} desktop_asset_image_t;
static struct {uint32_t display_width,display_height;} g_window_server={128,128};
static uint32_t output[128*128];
static unsigned visits,filtered;
static void desktop_blend_asset_pixel_locked(const desktop_render_context_t *c,
 int32_t x,int32_t y,uint32_t p) {
 assert(x>=0 && y>=0 && x<128 && y<128);
 assert(x>=c->bounds.x0 && x<c->bounds.x1);
 assert(y>=c->bounds.y0 && y<c->bounds.y1);
 ++visits;output[y*128+x]=p;
}
static uint32_t desktop_icon_area_pixel(const uint8_t *p,uint32_t s,
 uint32_t l,uint32_t t,uint32_t sw,uint32_t sh,uint32_t w,uint32_t h,uint32_t x,uint32_t y) {
 (void)p;(void)s;(void)l;(void)t;(void)sw;(void)sh;(void)h;++filtered;return y*w+x;
}
''' + asset_declaration + functions + r'''
int main(void) {
 desktop_render_context_t c={.bounds={70,70,80,80}};
 uint8_t p[16]={0};desktop_asset_image_t a={.pixels=p,.stride=8,.width=2,.height=2};
 desktop_draw_asset_icon_locked(&c,0,0,64,64,&a,0,0,2,2);assert(!filtered && !visits);
 c.bounds=(Rect){12,13,14,16};
 desktop_draw_asset_icon_locked(&c,10,10,64,64,&a,0,0,2,2);
 assert(filtered==4096 && visits==6 && output[13*128+12]==3*64+2);
 visits=0;desktop_draw_asset_icon_locked(&c,10,10,64,64,&a,0,0,2,2);
 assert(filtered==4096 && visits==6);
 visits=0;c.bounds=(Rect){0,0,4,3};
 desktop_draw_asset_icon_locked(&c,-10,-5,64,64,&a,0,0,2,2);
 assert(filtered==4096 && visits==12 && output[0]==5*64+10);
 desktop_icon_clip_t clip;
 assert(!desktop_icon_clip_locked(0,0,0,64,64,&clip));
 assert(!desktop_icon_clip_locked(&c,INT32_MIN,INT32_MIN,64,64,&clip));
 assert(!desktop_icon_clip_locked(&c,INT32_MAX,INT32_MAX,64,64,&clip));
 c.bounds=(Rect){-20,-20,4,3};visits=0;
 desktop_draw_asset_icon_locked(&c,-10,-5,64,64,&a,0,0,2,2);assert(visits==12);
 c.target_width=2;c.target_height=2;visits=0;
 desktop_draw_asset_icon_locked(&c,0,0,64,64,&a,0,0,2,2);assert(visits==4);
 c.target_width=c.target_height=0;
 c.bounds=(Rect){70,70,80,80};visits=0;
 desktop_draw_asset_icon_locked(&c,0,0,64,64,&a,0,0,2,2);
 assert(filtered==4096 && !visits);
 c.bounds=(Rect){12,13,14,16};
 desktop_draw_asset_icon_locked(&c,10,10,64,64,&a,0,0,2,2);
 assert(filtered==4096 && visits==6 && output[13*128+12]==3*64+2);
 visits=0;desktop_draw_asset_icon_locked(&c,10,10,64,64,&a,0,0,2,2);
 assert(filtered==4096 && visits==6);
 visits=0;unsigned before=filtered;
 desktop_draw_asset_icon_locked(&c,10,10,32,32,&a,0,0,2,2);
 assert(filtered==before+6 && visits==6); // uncached sizes clip before filtering
 visits=0;desktop_draw_asset_icon_locked(&c,10,10,64,64,&a,1,0,2,2);
 assert(!visits); // reject invalid source crop
 c.bounds=(Rect){0,0,128,128};visits=0;
 desktop_draw_asset_icon_locked(&c,10,10,64,64,&a,0,0,2,2);assert(visits==4096);
 for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
  assert(output[(y+10)*128+x+10]==y*64+x);
 /* 每个应用均应拥有缓存槽；填满后复用不得重新过滤，额外图稿只处理损坏范围。 */
 enum {slots=sizeof(g_desktop_app_assets)/sizeof(g_desktop_app_assets[0])-1U};
 static uint8_t bank[slots][16];
 c.bounds=(Rect){12,13,14,16};
 before=filtered;
 for(unsigned i=0;i<slots-1U;++i){
  a.pixels=bank[i];visits=0;
  desktop_draw_asset_icon_locked(&c,10,10,64,64,&a,0,0,2,2);
  assert(visits==6&&filtered==before+(i+1U)*4096U);
 }
 before=filtered;
 for(unsigned i=0;i<slots-1U;++i){
  a.pixels=bank[i];visits=0;
  desktop_draw_asset_icon_locked(&c,10,10,64,64,&a,0,0,2,2);
  assert(visits==6&&filtered==before);
 }
 a.pixels=bank[slots-1U];visits=0;
 desktop_draw_asset_icon_locked(&c,10,10,64,64,&a,0,0,2,2);
 assert(visits==6&&filtered==before+6);
 a.pixels=p;before=filtered;
 desktop_draw_asset_icon_locked(&c,10,10,64,64,&a,0,0,2,2);
 assert(filtered==before);
}
'''
with tempfile.TemporaryDirectory(prefix='desktop-icon-clip-') as directory:
    path=Path(directory); (path/'test.c').write_text(program)
    command=['cc','-std=c11','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(path/'test')],check=True)
    capacity='cache[sizeof(g_desktop_app_assets)/sizeof(g_desktop_app_assets[0])-1U]'
    assert capacity in program
    (path/'test.c').write_text(program.replace(capacity,'cache[1]'))
    subprocess.run(command,check=True)
    result=subprocess.run([str(path/'test')],capture_output=True,text=True,
        preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode!=0 and 'Assertion' in result.stderr,result.stderr
    print('Negative control rejected: cache omits application icon slots')
print('Desktop icon clipping: 2x3 damage visits 6 instead of 4096 pixels; invisible icons do no filtering; cache reuse and full-frame pixels PASS')
