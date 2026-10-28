"""执行生产损坏区域合并，以像素并集验证阈值、包含快路及批量合并不漏绘。"""
from pathlib import Path
import subprocess
import resource
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/damage.c').read_text()

def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]

body = '\n'.join(function(name) for name in (
    'static bool window_damage_try_merge(',
    'void window_mark_rect_locked(',
    'void window_coalesce_damage_locked(',
))
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/rect.h>
#define WINDOW_DAMAGE_MAX_RECTS 16U
static struct {
 unsigned display_width,display_height,damage_count;
 bool dirty,damage_full,damage_tiles_active;
 Rect damage_bounds,damage_rects[16];
} g_window_server;
static unsigned edges,tiles;
static bool window_dirty_edge_locked(void){++edges;g_window_server.dirty=true;return true;}
static void window_damage_enable_tiles_locked(void){g_window_server.damage_tiles_active=true;++tiles;}
static void window_damage_mark_tiles_locked(const Rect *r){assert(r->x0<r->x1&&r->y0<r->y1);++tiles;}
''' + body + r'''
static bool inside(Rect r,int x,int y){return x>=r.x0&&x<r.x1&&y>=r.y0&&y<r.y1;}
static unsigned seed=729;
static unsigned random_value(void){seed=seed*1664525U+1013904223U;return seed;}
int main(void){
 unsigned pairs=0;
 /* 用逐像素并集计数决定是否合并，避免把生产的面积公式复制为测试预期。 */
 for(int x0=0;x0<5;++x0)for(int y0=0;y0<5;++y0)
 for(int x1=x0+1;x1<=5;++x1)for(int y1=y0+1;y1<=5;++y1)
 for(int u0=0;u0<5;++u0)for(int v0=0;v0<5;++v0)
 for(int u1=u0+1;u1<=5;++u1)for(int v1=v0+1;v1<=5;++v1){
  Rect a={x0,y0,x1,y1},b={u0,v0,u1,v1},original=a;
  unsigned area=0;int left=5,top=5,right=0,bottom=0;
  for(int y=0;y<5;++y)for(int x=0;x<5;++x)if(inside(a,x,y)||inside(b,x,y)){
   ++area;if(x<left)left=x;if(y<top)top=y;if(x+1>right)right=x+1;if(y+1>bottom)bottom=y+1;
  }
  unsigned box=0;for(int y=top;y<bottom;++y)for(int x=left;x<right;++x)++box;
  bool expected=box<=area+area/4;
  assert(window_damage_try_merge(&a,&b)==expected);
  if(expected)assert(a.x0==left&&a.y0==top&&a.x1==right&&a.y1==bottom);
  else assert(!memcmp(&a,&original,sizeof(a)));
  ++pairs;
 }
 /* 大坐标包含与部分交叠，面积不得截断成 32 位。 */
 Rect a={0,0,INT32_MAX,INT32_MAX},b={1,1,INT32_MAX-1,INT32_MAX-1};
 assert(window_damage_try_merge(&a,&b)&&a.x1==INT32_MAX);
 a=b;b=(Rect){0,0,INT32_MAX,INT32_MAX};assert(window_damage_try_merge(&a,&b)&&a.x0==0);
 a=(Rect){0,0,1000000000,1000000000};b=(Rect){1000000000,0,2000000000,1000000000};
 assert(window_damage_try_merge(&a,&b)&&a.x1==2000000000);
 /* 插入保持原先的不相交拒绝策略，批量整理仍允许低成本间隙合并。 */
 memset(&g_window_server,0,sizeof(g_window_server));
 g_window_server.display_width=32;g_window_server.display_height=32;
 window_mark_rect_locked(0,0,8,8);window_mark_rect_locked(9,0,8,8);
 assert(g_window_server.damage_count==2&&edges==1);
 window_coalesce_damage_locked();assert(g_window_server.damage_count==1);
 assert(g_window_server.damage_rects[0].x1==17);
 /* 重复更新和包含不增加矩形，也不能反复产生唤醒边沿。 */
 for(unsigned i=0;i<10000;++i)window_mark_rect_locked(1,1,2,2);
 assert(g_window_server.damage_count==1&&edges==1&&!tiles);
 /* 随机批量合并后，输入脏像素必须全部保留，且不存在可以继续合并的矩形对。 */
 for(unsigned trial=0;trial<1000;++trial){
  unsigned char dirty[32*32]={0};
  memset(&g_window_server,0,sizeof(g_window_server));
  g_window_server.display_width=32;g_window_server.display_height=32;
  g_window_server.damage_count=16;
  for(unsigned i=0;i<16;++i){
   int x=random_value()%31,y=random_value()%31;
   Rect r={x,y,x+1+(int)(random_value()%(32-x)),y+1+(int)(random_value()%(32-y))};
   g_window_server.damage_rects[i]=r;
   for(int py=r.y0;py<r.y1;++py)for(int px=r.x0;px<r.x1;++px)dirty[py*32+px]=1;
  }
  window_coalesce_damage_locked();
  assert(g_window_server.damage_count>=1&&g_window_server.damage_count<=16);
  for(int y=0;y<32;++y)for(int x=0;x<32;++x)if(dirty[y*32+x]){
   bool covered=false;
   for(unsigned i=0;i<g_window_server.damage_count;++i)covered|=inside(g_window_server.damage_rects[i],x,y);
   assert(covered);
  }
  /* 再整理必须稳定，结果不得因为删除中间元素而遗漏可合并对。 */
  unsigned count=g_window_server.damage_count;
  Rect saved[16];memcpy(saved,g_window_server.damage_rects,sizeof(saved));
  window_coalesce_damage_locked();
  assert(count==g_window_server.damage_count&&!memcmp(saved,g_window_server.damage_rects,count*sizeof(Rect)));
 }
 printf("Damage merge: %u pixel-oracle pairs, large coordinates, 10000 contained updates and 1000 batch coverage/stability cases PASS\n",pairs);
}
'''
with tempfile.TemporaryDirectory(prefix='damage-merge-') as directory:
    binary = Path(directory) / 'test'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', str(binary)],
                   input=program, text=True, check=True)
    subprocess.run([str(binary)], check=True)
    # 旧逻辑只重新比较当前矩形，会漏掉已经越过的矩形，稳定性检查必须拒绝它。
    old = 'first = 0U;\n                second = 1U;'
    assert old in program
    mutant = program.replace(old, 'second = first + 1U;')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', str(binary)],
                   input=mutant, text=True, check=True)
    result = subprocess.run([str(binary)], capture_output=True,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode != 0 and b'Assertion' in result.stderr
    print('负面对照通过：跳过前面区域的旧扫描方式被稳定性检查拒绝')
