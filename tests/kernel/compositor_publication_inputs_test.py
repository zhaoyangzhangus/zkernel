"""执行发布策略的面积统计与旧位置擦除，禁止隐式依赖全局帧快照。"""
from pathlib import Path
import subprocess
import resource
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/publication_policy.c').read_text()
area = source[source.index('uint64_t compositor_publication_damage_pixels('):
              source.index('/* 发布一个损坏矩形。')]
exposure = source[source.index('static void compositor_publication_drag_exposure_rect('):]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/rect.h>
typedef struct {
 bool drag_blit_valid;
 uint32_t drag_width,drag_height;
 int32_t drag_old_x,drag_old_y,drag_new_x,drag_new_y;
} compositor_snapshot_t;
static struct {uint32_t display_width,display_height;} g_window_server={64,48};
static unsigned writes[64*48];
static void compositor_publication_rect(const Rect *r,bool allow_yield){
 assert(!allow_yield&&r->x0>=0&&r->y0>=0&&r->x1<=64&&r->y1<=48);
 assert(r->x0<r->x1&&r->y0<r->y1);
 for(int y=r->y0;y<r->y1;++y)for(int x=r->x0;x<r->x1;++x)++writes[y*64+x];
}
''' + area + exposure + r'''
static uint32_t random_state=173;
static uint32_t next(void){random_state=random_state*1664525U+1013904223U;return random_state;}
static bool inside(int x,int y,int32_t left,int32_t top,uint32_t width,uint32_t height){
 return (int64_t)x>=left&&(int64_t)y>=top&&
  (int64_t)x<(int64_t)left+width&&(int64_t)y<(int64_t)top+height;
}
int main(void){
 assert(compositor_publication_damage_pixels(0,UINT32_MAX)==0);
 assert(compositor_publication_damage_pixels((Rect[]){{0,0,1,1}},0)==0);
 compositor_publication_drag_old_exposure(0);
 const int32_t positions[]={INT32_MIN,-65,-1,0,1,31,47,63,64,INT32_MAX};
 const uint32_t sizes[]={0,1,2,24,64,128,UINT32_MAX};
 for(unsigned sample=0;sample<12000;++sample){
  Rect rects[4];
  __uint128_t total=0;
  for(unsigned i=0;i<4;++i){
   rects[i]=(Rect){positions[next()%10],positions[next()%10],positions[next()%10],positions[next()%10]};
   Rect r=rects[i];
   if(r.x0<r.x1&&r.y0<r.y1)total+=(__uint128_t)((int64_t)r.x1-r.x0)*((int64_t)r.y1-r.y0);
  }
  assert(compositor_publication_damage_pixels(rects,4)==(total>UINT64_MAX?UINT64_MAX:(uint64_t)total));
  compositor_snapshot_t frame={sample%17!=0,sizes[next()%7],sizes[next()%7],
   positions[next()%10],positions[next()%10],positions[next()%10],positions[next()%10]};
  compositor_snapshot_t original=frame;
  memset(writes,0,sizeof(writes));
  compositor_publication_drag_old_exposure(&frame);
  assert(!memcmp(&original,&frame,sizeof(frame)));
  for(int y=0;y<48;++y)for(int x=0;x<64;++x){
   bool exposed=frame.drag_blit_valid&&
    inside(x,y,frame.drag_old_x,frame.drag_old_y,frame.drag_width,frame.drag_height)&&
    !inside(x,y,frame.drag_new_x,frame.drag_new_y,frame.drag_width,frame.drag_height);
   assert(writes[y*64+x]==(unsigned)exposed);
  }
 }
 Rect huge={INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX};
 assert(compositor_publication_damage_pixels((Rect[]){huge,huge},2)==UINT64_MAX);
 puts("发布输入：12000 组独立面积/拖动像素对照通过，覆盖饱和、空输入、极端坐标、不重复擦除及快照只读");
}
'''
with tempfile.TemporaryDirectory(prefix='publication-inputs-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
    # 反例必须被独立对照拒绝，防止用只执行不核对像素的夹具掩盖回退。
    for label, previous, replacement in (
        ('遗漏纵向暴露条带', 'if (new_top > old_top)', 'if (false && new_top > old_top)'),
        ('面积溢出归零', 'if (UINT64_MAX - pixels < area) return UINT64_MAX;',
         'if (UINT64_MAX - pixels < area) return 0U;'),
    ):
        assert previous in program
        subprocess.run(['cc', '-std=c11', '-O2', '-I' + str(root / 'include'),
                        '-x', 'c', '-', '-o', binary],
                       input=program.replace(previous, replacement, 1), text=True, check=True)
        result = subprocess.run([binary], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert result.returncode != 0, label
        print('反例被拒绝：' + label)
