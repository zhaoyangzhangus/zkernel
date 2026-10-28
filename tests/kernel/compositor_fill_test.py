"""真实填充汇编与逐像素裁剪公式对照，覆盖连续行合并和行尾保护。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/raster.c').read_text()
body = source[source.index('void compositor_fill_span_wb('):]
body = body.replace('    uint64_t pattern;', '    ++spans;\n    uint64_t pattern;', 1)
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <kernel/rect.h>
#include <uapi/chrome_caption.h>
#include <uapi/window_theme.h>
#define WINDOW_CORNER_RADIUS 8U
static unsigned spans;
static struct { uint32_t display_width,display_height,display_stride;
    volatile uint32_t *composite_framebuffer; } g_window_server;
static struct { Rect damage_bounds; uint32_t theme; } g_compositor_snapshot;
static uint32_t compositor_corner_inset(uint32_t row,uint32_t w,uint32_t h) {
    return chrome_frame_row_inset(row,w,h);
}
void compositor_fill_rounded_locked(int32_t x,int32_t y,uint32_t w,uint32_t h,
    uint32_t radius,uint32_t color);
'''
main = r'''
int main(void) {
    uint32_t actual[4096],expected[4096];unsigned cases=0;
    for(unsigned sample=0;sample<16000;++sample) {
        unsigned w=1+sample%31,h=1+sample%23,stride=w+sample%8,start=1+(sample/8)%2;
        for(unsigned i=0;i<4096;++i)actual[i]=expected[i]=i*2654435761U;
        g_window_server.display_width=w;g_window_server.display_height=h;
        g_window_server.display_stride=stride;g_window_server.composite_framebuffer=actual+start;
        int32_t x=(int)(sample%63)-20,y=(int)(sample%47)-15;
        uint32_t width=sample%73,height=sample%53;
        if(sample%103==0){x=INT32_MIN;width=UINT32_MAX;}
        if(sample%107==0){y=INT32_MIN;height=UINT32_MAX;}
        if(sample%109==0)x=INT32_MAX;
        if(sample%113==0)y=INT32_MAX;
        uint32_t radius=sample%3==0?0:sample%3==1?8:2,color=sample*1103515245U;
        Rect clip=sample%4?(Rect){0,0,(int32_t)w,(int32_t)h}:
            (Rect){(int32_t)(w/3),(int32_t)(h/3),(int32_t)(w*2/3),(int32_t)(h*2/3)};
        g_compositor_snapshot.damage_bounds=clip;
        compositor_fill_rounded_locked(x,y,width,height,radius,color);
        for(unsigned py=0;py<h;++py)for(unsigned px=0;px<w;++px) {
            if((int64_t)px<x||(int64_t)py<y||(int64_t)px>=(int64_t)x+width||
                (int64_t)py>=(int64_t)y+height||(int32_t)px<clip.x0||(int32_t)py<clip.y0||
                (int32_t)px>=clip.x1||(int32_t)py>=clip.y1)continue;
            uint32_t inset=radius==8?chrome_frame_row_inset((uint32_t)((int64_t)py-y),width,height):0;
            if((int64_t)px>=(int64_t)x+inset&&(int64_t)px<(int64_t)x+width-inset)
                expected[start+py*stride+px]=color;
        }
        assert(!memcmp(actual,expected,sizeof(actual)));++cases;
    }
    /* 整个 4K 目标带偏移和护栏，验证真实 rep stosq 的对齐、尾部与整块写入。 */
    const unsigned width=3840,height=2160,pixels=width*height;
    uint32_t *frame=malloc(((size_t)pixels+2)*sizeof(*frame));assert(frame);
    for(unsigned i=0;i<pixels+2;++i)frame[i]=0x12345678;
    g_window_server.display_width=g_window_server.display_stride=width;
    g_window_server.display_height=height;g_window_server.composite_framebuffer=frame+1;
    g_compositor_snapshot.damage_bounds=(Rect){0,0,width,height};spans=0;
    compositor_fill_locked(0,0,width,height,0xabcdefff);
    assert(spans==1&&frame[0]==0x12345678&&frame[pixels+1]==0x12345678);
    for(unsigned i=1;i<=pixels;++i)assert(frame[i]==0xabcdefff);
    /* 纵向局部损坏仍连续；横向局部损坏必须保留行间隔。 */
    spans=0;g_compositor_snapshot.damage_bounds=(Rect){0,100,width,200};
    compositor_fill_locked(0,0,width,height,0x12345678);assert(spans==1);
    assert(frame[1+99*width]==0xabcdefff&&frame[1+100*width]==0x12345678&&frame[1+200*width]==0xabcdefff);
    spans=0;g_compositor_snapshot.damage_bounds=(Rect){1,100,width-1,200};
    compositor_fill_locked(0,0,width,height,0x99887766);assert(spans==100);
    assert(frame[1+100*width]==0x12345678&&frame[2+100*width]==0x99887766&&frame[100*width+width]==0x12345678);
    free(frame);
    printf("Compositor fill: %u pixel-oracle rectangles, stride padding, clipping, corners, extreme origins; real 4K fill 1 span instead of 2160 PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-fill-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(prefix+body+main)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),
        str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
