"""执行真实合成工作线程和任务屏障，验证逐行像素及唤醒竞态。"""
from pathlib import Path
import subprocess
import resource
import signal
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/parallel.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
render=(root/'kernel/graphics/render.c').read_text()
rows=render[render.index('typedef struct compositor_surface_job'):render.index('static void compositor_surface_source_prepare(')]
control=(root/'kernel/graphics/control_document.c').read_text()
rows+='\ntypedef struct window_control_document window_control_document_t;\n'+control[control.index('typedef struct control_paint_context {'):control.index('static void control_fill(')]
geometry=(root/'kernel/graphics/window_geometry.c').read_text()
def function(name):
    start=geometry.index(name)
    start=geometry.rfind('\n',0,start)+1
    return geometry[start:geometry.index('\n}',start)+2]
geometry='\n'.join(function(name) for name in ('window_client_decorations(',
    'window_standard_caption(', 'window_frame_border(', 'window_frame_extra(',
    'window_titlebar_height(', 'window_client_offset_y(', 'window_corner_inset(',
    'compositor_corner_inset('))
program = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/wait.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include "internal.h"
#include <uapi/chrome_caption.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static unsigned cpus,spawned;
static bool reject_first;
static _Thread_local unsigned current_cpu=1;
uint32_t x86_current_cpu_index(void){return current_cpu;}
uint32_t x86_smp_discovered_count(void){return cpus;}
bool x86_smp_cpu_online(uint32_t cpu){return cpu<cpus;}
void x86_kernel_thread_start(void){abort();}
void wait_queue_init(wait_queue_t *q){memset(q,0,sizeof(*q));}
kstatus_t wait_on_queue_uninterruptible(wait_queue_t *q,bool (*ready)(void *),void *context,uint64_t timeout){
 (void)q;assert(timeout==UINT64_MAX);pthread_mutex_lock(&lock);
 while(!ready(context))pthread_cond_wait(&changed,&lock);
 pthread_mutex_unlock(&lock);return K_OK;
}
uint32_t wake_one(wait_queue_t *q){
 (void)q;pthread_mutex_lock(&lock);pthread_cond_broadcast(&changed);pthread_mutex_unlock(&lock);return 1;
}
void sched_initialize_new_thread(thread_t *t,uint32_t cpu,bool pinned){
 assert(pinned&&cpu>1&&cpu<cpus);t->owner_cpu=cpu;
}
static void *run(void *raw){
 thread_t *t=raw;current_cpu=t->owner_cpu;
 void (*entry)(void *)=(void (*)(void *))(uintptr_t)t->arch.switch_ctx.r12;
 entry((void *)(uintptr_t)t->arch.switch_ctx.r13);return 0;
}
bool sched_enqueue_bootstrap(thread_t *t){
 if(reject_first){reject_first=false;return false;}
 pthread_t id;assert(!pthread_create(&id,0,run,t));pthread_detach(id);++spawned;return true;
}
void sched_monitor_register(thread_t *t,const char *name){assert(t&&name);}
''' + source + '\n' + geometry + r'''
window_server_state_t g_window_server;
compositor_snapshot_t g_compositor_snapshot;
static Rect test_damage;
void compositor_surface_source_prepare(const compositor_window_view_t *w,compositor_surface_source_context_t *s){
 *s=(compositor_surface_source_context_t){.row_bytes=w->width*4U,.readable=true};
}
void compositor_copy_surface_span(const compositor_window_view_t *w,const compositor_surface_source_context_t *s,
 uint32_t row,uint32_t first,uint32_t end,uint32_t *wb,volatile uint32_t *device,
 uint64_t *cached,uint8_t **base){
 (void)w;(void)s;(void)cached;(void)base;
 for(unsigned i=first;i<end;++i){
  uint32_t pixel=(row*w->width+i)*2654435761U;
  if(wb)*wb++=pixel;else *device++=pixel;
 }
}
''' + rows + r'''
static void surface_integration(void){
 uint_fast64_t before=atomic_load(&g_compositor_parallel_batches);
 unsigned window_batches=0,control_batches=0;
 static uint32_t serial[1024*180],parallel[1024*180];
 g_window_server.display_width=1024;g_window_server.display_height=180;
 g_window_server.display_stride=1024;
 for(unsigned mode=0;mode<360;++mode){
  int positions_x[]={0,-17,1000,1024,-1100};
  int positions_y[]={0,-43,160,180,-300};
  compositor_window_view_t w={.x=positions_x[mode%5],.y=positions_y[(mode/5)%5],
   .width=mode%2?944U:1024U,.height=150,.maximized=mode%2};
  w.flags=mode%3?OS_WINDOW_CLIENT_DECORATIONS:0;
  if(mode%4==0)w.flags|=OS_WINDOW_STANDARD_CAPTION_CONTROLS;
  test_damage=(Rect){.x0=mode%2?3:0,.y0=mode%3?7:0,.x1=1024,.y1=mode%2?32:180};
  memset(serial,0xa5,sizeof(serial));memset(parallel,0xa5,sizeof(parallel));
  /* 独立逐像素基准，不复用生产代码的客户区裁剪或扫描线分派。 */
  bool embedded=mode%3==0;
  control_tree_box_t box={.bounds={mode%2?8:0,mode%2?12:0,1020,145}};
  control_paint_context_t context={.view=&w,.x=w.x,.y=w.y};
  Rect local_damage={box.bounds.x0,box.bounds.y0,1020,mode%2?40:145};
  int64_t left=embedded?(int64_t)w.x+box.bounds.x0:(int64_t)w.x+window_frame_border(w.flags);
  int64_t top=embedded?(int64_t)w.y+box.bounds.y0:(int64_t)w.y+window_client_offset_y(w.flags);
  Rect damage=embedded?(Rect){w.x+local_damage.x0,w.y+local_damage.y0,w.x+1020,w.y+local_damage.y1}:
   test_damage;
  for(int y=0;y<180;++y)for(int x=0;x<1024;++x){
   int64_t row=y-top,col=x-left,fr=(int64_t)y-w.y;
   if(x<damage.x0||x>=damage.x1||y<damage.y0||y>=damage.y1||
      row<0||row>=w.height||col<0||col>=w.width)continue;
   uint32_t inset=embedded||w.maximized?0:compositor_corner_inset(fr,
    w.width+window_frame_extra(w.flags),
    w.height+window_frame_extra(w.flags)+window_titlebar_height(w.flags));
   if(!embedded&&!w.maximized&&window_client_decorations(w.flags)&&window_standard_caption(w.flags))
    inset=chrome_frame_row_inset(fr,w.width,w.height);
   if(col<inset||col>=(int64_t)w.width-inset)continue;
   serial[y*1024+x]=(uint32_t)(row*w.width+col)*2654435761U;
  }
  g_window_server.composite_framebuffer=parallel;
  g_window_server.framebuffer=mode%7?0:parallel;
  uint_fast64_t batch=atomic_load(&g_compositor_parallel_batches);
  if(embedded)control_paint_surface(&context,&box,local_damage);
  else compositor_surface_locked(test_damage,&w);
  if(atomic_load(&g_compositor_parallel_batches)>batch){
   if(embedded)++control_batches;else ++window_batches;
  }
  assert(!memcmp(serial,parallel,sizeof(serial)));
  if(damage.y1-damage.y0<64)assert(atomic_load(&g_compositor_parallel_batches)==batch);
 }
 if(spawned){assert(atomic_load(&g_compositor_parallel_batches)>before);
  assert(window_batches&&control_batches);}
 else assert(!window_batches&&!control_batches);
 puts("PASS: 360 independent window/control Surface pixel oracles, offscreen/edge clipping, 32-row strips, direct framebuffer, maximized and frame corners");
}

