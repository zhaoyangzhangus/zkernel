"""Actual rational pacing: exact deadlines, stall recovery and timeout bounds."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = root / 'tools/nvidia/video_pacing.h'
player = (root / 'tools/nvidia/vulkan_video_player.c').read_text()
assert 'window_closed(fd,33)' not in player
assert 'closed=wait_next_frame(fd,&pacing)' in player
code = r'''
#include <assert.h>
#include "video_pacing.h"
int main(void) {
    const int rates[][2]={{30,1},{60,1},{30000,1001},{24000,1001},{240,1},{1,1}};
    for(unsigned r=0;r<sizeof(rates)/sizeof(rates[0]);++r) {
        video_pacing_t p;video_pacing_init(&p,rates[r][0],rates[r][1]);
        uint64_t base=1000000000,now=base;
        for(uint64_t i=1;i<=100000;++i) {
            uint64_t expected=base+i*UINT64_C(1000000000)*rates[r][1]/rates[r][0];
            uint64_t deadline=video_pacing_advance(&p,now);
            assert(deadline==expected);
            /* 渲染耗时 2ms；下一个间隔不能累加这段时间。 */
            now=deadline+2000000;
            assert(video_pacing_timeout(deadline,deadline)==0);
            assert(video_pacing_timeout(deadline,deadline-1)==1);
        }
        now=p.deadline+p.step*10;
        uint64_t deadline=video_pacing_advance(&p,now);
        assert(deadline>now&&deadline-now<=p.step+1);
        int first=video_pacing_timeout(deadline,now);
        for(unsigned event=0;event<1000;++event)
            assert(video_pacing_timeout(deadline,now)==first&&p.deadline==deadline);
    }
    const int invalid[][2]={{0,0},{-1,1},{1,0},{1,-1},{1,2},{241,1},{INT_MAX,1}};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        video_pacing_t p;video_pacing_init(&p,invalid[i][0],invalid[i][1]);
        assert(p.step==33333333&&p.divisor==30);
    }
    video_pacing_t p;video_pacing_init(&p,INT_MAX,INT_MAX);
    assert(p.step==1000000000);
    assert(video_pacing_advance(&p,UINT64_MAX-1)==UINT64_MAX);
    assert(video_pacing_timeout(UINT64_MAX,0)==INT_MAX);
    assert(video_pacing_timeout(1000001,0)==2);
    assert(video_pacing_timeout(1000000,0)==1);
}
'''
with tempfile.TemporaryDirectory(prefix='video-pacing-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(header.parent), str(base / 'test.c'), '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=15)
print('Video pacing: 600000 exact fractional deadlines, no render-time drift, input wake stability, stalls, malformed rates and overflow bounds passed')
