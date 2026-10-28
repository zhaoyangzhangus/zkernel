"""Run production publication/policy with a fixed-page cursor and CPU yields."""
from pathlib import Path
import subprocess
import resource
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/publication_policy.c').read_text()
start = source.index('typedef struct compositor_publication_budget')
end = source.index('\n/*\n * 从已完成的保留 WB 场景', start)
body = source[start:end].replace('__asm__ volatile ("sfence" : : : "memory");', 'fence();')
commit = (root/'kernel/graphics/compositor.c').read_text()
policy_start = commit.index('    allow_yield = ')
policy = commit[policy_start:commit.index(';', policy_start)+1]
assert 'compositor_publication_rects(g_compositor_snapshot.damage_rects,' in commit
assert 'qemu_stdvga' not in policy, 'Fixed-page ordinary publication must be bounded too'
assert source.index('"sfence"', start) < source.index('sched_preempt_enable();', start)
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define WINDOW_COMPOSITOR_PUBLICATION_YIELD_PIXELS (128U * 1024U)
#define WINDOW_COMPOSITOR_ATOMIC_PIXELS (128U * 1024U)
#include <kernel/rect.h>
static struct { uint32_t *framebuffer, *composite_framebuffer; uint32_t display_stride,display_width,display_height; } g_window_server;
static unsigned preempt=1, yields, fences, rows, cursor_rows, motion_pumps;
static uint64_t ticks,row_ticks;
static bool resched=true,wake_on_motion;
static unsigned pending_input;
static bool inject_request;
static uint64_t inject_at;
static bool sched_tick_should_preempt(void) {
 if(inject_request && ticks>=inject_at)resched=true;
 return resched;
}
static unsigned input_core_pending(void) { return pending_input; }
static uint64_t x86_read_tsc(void) { return ticks; }
static uint64_t x86_timeout_ns_to_tsc(uint64_t ns) { return ns; }
static uint64_t damage_pixels;
static uint64_t compositor_publication_damage_pixels(void) { return damage_pixels; }
static bool policy(bool commit_as_move_transaction) {
 bool allow_yield;
''' + policy + r'''
 return allow_yield;
}
static void fence(void) { assert(preempt==1); ++fences; }
static void sched_preempt_enable(void) { assert(preempt==1 && fences==motion_pumps+1); --preempt; }
static void schedule(void) { assert(!preempt && resched); ++yields; }
static void window_server_pump_publication_motion(void) {
 assert(!preempt && fences==motion_pumps+1);++motion_pumps;
 if(wake_on_motion)resched=true;
}
static void sched_preempt_disable(void) { assert(!preempt); ++preempt; }
static void compositor_copy_wc_scanline(volatile uint32_t *dst,const uint32_t *src,uint32_t pixels) {
 assert(preempt==1); ++rows;
 ticks+=row_ticks;
 for (uint32_t x=0;x<pixels;++x) dst[x]=src[x];
}
static void window_present_cursor_prepare_rect(const Rect *rect) {
 assert(preempt==1);
 if(rect->x0<=10&&rect->x1>10&&rect->y0<=170&&rect->y1>170)
  g_window_server.framebuffer[170*g_window_server.display_stride+10]=0xabcdef;
}
static bool window_present_cursor_publish_span(volatile uint32_t *dst,const uint32_t *src,
 uint32_t x,uint32_t y,uint32_t pixels) {
 assert(preempt==1);
 if (y!=170 || x>10 || x+pixels<=10) return false;
 ++rows;ticks+=row_ticks;
 assert(dst[10-x]==0xabcdef);
 for(uint32_t i=0;i<pixels;++i)if(i!=10-x)dst[i]=src[i];
 ++cursor_rows; return true;
}
''' + body + r'''
int main(void) {
 const unsigned width=945,height=1060;
 uint32_t *front=calloc(width*height,sizeof(uint32_t)), *back=malloc(width*height*4);
 assert(front && back);
 for(unsigned i=0;i<width*height;++i) back[i]=i+1;
 g_window_server.framebuffer=front;g_window_server.composite_framebuffer=back;
 g_window_server.display_stride=width;
 g_window_server.display_width=width;g_window_server.display_height=height;
 damage_pixels=(uint64_t)width*height;
 assert(policy(false) && !policy(true));
 Rect rect={0,0,width,height};
 compositor_publication_rect(&rect,policy(false));
 assert(rows==height && cursor_rows==1 && yields==7 && fences==yields && preempt==1);
 assert(motion_pumps==yields);
 for(unsigned i=0;i<width*height;++i) assert(front[i]==(i==170*width+10?0xabcdef:back[i]));
 // Drag and tiny updates retain atomic behavior; no final-row scheduling.
 unsigned before=yields; compositor_publication_rect(&rect,policy(true)); assert(yields==before);
 damage_pixels=24*24;assert(!policy(false));
 rect=(Rect){0,0,24,24};compositor_publication_rect(&rect,policy(false));assert(yields==before);
 rect=(Rect){0,0,width,139};compositor_publication_rect(&rect,true);assert(yields==before);
 // Slow device memory: time, not just pixel count, bounds the input blackout.
 row_ticks=250000;before=yields;
 rect=(Rect){0,0,100,100};compositor_publication_rect(&rect,true);
 assert(yields-before==24 && motion_pumps==yields);
 row_ticks=0;before=yields;
 rect=(Rect){3,4,3,8};compositor_publication_rect(&rect,true);
 compositor_publication_rect(0,true);assert(yields==before && preempt==1);
 // Negative and overlarge rectangles must be clipped before pointer math.
 // Use a padded stride and canaries to catch writes outside the visible area.
 uint32_t padded_front[8*6],padded_back[8*6];
 for(unsigned i=0;i<48;++i){padded_front[i]=0xdead;padded_back[i]=1000+i;}
 g_window_server.framebuffer=padded_front;g_window_server.composite_framebuffer=padded_back;
 g_window_server.display_width=5;g_window_server.display_height=4;g_window_server.display_stride=8;
 rect=(Rect){INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX};
 Rect original=rect;
 compositor_publication_rect(&rect,false);
 assert(rect.x0==original.x0&&rect.y0==original.y0&&rect.x1==original.x1&&rect.y1==original.y1);
 for(unsigned y=0;y<6;++y)for(unsigned x=0;x<8;++x)
  assert(padded_front[y*8+x]==(y<4&&x<5?padded_back[y*8+x]:0xdead));
 for(unsigned i=0;i<48;++i)padded_front[i]=0xdead;
 rect=(Rect){-3,-2,2,2};
 compositor_publication_rect(&rect,false);
 for(unsigned y=0;y<6;++y)for(unsigned x=0;x<8;++x)
  assert(padded_front[y*8+x]==(y<2&&x<2?padded_back[y*8+x]:0xdead));
 for(unsigned i=0;i<48;++i)padded_front[i]=0xdead;
 rect=(Rect){3,2,20,20};
 compositor_publication_rect(&rect,false);
 for(unsigned y=0;y<6;++y)for(unsigned x=0;x<8;++x)
  assert(padded_front[y*8+x]==(y>=2&&y<4&&x>=3&&x<5?padded_back[y*8+x]:0xdead));
 unsigned copied_rows=rows;
 const Rect outside[]={{-10,-10,-1,-1},{5,0,9,2},{0,4,2,9},{INT32_MAX,0,INT32_MAX,4}};
 for(unsigned i=0;i<sizeof(outside)/sizeof(outside[0]);++i)
  compositor_publication_rect(&outside[i],true);
 assert(rows==copied_rows);
 g_window_server.display_stride=4;compositor_publication_rect(&original,false);
 assert(rows==copied_rows);
 g_window_server.display_stride=8;g_window_server.framebuffer=0;
 compositor_publication_rect(&original,false);assert(rows==copied_rows);
 assert(yields==before&&preempt==1);

 // 无调度请求和输入：完整复制，不额外拆分可见帧。
 g_window_server.framebuffer=front;g_window_server.composite_framebuffer=back;
 g_window_server.display_stride=width;g_window_server.display_width=width;
 g_window_server.display_height=height;rect=(Rect){0,0,width,height};
 unsigned old_yields=yields,old_fences=fences,old_pumps=motion_pumps;
 resched=false;pending_input=0;
 compositor_publication_rect(&rect,true);
 assert(yields==old_yields && fences==old_fences && motion_pumps==old_pumps && preempt==1);
 for(unsigned i=0;i<width*height;++i)assert(front[i]==(i==170*width+10?0xabcdef:back[i]));
 // 只有鼠标输入时及时处理，不强制将 CPU 切给其他线程。
 pending_input=1;
 compositor_publication_rect(&rect,true);
 assert(yields==old_yields && motion_pumps==old_pumps+7 && fences==old_fences+7 && preempt==1);
 // 输入处理新唤醒线程，必须在处理后再次检查调度需求。
 wake_on_motion=true;old_yields=yields;old_pumps=motion_pumps;
 compositor_publication_rect(&rect,true);
 assert(yields==old_yields+7 && motion_pumps==old_pumps+7 && preempt==1);
 wake_on_motion=false;pending_input=0;
 // 模拟慢设备上在复制中途到达抢占请求，仍按时间预算交还 CPU。
 resched=false;row_ticks=250000;
 old_yields=yields;old_pumps=motion_pumps;
 inject_request=true;inject_at=ticks+1500000;
 rect=(Rect){0,0,100,100};compositor_publication_rect(&rect,true);
 assert(yields>old_yields && motion_pumps-old_pumps==yields-old_yields && preempt==1);
 // 同样的画面拆成单行矩形，预算仍按整批累计。
 inject_request=false;resched=true;row_ticks=0;
 Rect strips[1060];
 for(unsigned y=0;y<height;++y)strips[y]=(Rect){0,y,width,y+1};
 old_yields=yields;old_pumps=motion_pumps;unsigned old_rows=rows;
 compositor_publication_rects(strips,height,true);
 assert(yields==old_yields+7 && motion_pumps==old_pumps+7 && rows==old_rows+height);
 for(unsigned i=0;i<width*height;++i)assert(front[i]==(i==170*width+10?0xabcdef:back[i]));
 // 极窄碎片未达到像素预算时，跨矩形的时间预算仍有效。
 row_ticks=250000;old_yields=yields;
 for(unsigned y=0;y<100;++y)strips[y]=(Rect){0,y,1,y+1};
 compositor_publication_rects(strips,100,true);
 assert(yields==old_yields+24 && preempt==1);
 // 没有需求时不额外调度；原子事务及空输入保持原有行为。
 resched=false;pending_input=0;old_yields=yields;old_pumps=motion_pumps;
 compositor_publication_rects(strips,100,true);
 assert(yields==old_yields && motion_pumps==old_pumps);
 resched=true;compositor_publication_rects(strips,100,false);
 compositor_publication_rects(0,100,true);
 compositor_publication_rects(strips,0,true);
 assert(yields==old_yields && preempt==1);
 free(front);free(back);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-publication-preempt-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
    for old,new in [
        ('        compositor_publication_rect_budget(&rects[index], allow_yield,',
         '        budget = (compositor_publication_budget_t){0};\n        compositor_publication_rect_budget(&rects[index], allow_yield,'),
        ('if (sched_tick_should_preempt()) schedule();','schedule();'),
        ('!sched_tick_should_preempt() && input_core_pending() == 0U',
         'input_core_pending() == 0U'),
    ]:
        assert old in program
        (path/'test.c').write_text(program.replace(old,new))
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
        result=subprocess.run([str(path/'test')],capture_output=True,
            preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert result.returncode!=0 and b'Assertion' in result.stderr
    print('负面对照通过：逐块重置预算、强制中途调度或忽略真实抢占请求均被拒绝')
print('Production publication: shared pixel/time budget across fragmented damage, demand-driven yields, idle continuous copy, input-only service, post-input wake and mid-copy preemption; fence/cursor/clip/stride and bounded slow-device cases passed')