static uint32_t pixels[260][1025];
static atomic_uint seen[260],active,maximum,participants;
typedef struct {unsigned width,first,end;bool nested;} job_t;
static void nested(void *raw,uint32_t first,uint32_t end){
 atomic_uint *visits=raw;assert(first<end&&end<=64);atomic_fetch_add(visits,end-first);
}
static void draw(void *raw,uint32_t first,uint32_t end){
 job_t *job=raw;assert(first>=job->first&&end<=job->end&&first<end);
 unsigned n=atomic_fetch_add(&active,1)+1,old=atomic_load(&maximum);
 while(n>old&&!atomic_compare_exchange_weak(&maximum,&old,n)){}
 atomic_fetch_or(&participants,1U<<current_cpu);
 if(job->nested){atomic_uint count=0;compositor_parallel_rows(0,64,1024,nested,&count);assert(atomic_load(&count)==64);}
 usleep(100);
 for(unsigned y=first;y<end;++y){
  assert(atomic_fetch_add(&seen[y],1)==0);
  for(unsigned x=0;x<job->width;++x)pixels[y][x]=(y*65537U)^x;
 }
 atomic_fetch_sub(&active,1);
}
int main(int argc,char **argv){
 assert(argc==3);cpus=atoi(argv[1]);reject_first=atoi(argv[2]);
 unsigned expected=cpus<=2?0:cpus-2-(unsigned)reject_first;
 compositor_parallel_start();compositor_parallel_start();assert(spawned==expected);
 surface_integration();
 compositor_parallel_rows(1,1,1024,draw,0);
 compositor_parallel_rows(0,10,1024,0,0);
 /* 即使很宽，32 行输入服务分片也不得唤醒工作线程。 */
 atomic_uint strip_rows=0;
 uint_fast64_t strip_batches=atomic_load(&g_compositor_parallel_batches);
 compositor_parallel_rows(0,32,3840,nested,&strip_rows);
 assert(atomic_load(&strip_rows)==32);
 assert(atomic_load(&g_compositor_parallel_batches)==strip_batches);
 /* 精确覆盖输入服务预算边界：预算内串行，超过预算才允许跨核分派。 */
 atomic_store(&strip_rows,0);
 compositor_parallel_rows(0,64,2048,nested,&strip_rows);
 assert(atomic_load(&strip_rows)==64);
 assert(atomic_load(&g_compositor_parallel_batches)==strip_batches);
 atomic_store(&strip_rows,0);
 compositor_parallel_rows(0,64,2049,nested,&strip_rows);
 assert(atomic_load(&strip_rows)==64);
 assert(atomic_load(&g_compositor_parallel_batches)==strip_batches+(spawned?1U:0U));
 for(unsigned i=0;i<150;++i){
  unsigned heights[]={7,8,15,16,24,32,63,128,255};
  job_t job={.width=i%5==0?1U:i%2?1025U:1024U,.first=2,.nested=i%7==0};
  job.end=job.first+heights[i%9];
  memset(pixels,0xa5,sizeof(pixels));for(unsigned y=0;y<260;++y)atomic_store(&seen[y],0);
  compositor_parallel_rows(job.first,job.end,job.width,draw,&job);
  assert(!atomic_load(&active));
  for(unsigned y=0;y<260;++y){
   bool hit=y>=job.first&&y<job.end;
   assert(atomic_load(&seen[y])==(unsigned)hit);
   for(unsigned x=0;x<1025;++x)assert(pixels[y][x]==(hit&&x<job.width?((y*65537U)^x):0xa5a5a5a5U));
  }
 }
 if(expected){assert(atomic_load(&maximum)>=2);assert(atomic_load(&g_compositor_parallel_batches)>0);assert(atomic_load(&g_compositor_parallel_worker_rows)>0);}
 else assert(!atomic_load(&g_compositor_parallel_batches));
 printf("PASS CPUs=%u workers=%u: 150 batches, exact row/pixel oracle, nested submission, small-area fallback, completion lifetime and real concurrent execution\n",cpus,spawned);
}
'''
with tempfile.TemporaryDirectory(prefix='compositor-parallel-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-D_DEFAULT_SOURCE','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/graphics'),'-x','c','-','-o',binary],input=program,text=True,check=True)
    for cpus,failure in ((2,0),(3,0),(4,0),(4,1)):
        subprocess.run([binary,str(cpus),str(failure)],check=True,timeout=30)

    guard='worker->draw(worker->context, worker->first, worker->end);'
    assert program.count(guard)==1
    negative=program.replace(guard,'/* 负面对照：故意丢弃工作线程绘制。 */')
    subprocess.run(['cc','-D_DEFAULT_SOURCE','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-I'+str(root/'kernel/graphics'),
                    '-x','c','-','-o',binary],input=negative,text=True,check=True)
    result=subprocess.run([binary,'4','0'],capture_output=True,text=True,timeout=30,
        preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode==-signal.SIGABRT and 'memcmp(serial,parallel' in result.stderr
    print('负面对照通过：工作线程漏绘扫描线会触发完整表面像素差异')
