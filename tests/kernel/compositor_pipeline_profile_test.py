"""执行真实帧协调函数，核对阶段顺序、关闭时零计时与完整帧统计。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/compositor.c').read_text()
body = source[source.index('void compositor_frame_run(void) {'):
              source.index('\n/* 遮挡淘汰和帧结束共用释放顺序')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define K_OK 0
static int g_compositor_publication_status;
static volatile bool g_compositor_profile_enabled;
static volatile uint64_t g_compositor_pipeline_profile[6];
static uint64_t ticks;
static unsigned clocks,step;
static bool compose=true,locked;
static uint64_t x86_read_tsc(void){++clocks;return ticks;}
static void desktop_wallpaper_apply_pending(void){assert(step++==0);ticks+=11;}
static void window_lock(void){assert(step++==1&&!locked);locked=true;ticks+=13;}
static bool compositor_snapshot_begin_locked(void){assert(step++==2&&locked);ticks+=17;return compose;}
static void window_unlock(void){assert(step++==3&&locked);locked=false;ticks+=19;}
static void desktop_prepare_text(void){assert(step++==4&&!locked);ticks+=23;}
static void compositor_snapshot_plan(void){assert(step++==5&&!locked);ticks+=29;}
static void compositor_render_snapshot(void){assert(step++==6&&!locked);ticks+=31;}
static void compositor_snapshot_finish(void){
 assert(step++==7&&!locked);ticks+=37;
 /* 统计只能在完整帧结束后发布，不得阶段完成就写一部分。 */
 assert(g_compositor_pipeline_profile[0]==0);
 for(unsigned i=1;i<6;++i)assert(g_compositor_pipeline_profile[i]==0);
}
''' + body + r'''
int main(void){
 for(unsigned enabled=0;enabled<2;++enabled)
 for(unsigned valid=0;valid<2;++valid)
 for(unsigned wrapped=0;wrapped<2;++wrapped){
  memset((void*)g_compositor_pipeline_profile,0,sizeof(g_compositor_pipeline_profile));
  step=clocks=0;compose=valid;g_compositor_profile_enabled=enabled;
  ticks=wrapped?UINT64_MAX-20:0;g_compositor_publication_status=-1;
  compositor_frame_run();
  assert(!locked&&step==(valid?8U:4U)&&g_compositor_publication_status==K_OK);
  assert(clocks==(enabled?(valid?6U:1U):0U));
  const uint64_t expected[]={1,60,23,29,31,37};
  for(unsigned i=0;i<6;++i)
   assert(g_compositor_pipeline_profile[i]==(enabled&&valid?expected[i]:0));
 }
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-pipeline-profile-') as directory:
    work = Path(directory)
    (work/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(work/'test.c'), '-o', str(work/'test')], check=True)
    subprocess.run([str(work/'test')], check=True)
print('合成流水线：真实阶段顺序、关闭时零计时、跳过空帧、完整帧统计及 TSC 回绕通过')
