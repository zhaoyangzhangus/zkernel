"""Production premultiplied spans and all opaque-coverage decisions."""
from pathlib import Path
import subprocess
import resource
import tempfile

root = Path(__file__).resolve().parents[2]
def source(name):
    return (root / 'kernel/graphics' / name).read_text()
render = source('render.c')
spans = render[render.index('static void compositor_surface_blend_span('):]
tile = source('tile_metadata.c')
tile = tile[tile.index('static uint32_t tile_count_for_extent('):tile.index('bool compositor_tile_metadata_build(')]
plan = source('render_plan.c')
plan = plan[plan.index('bool compositor_view_fully_covers_snapshot_bounds('):
            plan.index('static uint32_t compositor_render_plan_exact_floor(')]
program = r'''
#include <kernel/base.h>
#include <uapi/window.h>
#include <assert.h>
#include <kernel/pixel_blend.h>
#include <stdio.h>
#include <string.h>
#define WINDOW_CORNER_RADIUS 8U
#define WINDOW_DAMAGE_TILE_SIZE 32U
#define WINDOW_DAMAGE_MAX_TILES_X 8U
#define WINDOW_DAMAGE_TILE_COUNT 64U
#define WINDOW_DAMAGE_MAX_TILES_Y 8U
#define WINDOW_SERVER_MAX_WINDOWS 64U
typedef struct { uint64_t touch_mask, opaque_mask; } compositor_tile_meta_t;
#include <kernel/rect.h>
typedef struct { int32_t x,y; uint32_t width,height,flags,background,surface_width,surface_height; uint64_t buffer_size,surface_offset; bool maximized,surface_lease; } compositor_window_view_t;
typedef struct { bool readable; uint64_t row_bytes; } compositor_surface_source_context_t;
typedef struct { unsigned window_count,damage_count; compositor_window_view_t windows[2]; Rect damage_rects[16]; } compositor_snapshot_t;
static uint32_t page_data[3072], output[3072];
static int missing=-1;
static unsigned copied, resolve_calls[3];
static unsigned page_order[3]={0,1,2};
static bool window_client_decorations(uint32_t f) { return (f&OS_WINDOW_CLIENT_DECORATIONS)!=0; }
static bool window_standard_caption(uint32_t f) { return (f&(OS_WINDOW_CLIENT_DECORATIONS|OS_WINDOW_STANDARD_CAPTION_CONTROLS))==(OS_WINDOW_CLIENT_DECORATIONS|OS_WINDOW_STANDARD_CAPTION_CONTROLS); }
static unsigned window_outer_width(unsigned w,unsigned f) { (void)f; return w; }
static unsigned window_outer_height(unsigned h,unsigned f) { (void)f; return h; }
static uint8_t *compositor_surface_page_resolve(const compositor_surface_source_context_t *s,uint64_t index) {
    (void)s; assert(index<3); ++resolve_calls[index];
    return (int)index==missing ? 0 : (uint8_t*)&page_data[page_order[index]*1024];
}
static void compositor_surface_copy_destination(uint32_t *wb,volatile uint32_t *dev,const uint32_t *src,uint32_t n) {
    ++copied; for(unsigned i=0;i<n;++i) { if(wb)wb[i]=src[i]; else dev[i]=src[i]; }
}
static void compositor_surface_fill_destination(uint32_t *wb,volatile uint32_t *dev,uint32_t n,uint32_t c) {
    for(unsigned i=0;i<n;++i) { if(wb)wb[i]=c; else dev[i]=c; }
}
''' + spans + tile + plan + r'''
static void run_span(compositor_window_view_t *w,bool device,uint32_t start,uint32_t end) {
    compositor_surface_source_context_t s={.readable=true};
    uint64_t index=UINT64_MAX;uint8_t *base=0;
    compositor_copy_surface_span(w,&s,0,start,end,device?0:output,device?output:0,&index,&base);
}
int main(void) {
    compositor_window_view_t w={.width=256,.height=256,.flags=OS_WINDOW_VISIBLE|OS_WINDOW_CLIENT_DECORATIONS|OS_WINDOW_PREMULTIPLIED_ALPHA,.buffer_size=sizeof(page_data)};
    const uint32_t colors[]={0,0xffabcdef,0x80000000,0x80402010,0x00ffffff};
    const uint32_t expected[]={0x006080a0,0xffabcdef,0x00304050,0x00706060,0x006080a0};
    for(unsigned device=0;device<2;++device) {
        for(unsigned i=0;i<3072;++i) { page_data[i]=colors[i%5];output[i]=0x006080a0; }
        run_span(&w,device,1019,2051);
        for(unsigned i=0;i<1032;++i) assert(output[i]==expected[(1019+i)%5]);
        assert(output[1032]==0x006080a0);
        missing=1;for(unsigned i=0;i<3072;++i)output[i]=0x006080a0;
        run_span(&w,device,1020,2050);
        for(unsigned i=4;i<1028;++i)assert(output[i]==0x006080a0);
        missing=-1;w.buffer_size=1025*4;
        for(unsigned i=0;i<3072;++i)output[i]=0x006080a0;
        run_span(&w,device,1020,2050);
        for(unsigned i=5;i<1030;++i)assert(output[i]==0x006080a0);
        w.buffer_size=sizeof(page_data);
    }
    assert(!copied);
    /* 连续页只调用一次复制；倒序页、缺失页不能跨边界合并，每页也只解析一次。 */
    uint32_t saved_flags=w.flags;
    w.flags=0;w.background=0x334455;
    for(unsigned dev=0;dev<2;++dev)for(unsigned layout=0;layout<3;++layout) {
        for(unsigned i=0;i<3072;++i)page_data[i]=i*2654435761U;
        for(unsigned p=0;p<3;++p)page_order[p]=layout==1?2-p:p;
        missing=layout==2?1:-1;
        memset(resolve_calls,0,sizeof(resolve_calls));
        unsigned before=copied;
        run_span(&w,dev,0,3072);
        assert(copied-before==(layout==0?1U:layout==1?3U:2U));
        for(unsigned p=0;p<3;++p) {
            assert(resolve_calls[p]==1);
            for(unsigned x=0;x<1024;++x)
                assert(output[p*1024+x]==((int)p==missing?w.background:
                    page_data[page_order[p]*1024+x]));
        }
    }
    for(unsigned p=0;p<3;++p)page_order[p]=p;
    missing=-1;w.flags=saved_flags;w.background=0;

    // 类似 Chrome 的不透明内部区域走批量路径，同时保留半透明/透明边界像素，
    // 并覆盖两种目标类型。
    for(unsigned device=0;device<2;++device) {
        for(unsigned i=0;i<256;++i){page_data[i]=0xff123456;output[i]=0x006080a0;}
        page_data[0]=0x80402010;page_data[255]=0;
        unsigned before=copied;run_span(&w,device,0,256);
        assert(copied==before+1 && output[0]==0x00706060 && output[255]==0x006080a0);
        for(unsigned i=1;i<255;++i)assert(output[i]==0xff123456);
    }
    w.flags&=~OS_WINDOW_PREMULTIPLIED_ALPHA;
    run_span(&w,false,0,5);assert(copied && !memcmp(output,page_data,20));
    w.surface_offset=4096;w.buffer_size=4096;
    run_span(&w,false,0,5);assert(!memcmp(output,page_data+1024,20));
    w.surface_offset=UINT64_MAX;
    run_span(&w,false,1,5);for(unsigned i=0;i<4;++i)assert(output[i]==w.background);
    // 移动的时钟笔画：在保留的上一帧上重绘，而不是绘制到刚清空的目标缓冲区。
    // 旧的白色像素必须变为黑色。交替使用真实页偏移，并测试 WB 和设备内存目标，
    // 包括 Chrome 使用的预乘路径。
    for(unsigned alpha=0;alpha<2;++alpha)for(unsigned device=0;device<2;++device){
        w.flags=OS_WINDOW_VISIBLE|OS_WINDOW_CLIENT_DECORATIONS|
            (alpha?OS_WINDOW_PREMULTIPLIED_ALPHA:0);
        w.buffer_size=4096;
        for(unsigned i=0;i<256;++i)output[i]=0x00ffffff;
        for(unsigned frame=0;frame<1000;++frame){
            unsigned slot=frame&1U,stroke=frame%251U;
            w.surface_offset=slot*4096U;
            for(unsigned i=0;i<256;++i)
                page_data[slot*1024+i]=(alpha?0xff000000:0)|
                    ((i>=stroke&&i<stroke+5)?0xffffff:0);
            run_span(&w,device,0,256);
            for(unsigned i=0;i<256;++i)
                assert((output[i]&0xffffff)==
                    ((i>=stroke&&i<stroke+5)?0xffffff:0));
        }
    }
    w.flags=OS_WINDOW_VISIBLE|OS_WINDOW_CLIENT_DECORATIONS;
    w.surface_offset=0;w.buffer_size=sizeof(page_data);
    int64_t l,t,r,b;Rect bounds={32,32,64,64};
    assert(compositor_view_opaque_bounds(&w,&l,&t,&r,&b));
    assert(compositor_view_fully_covers_snapshot_bounds(&w,&bounds));
    compositor_snapshot_t snapshot={.window_count=2,.damage_count=16};
    snapshot.windows[0]=w;snapshot.windows[1]=w;
    snapshot.windows[1].flags|=OS_WINDOW_PREMULTIPLIED_ALPHA;
    for(unsigned i=0;i<16;++i)snapshot.damage_rects[i]=bounds;
    assert(!compositor_view_opaque_bounds(&snapshot.windows[1],&l,&t,&r,&b));
    assert(!compositor_view_fully_covers_snapshot_bounds(&snapshot.windows[1],&bounds));
    compositor_tile_meta_t metadata[64];uint32_t columns=0,rows=0;
    assert(tile_metadata_build_into(&snapshot,256,256,metadata,&columns,&rows));
    assert(metadata[9].opaque_mask==1);
    snapshot.windows[0].flags|=OS_WINDOW_PREMULTIPLIED_ALPHA;
    assert(tile_metadata_build_into(&snapshot,256,256,metadata,&columns,&rows));
    assert(!metadata[9].opaque_mask);
    puts("Alpha: WB/device, premultiplied edges, transparent RGB, page crossings/missing/tails, 4000 alternating-slot moving strokes without residual pixels, opaque fast path, tile/exact exclusion passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-alpha-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

    negative=program.replace('next == 0 || (uintptr_t)next != next_address', 'next == 0')
    assert negative != program
    (path/'test.c').write_text(negative)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    result=subprocess.run([str(path/'test')],capture_output=True,text=True,preexec_fn=no_core)
    assert result.returncode != 0 and ('Assertion' in result.stderr or 'AddressSanitizer' in result.stderr)
print('连续页合并：连续/倒序/缺失页均逐像素匹配，每页仅解析一次；错误跨物理边界合并的负面对照被拒绝')
