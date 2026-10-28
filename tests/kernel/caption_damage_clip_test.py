"""Pixel oracle for production caption clipping versus full legacy traversal."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/decorations.c').read_text()
start = source.index('void compositor_standard_caption_locked(')
body = source[start:source.index('void compositor_titlebar_locked(', start)]
reference = body.replace('compositor_standard_caption_locked(', 'reference_caption(', 1)
a = reference.index('        if ((int64_t)x + button_width <=')
b = reference.index('        for (uint32_t row = 0;', a)
reference = reference[:a] + reference[b:]
a = reference.index('        int64_t first_row =')
b = reference.index('            uint32_t cut =', a)
reference = reference[:a] + '        for (uint32_t row = 0; row < window->height; ++row) {\n' + reference[b:]
a = reference.index('        int64_t first_col =')
b = reference.index('            uint32_t cut =', a)
reference = reference[:a] + '        for (uint32_t col = 0; col < window->width; ++col) {\n' + reference[b:]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <kernel/rect.h>
#include <uapi/window.h>
#include <uapi/chrome_caption.h>
#include <uapi/window_theme.h>
typedef struct {uint32_t flags,width,height,identifier;int32_t x,y;bool maximized;} compositor_window_view_t;
static struct {Rect damage_bounds;uint32_t pointer_x,pointer_y,focused_identifier,
    title_pressed_identifier,title_pressed_button,hovered_window_identifier,hovered_title_button;} g_compositor_snapshot;
static uint32_t pixels[600*400],oracle[600*400];
static unsigned operations;
static bool window_standard_caption(uint32_t flags) {return flags!=0;}
static bool window_client_decorations(uint32_t flags) {return (flags&OS_WINDOW_CLIENT_DECORATIONS)!=0;}
static uint32_t window_outer_width(uint32_t w,uint32_t flags) {(void)flags;return w;}
static uint32_t window_outer_height(uint32_t h,uint32_t flags) {(void)flags;return h;}
static bool window_caption_button_rect(uint32_t f,int32_t x,int32_t y,
    uint32_t w,uint32_t b,int32_t *ox,int32_t *oy) {
    *ox=x+w-(4-b)*os_window_caption_button_width(f);*oy=y;return true;
}
static bool inside(int64_t x,int64_t y) {
    Rect r=g_compositor_snapshot.damage_bounds;
    return x>=0&&x<600&&y>=0&&y<400&&x>=r.x0&&x<r.x1&&y>=r.y0&&y<r.y1;
}
static void compositor_blend_pixel_locked(int32_t x,int32_t y,uint32_t color,uint32_t alpha) {
    ++operations;if(!inside(x,y))return;
    /* Order-sensitive test blend preserves repeated-write differences. */
    pixels[y*600+x]=(pixels[y*600+x]*33U)^color^alpha;
}
static void compositor_fill_locked(int32_t x,int32_t y,uint32_t w,uint32_t h,uint32_t c) {
    ++operations;
    for(uint32_t row=0;row<h;++row)for(uint32_t col=0;col<w;++col)
        if(inside((int64_t)x+col,(int64_t)y+row))pixels[(y+row)*600+x+col]=c;
}
''' + body + reference + r'''
int main(void) {
    /* 屏幕坐标落在按钮上也不能让被遮挡窗口高亮；绘制只消费场景命中。 */
    compositor_window_view_t hovered={.flags=OS_WINDOW_STANDARD_CAPTION_CONTROLS,
        .width=510,.height=350,.identifier=7,.x=20,.y=20,.maximized=true};
    g_compositor_snapshot.damage_bounds=(Rect){0,0,600,400};
    g_compositor_snapshot.focused_identifier=7;
    memset(pixels,0,sizeof(pixels));compositor_standard_caption_locked(&hovered);
    memcpy(oracle,pixels,sizeof(pixels));
    g_compositor_snapshot.pointer_x=hovered.x+hovered.width-16;
    g_compositor_snapshot.pointer_y=hovered.y+20;
    g_compositor_snapshot.hovered_window_identifier=8;
    g_compositor_snapshot.hovered_title_button=3;
    memset(pixels,0,sizeof(pixels));compositor_standard_caption_locked(&hovered);
    assert(!memcmp(oracle,pixels,sizeof(pixels)));
    g_compositor_snapshot.hovered_window_identifier=7;
    memset(pixels,0,sizeof(pixels));compositor_standard_caption_locked(&hovered);
    assert(memcmp(oracle,pixels,sizeof(pixels)));
    memcpy(oracle,pixels,sizeof(pixels));
    g_compositor_snapshot.pointer_x=g_compositor_snapshot.pointer_y=0;
    memset(pixels,0,sizeof(pixels));compositor_standard_caption_locked(&hovered);
    assert(!memcmp(oracle,pixels,sizeof(pixels)));
    g_compositor_snapshot.hovered_window_identifier=0;
    g_compositor_snapshot.hovered_title_button=0;
    /* 直接校验实际按钮底色，避免参考实现同步出错而漏掉失焦回归。 */
    for(unsigned dark=0;dark<2;++dark)
    for(unsigned client=0;client<2;++client)
    for(unsigned active=0;active<2;++active) {
        compositor_window_view_t w={.flags=OS_WINDOW_STANDARD_CAPTION_CONTROLS|
            (client?OS_WINDOW_CLIENT_DECORATIONS:0)|
            (dark?OS_WINDOW_DARK_CAPTION:0),.width=510,.height=350,
            .identifier=7,.x=20,.y=20,.maximized=true};
        g_compositor_snapshot.damage_bounds=(Rect){0,0,600,400};
        g_compositor_snapshot.focused_identifier=active?7:0;
        memset(pixels,0,sizeof(pixels));
        compositor_standard_caption_locked(&w);
        uint32_t expected=dark?0x001E1E1EU:
            (!active&&!client?OS_THEME_HEADER_INACTIVE:OS_THEME_HEADER);
        for(unsigned button=1;button<=3;++button) {
            int32_t x,y;
            assert(window_caption_button_rect(w.flags,w.x,w.y,w.width,button,&x,&y));
            assert(pixels[(y+2)*600+x+2]==expected);
        }
    }
    unsigned cases=0;uint64_t old_ops=0,new_ops=0;
    for(unsigned dark=0;dark<2;++dark)
    for(unsigned client=0;client<2;++client)
    for(unsigned active=0;active<2;++active)
    for(unsigned caption=40;caption<=128;caption+=44)
    for(unsigned pos=0;pos<3;++pos)for(unsigned band=0;band<15;++band)
    for(unsigned max=0;max<2;++max)for(unsigned hover=0;hover<2;++hover) {
        compositor_window_view_t w={.flags=OS_WINDOW_STANDARD_CAPTION_CONTROLS|
            (client?OS_WINDOW_CLIENT_DECORATIONS:0)|OS_WINDOW_CAPTION_HEIGHT(caption)|(dark?OS_WINDOW_DARK_CAPTION:0),.width=510,.height=350,.identifier=7,
            .x=pos==0?-50:pos==1?20:550,.y=pos==0?-20:20,.maximized=max};
        g_compositor_snapshot.damage_bounds=(Rect){0,band*30,600,band*30+30};
        if(band==14)g_compositor_snapshot.damage_bounds=(Rect){0,0,600,400};
        g_compositor_snapshot.pointer_x=hover?w.x+w.width-16:0;
        g_compositor_snapshot.pointer_y=hover?w.y+20:0;
        g_compositor_snapshot.hovered_window_identifier=hover?7:0;
        g_compositor_snapshot.hovered_title_button=hover?3:0;
        g_compositor_snapshot.focused_identifier=active?7:0;
        g_compositor_snapshot.title_pressed_identifier=hover?7:0;
        g_compositor_snapshot.title_pressed_button=3;
        for(unsigned i=0;i<600*400;++i)pixels[i]=i+123;
        operations=0;reference_caption(&w);old_ops+=operations;
        memcpy(oracle,pixels,sizeof(pixels));
        for(unsigned i=0;i<600*400;++i)pixels[i]=i+123;
        operations=0;compositor_standard_caption_locked(&w);new_ops+=operations;
        assert(!memcmp(oracle,pixels,sizeof(pixels)));++cases;
    }
    assert(new_ops<old_ops/3);
    printf("Caption clipping: %u pixel-equivalent cases, full/banded/offscreen/maximized/hover; draw calls %llu -> %llu\n",
        cases,(unsigned long long)old_ops,(unsigned long long)new_ops);
}
'''
with tempfile.TemporaryDirectory(prefix='caption-damage-clip-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
    # 负向对照：恢复旧底色必须触发像素断言。
    broken = program.replace(
        '(!active && !window_client_decorations(window->flags) ?\n'
        '            OS_THEME_HEADER_INACTIVE : OS_THEME_HEADER)',
        'OS_THEME_HEADER')
    assert broken != program
    subprocess.run(['cc', '-std=gnu11', '-O2', '-I' + str(root / 'include'),
                    '-x', 'c', '-', '-o', binary], input=broken, text=True, check=True)
    result = subprocess.run([binary], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode != 0, '旧版失焦底色未触发回归断言'
    print('Caption focus: active/inactive, client/server, light/dark pixels and negative control passed')
