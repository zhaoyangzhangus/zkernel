"""Run production render banding; pump only at fully layered boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/compositor.c').read_text()
begin = source.index('static void compositor_render_region_bounded(')
body = source[begin:source.index('void compositor_render_snapshot(', begin)]
render = source[source.index('void compositor_render_snapshot('):]
assert 'compositor_render_region_bounded(span, service_motion)' in render
assert 'compositor_render_region_bounded(0, service_motion)' in render
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/rect.h>
#define WINDOW_COMPOSITOR_ATOMIC_PIXELS (128U*1024U)
typedef struct {unsigned mask;} compositor_render_span_t;
static struct {Rect damage_bounds;} g_compositor_snapshot;
static uint32_t pixels[2100*300],visits[2100*300];
static unsigned calls,pumps,finished;
static uint32_t last_bottom;
static const compositor_render_span_t *expected;
static Rect original;
static void compositor_region_locked(const compositor_render_span_t *span) {
    assert(span==expected);++calls;
    Rect r=g_compositor_snapshot.damage_bounds;
    assert(r.x0==original.x0&&r.x1==original.x1);
    assert(r.y0==(int)last_bottom&&r.y1<=original.y1);
    for(unsigned layer=0;layer<3;++layer)
        for(int y=r.y0;y<r.y1;++y)for(int x=r.x0;x<r.x1;++x) {
            unsigned i=y*2100+x;
            if(!layer){assert(!visits[i]);++visits[i];}
            pixels[i]=layer+1;
        }
    last_bottom=r.y1;finished=1;
}
static void window_server_pump_publication_motion(void) {
    assert(finished);finished=0;++pumps;
    /* A moving cursor may restore any pixel: touched pixels must be fully
     * layered, all others must still be the previous clean retained scene. */
    for(unsigned i=0;i<2100*300;++i)assert(pixels[i]==(visits[i]?3:17));
}
''' + body + r'''
static void run(unsigned width,unsigned height,bool service,bool planned) {
    compositor_render_span_t span={7};expected=planned?&span:0;
    original=(Rect){3,5,3+(int)width,5+(int)height};
    g_compositor_snapshot.damage_bounds=original;
    memset(visits,0,sizeof(visits));
    for(unsigned i=0;i<2100*300;++i)pixels[i]=17;
    calls=pumps=finished=0;last_bottom=original.y0;
    compositor_render_region_bounded(expected,service);
    assert(!memcmp(&original,&g_compositor_snapshot.damage_bounds,sizeof(original)));
    bool split=service&&(uint64_t)width*height>WINDOW_COMPOSITOR_ATOMIC_PIXELS;
    assert(calls>=1&&pumps==(service&&width&&height?calls:0));
    if(!split)assert(calls==1);
    for(unsigned y=0;y<300;++y)for(unsigned x=0;x<2100;++x) {
        bool inside=x>=3&&x<3+width&&y>=5&&y<5+height;
        assert(visits[y*2100+x]==inside);
        assert(pixels[y*2100+x]==(inside?3:17));
    }
}
/* 一个快照包含许多小矩形时，每个完整区域之后都必须推进输入消费。 */
static void fragmented(void) {
    compositor_render_span_t span={7};
    memset(visits,0,sizeof(visits));
    for(unsigned i=0;i<2100*300;++i)pixels[i]=17;
    calls=pumps=finished=0;
    for(unsigned i=0;i<64;++i){
        original=(Rect){3,5+(int)i*4,67,7+(int)i*4};
        g_compositor_snapshot.damage_bounds=original;
        last_bottom=original.y0;expected=i%2?&span:0;
        compositor_render_region_bounded(expected,true);
        assert(calls==i+1&&pumps==i+1&&!finished);
        assert(!memcmp(&original,&g_compositor_snapshot.damage_bounds,sizeof(original)));
    }
}
int main(void) {
    const unsigned widths[]={1,64,945,1920,2048};
    const unsigned heights[]={0,1,31,32,33,64,200,290};
    for(unsigned w=0;w<5;++w)for(unsigned h=0;h<8;++h)
        for(unsigned service=0;service<2;++service)
            for(unsigned plan=0;plan<2;++plan)run(widths[w],heights[h],service,plan);
    fragmented();
    puts("Render motion: 160 planned/fallback/atomic/banded cases + 64 fragmented regions; input at every complete region, exact pixels and bounds passed");
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-render-motion-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
    # 回退到仅大矩形中间条带消费输入的实现，必须被同一组行为断言拒绝。
    broken = program.replace('if (service_motion && width != 0U && height != 0U)',
                             'if (false)')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=broken, text=True, check=True)
    assert subprocess.run([binary], stderr=subprocess.DEVNULL).returncode != 0
    print('负面对照通过：小区域不服务输入时，碎片化重绘断言拒绝该实现')
