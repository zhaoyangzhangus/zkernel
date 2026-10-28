"""Production window producers versus a linear FIFO oracle at every ring boundary."""
from pathlib import Path
import argparse
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-controls', action='store_true')
args = parser.parse_args()
source = (root/'kernel/graphics/input_events.c').read_text()
pointer = source[source.index('static int32_t window_pointer_clamp_i32('):
                 source.index('void window_event_reset_ready_locked(')]
producers = source[source.index('static bool window_event_discard_motion_locked('):]
dispatch_source = (root/'kernel/graphics/window.c').read_text()
dispatch = dispatch_source[dispatch_source.index('kstatus_t window_server_dispatch('):
                           dispatch_source.index('typedef struct window_event_wait_context')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "uapi/window.h"
#define WINDOW_EVENT_CAPACITY 64U
#define K_OK 0
#define K_EINVAL -22
#define K_ENOENT -2
typedef int kstatus_t;
typedef os_input_event_t input_event_t;
typedef struct {
 uint32_t identifier,flags,width,height;int32_t x,y;uint64_t buffer_size;
 bool minimized;void *owner;
 uint32_t event_read,event_write,event_count;
 uint64_t canary_before;os_window_event_t events[64];uint64_t canary_after;
} window_server_window_t;
static struct {uint32_t pointer_x,pointer_y;int event_waitq;struct {uint32_t hovered_window_identifier;} scene;} g_window_server;
static unsigned wakes,locked,global_wakes,flushes,pointer_queries;
static bool routed_inside,routed_prepared;
static uint64_t test_revision=1;
static bool window_control_event_metadata_locked(const window_server_window_t *w,uint32_t target,os_control_event_t *e){
 (void)w;if(!target)return false;e->node_index=target%4096;e->node_kind=OS_CONTROL_BUTTON;
 e->tree_revision=test_revision;e->user_data=0xaabbcc0000000000ULL|target;return true;
}
static bool initialized=true;
static window_server_window_t *registered;
static window_server_window_t *window_scene_pointer_window_locked(void){++pointer_queries;return registered;}
static void window_lock(void){assert(!locked);locked=1;}
static void window_unlock(void){assert(locked);locked=0;}
static bool window_server_init(void){return initialized;}
static window_server_window_t *window_scene_find_locked(uint32_t id){assert(locked);return registered && registered->identifier==id?registered:NULL;}
static bool window_flush_event_wakes(void){assert(!locked);++flushes;return true;}
static unsigned wake_all(void *q){assert(!locked && q);++global_wakes;return 1;}
static uint32_t window_client_offset_x(uint32_t f){return f&OS_WINDOW_CLIENT_DECORATIONS?0:1;}
static uint32_t window_client_offset_y(uint32_t f){return f&OS_WINDOW_CLIENT_DECORATIONS?0:41;}
static bool window_control_route_locked(window_server_window_t *w,const os_input_event_t *e,int32_t x,int32_t y,bool inside,bool prepared){
 (void)w;(void)e;(void)x;(void)y;routed_inside=inside;routed_prepared=prepared;assert(locked);return false;
}
static void window_event_schedule_wake_locked(window_server_window_t *w){
 assert(locked && w->event_count && w->event_count<=64);
 os_window_event_t *last=&w->events[(w->event_write+63)%64];
 assert(last->identifier==w->identifier && last->type<=OS_WINDOW_EVENT_CONTROL);
 ++wakes;
}
''' + pointer + producers + dispatch + r'''
static os_window_event_t expected[64];static unsigned count,expected_wakes;
static bool oracle_room(window_server_window_t *w,bool motion){
 if(count<64)return true;
 unsigned remove=64;
 for(unsigned i=0;i<count;++i)if(expected[i].identifier==w->identifier &&
  expected[i].type==OS_WINDOW_EVENT_CONTROL&&expected[i].control.action==OS_CONTROL_MOVE){remove=i;break;}
 if(remove==64){if(motion)return false;remove=0;}
 memmove(expected+remove,expected+remove+1,(count-remove-1)*sizeof(*expected));--count;
 return true;
}
static int32_t clamp(__int128 value){return value<INT32_MIN?INT32_MIN:value>INT32_MAX?INT32_MAX:(int32_t)value;}
static void oracle(window_server_window_t *w,unsigned op,const input_event_t *in){
 os_window_event_t e={.identifier=w->identifier};
 if(op==5){
  e.type=OS_WINDOW_EVENT_MOVED;
  e.moved.x=clamp((__int128)w->x+window_client_offset_x(w->flags));
  e.moved.y=clamp((__int128)w->y+window_client_offset_y(w->flags));
  if(count && expected[count-1].identifier==w->identifier && expected[count-1].type==OS_WINDOW_EVENT_MOVED){
   expected[count-1].moved=e.moved;return;
  }
 }else if(op==4){
  for(unsigned i=0;i<count;++i)
   if(expected[i].identifier==w->identifier && expected[i].type==OS_WINDOW_EVENT_CLOSE_REQUEST)return;
  e.type=OS_WINDOW_EVENT_CLOSE_REQUEST;
 }else if(op==3){e.type=OS_WINDOW_EVENT_MINIMIZED;e.minimized.minimized=w->minimized;
 }else if(op==2){
  e.type=OS_WINDOW_EVENT_RESIZE;e.resize.width=w->width;e.resize.height=w->height;e.resize.buffer_size=w->buffer_size;
  if(count && expected[count-1].identifier==w->identifier && expected[count-1].type==OS_WINDOW_EVENT_RESIZE){
   expected[count-1].resize=e.resize;return;
  }
 }else{
  e.type=OS_WINDOW_EVENT_INPUT;e.input=*in;
  e.pointer_x=clamp((__int128)g_window_server.pointer_x-w->x-window_client_offset_x(w->flags));
  e.pointer_y=clamp((__int128)g_window_server.pointer_y-w->y-window_client_offset_y(w->flags));
 }
 assert(oracle_room(w,false));
 expected[count++]=e;++expected_wakes;
}
static void apply(window_server_window_t *w,unsigned op,const input_event_t *in){
 oracle(w,op,in);
 if(op==1){registered=w;assert(window_server_dispatch(w->identifier,in)==K_OK);}
 else{
  window_lock();
  switch(op){
   case 0:window_enqueue_event_locked(w,in);break;
   case 2:window_enqueue_resize_event_locked(w);break;
   case 3:window_enqueue_minimized_event_locked(w);break;
   case 4:window_enqueue_close_request_locked(w);break;
   case 5:window_enqueue_moved_event_locked(w);break;
  }
  window_unlock();
 }
 assert(w->event_count==count && w->event_write==(w->event_read+count)%64 && wakes==expected_wakes);
 assert(w->canary_before==0x123456abcdefULL && w->canary_after==0xfedcba654321ULL);
 for(unsigned i=0;i<count;++i)assert(!memcmp(&w->events[(w->event_read+i)%64],&expected[i],sizeof(*expected)));
}
static void setup(window_server_window_t *w,unsigned read,unsigned depth,unsigned pattern){
 memset(w,0xa5,sizeof(*w));w->identifier=17;w->owner=w;
 w->flags=pattern&1?OS_WINDOW_CLIENT_DECORATIONS:0;
 w->width=945;w->height=1060;w->buffer_size=0xabcdef987ULL;
 w->x=pattern&2?INT32_MIN:10;w->y=pattern&4?INT32_MAX:20;w->minimized=pattern&1;
 w->event_read=read;w->event_write=(read+depth)%64;w->event_count=depth;
 w->canary_before=0x123456abcdefULL;w->canary_after=0xfedcba654321ULL;
 count=depth;wakes=expected_wakes=0;
 for(unsigned i=0;i<depth;++i){
  os_window_event_t e={.identifier=pattern==7?18:17,.type=(i+pattern)%4};
  e.input.timestamp=100+i;e.pointer_x=-321;e.pointer_y=654;
  expected[i]=e;w->events[(read+i)%64]=e;
 }
 g_window_server.pointer_x=pattern&2?UINT32_MAX:100;
 g_window_server.pointer_y=pattern&4?0:200;
}
static void apply_control(window_server_window_t *w,input_event_t *input,unsigned target,unsigned action,int x,int y){
 os_window_event_t e={.identifier=w->identifier,.type=OS_WINDOW_EVENT_CONTROL,
  .control={.timestamp=input->timestamp,.target=target,.code=input->code,.value=input->value,
   .action=action,.flags=input->flags,.node_index=target%4096,.node_kind=OS_CONTROL_BUTTON,
   .tree_revision=test_revision,.user_data=0xaabbcc0000000000ULL|target},.pointer_x=x,.pointer_y=y};
 bool merge=action==OS_CONTROL_MOVE&&count&&expected[count-1].identifier==w->identifier&&
  expected[count-1].type==OS_WINDOW_EVENT_CONTROL&&expected[count-1].control.action==OS_CONTROL_MOVE&&
  expected[count-1].control.target==target&&expected[count-1].control.flags==input->flags&&
  expected[count-1].control.tree_revision==test_revision;
 if(merge)expected[count-1]=e;
 else{
  if(oracle_room(w,action==OS_CONTROL_MOVE)){expected[count++]=e;++expected_wakes;}
 }
 window_lock();window_enqueue_control_event_locked(w,input,target,action,x,y);window_unlock();
 assert(w->event_count==count&&w->event_write==(w->event_read+count)%64&&wakes==expected_wakes);
 assert(w->canary_before==0x123456abcdefULL&&w->canary_after==0xfedcba654321ULL);
 for(unsigned i=0;i<count;++i)assert(!memcmp(w->events+(w->event_read+i)%64,expected+i,sizeof(*expected)));
}
int main(void){
 _Static_assert(sizeof(os_window_event_t)==64,"event ABI");
 window_server_window_t w;input_event_t input={.timestamp=123456,.device_id=5,.type=OS_INPUT_EVENT_BUTTON,.flags=3,.code=OS_INPUT_BUTTON_LEFT,.value=1};
 unsigned cases=0;
 for(unsigned read=0;read<64;++read)for(unsigned depth=0;depth<=64;++depth)
 for(unsigned pattern=0;pattern<8;++pattern)for(unsigned op=0;op<6;++op){
  setup(&w,read,depth,pattern);apply(&w,op,&input);++cases;
  // Exercise repeated close deduplication and resize coalescing after the first append.
  if(op==2 || op==4 || op==5){w.x=INT32_MAX;w.y=INT32_MIN;apply(&w,op,&input);++cases;}
 }
 setup(&w,63,0,0);uint32_t random=0xabcdef;
 for(unsigned i=0;i<200000;++i){
  random=random*1664525U+1013904223U;
  w.width=random;w.height=~random;w.buffer_size=(uint64_t)random<<32|i;
  w.minimized=i&1;input.timestamp=i+1;
  w.x=(int32_t)random;w.y=(int32_t)~random;
  apply(&w,random%6,&input);++cases;
  if(i%7==0 && count){w.event_read=(w.event_read+1)%64;--w.event_count;memmove(expected,expected+1,(count-1)*sizeof(*expected));--count;}
 }
 // Invalid public dispatch never modifies the queue or publishes a wake.
 registered=&w;unsigned before=wakes;
 assert(window_server_dispatch(w.identifier,NULL)==K_EINVAL);
 assert(window_server_dispatch(999,&input)==K_ENOENT);
 w.owner=NULL;assert(window_server_dispatch(w.identifier,&input)==K_ENOENT);w.owner=&w;
 initialized=false;assert(window_server_dispatch(w.identifier,&input)==K_EINVAL);initialized=true;
 assert(wakes==before && global_wakes==flushes);
 window_lock();window_enqueue_event_locked(NULL,&input);window_enqueue_event_locked(&w,NULL);
 window_enqueue_client_input_locked(NULL,&input);window_enqueue_client_input_locked(&w,NULL);
 window_enqueue_resize_event_locked(NULL);window_enqueue_close_request_locked(NULL);window_enqueue_minimized_event_locked(NULL);window_unlock();
 window_lock();window_enqueue_moved_event_locked(NULL);window_unlock();
 assert(wakes==before);
 /* 控件事件复用同一队列，检查所有环形边界及饱和淘汰后的负载和坐标。 */
 for(unsigned read=0;read<64;++read)for(unsigned depth=0;depth<=64;++depth){
  setup(&w,read,depth,0);
  window_lock();window_enqueue_control_event_locked(&w,&input,789,OS_CONTROL_CLICK,-12,456);window_unlock();
  os_window_event_t *last=&w.events[(w.event_write+63)%64];
  assert(last->type==OS_WINDOW_EVENT_CONTROL&&last->control.target==789&&last->control.action==OS_CONTROL_CLICK);
  assert(last->control.timestamp==input.timestamp&&last->control.code==input.code&&last->control.value==input.value);
  assert(last->control.flags==input.flags&&last->pointer_x==-12&&last->pointer_y==456);
  assert(w.event_count==(depth==64?64:depth+1));
 }
 /* 包括满队列与环回边界：合并必须原位更新，不能多占槽位、淘汰旧事件或重复安排唤醒。 */
 for(unsigned read=0;read<64;++read)for(unsigned depth=0;depth<=64;++depth){
  setup(&w,read,depth,0);
  for(unsigned action=OS_CONTROL_ENTER;action<=OS_CONTROL_BLUR;++action){
   input.flags=3;input.code=OS_INPUT_REL_X;input.value=4;
   apply_control(&w,&input,789,action,-100,99);
   for(unsigned move=0;move<8;++move){
    ++input.timestamp;input.code=move%2;input.value=(int)move-4;
    apply_control(&w,&input,789,OS_CONTROL_MOVE,move,move+5);
   }
   input.flags=7;apply_control(&w,&input,789,OS_CONTROL_MOVE,100,200);
   apply_control(&w,&input,790,OS_CONTROL_MOVE,200,300);
  }
 }
 /* 满队列各个环回位置：移动不能挤掉关键事件；淘汰中间快照后关键事件保持 FIFO。 */
 for(unsigned read=0;read<64;++read)for(unsigned victim=0;victim<64;++victim){
  setup(&w,read,64,0);input.flags=0;
  apply_control(&w,&input,789,OS_CONTROL_MOVE,1,2);
  assert(wakes==0&&count==64);
  unsigned slot=(read+victim)%64;
  expected[victim].identifier=w.identifier;expected[victim].type=OS_WINDOW_EVENT_CONTROL;
  expected[victim].control.action=OS_CONTROL_MOVE;w.events[slot]=expected[victim];
  if(victim&1)apply_control(&w,&input,789,OS_CONTROL_CLICK,10,10);
  else apply(&w,0,&input);
  assert(count==64&&wakes==1);
 }
 /* 一次点击之后跟随大量移动，点击仍在队首；移动只占一个槽位。 */
 setup(&w,63,0,0);input.flags=0;
 apply_control(&w,&input,789,OS_CONTROL_CLICK,10,10);
 for(unsigned i=0;i<10000;++i){++input.timestamp;apply_control(&w,&input,789,OS_CONTROL_MOVE,i,i);}
 assert(count==2&&expected[0].control.action==OS_CONTROL_CLICK&&wakes==2);
 window_lock();window_enqueue_control_event_locked(NULL,&input,1,OS_CONTROL_MOVE,0,0);
 window_enqueue_control_event_locked(&w,NULL,1,OS_CONTROL_MOVE,0,0);window_unlock();
 assert(wakes==2&&w.event_count==2);
 /* 同一标识在重提交后可能指向不同节点；不同版本的移动不能互相覆盖。 */
 setup(&w,63,0,0);
 apply_control(&w,&input,789,OS_CONTROL_MOVE,10,10);
 ++test_revision;
 apply_control(&w,&input,789,OS_CONTROL_MOVE,20,20);
 assert(count==2&&expected[0].control.tree_revision!=expected[1].control.tree_revision);
 assert(expected[0].control.node_index==789&&expected[1].control.node_index==789);
 assert(expected[1].control.user_data==(0xaabbcc0000000000ULL|789));
 /* 物理事件复用场景结果；定向注入重新查询；键盘不触发指针扫描。 */
 setup(&w,0,0,0);registered=&w;input.type=OS_INPUT_EVENT_BUTTON;
 g_window_server.scene.hovered_window_identifier=w.identifier;
 unsigned queries=pointer_queries;
 window_lock();window_enqueue_event_locked(&w,&input);window_unlock();
 assert(routed_inside&&routed_prepared&&pointer_queries==queries);
 g_window_server.scene.hovered_window_identifier=0;
 window_lock();window_enqueue_event_locked(&w,&input);window_unlock();
 assert(!routed_inside&&routed_prepared&&pointer_queries==queries);
 window_lock();window_enqueue_client_input_locked(&w,&input);window_unlock();
 assert(routed_inside&&!routed_prepared&&pointer_queries==queries+1);
 input.type=OS_INPUT_EVENT_KEY;
 window_lock();window_enqueue_event_locked(&w,&input);window_unlock();
 assert(routed_inside&&routed_prepared&&pointer_queries==queries+1);
 printf("Window queue: %u boundary/mixed FIFO comparisons; control MOVE snapshots coalesce at all ring/full boundaries; click/key/wheel/focus/target/modifier barriers and 10000-motion click retention and 4096 full-queue motion-eviction/key-retention cases passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-window-queue-') as directory:
    path = Path(directory)
    def run(name, code, fail=False):
        (path/(name+'.c')).write_text(code)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I'+str(root/'include'), str(path/(name+'.c')), '-o', str(path/name)], check=True)
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE,(0,0))
        result = subprocess.run([str(path/name)], text=True, capture_output=True, preexec_fn=no_core)
        if fail:
            assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
            print('Negative control rejected: '+name)
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
    run('production', program)
    if args.negative_controls:
        run('critical-event-evicts-key-before-motion',program.replace(
            'window->event_count >= WINDOW_EVENT_CAPACITY &&\n        !window_event_discard_motion_locked(window)',
            'window->event_count >= WINDOW_EVENT_CAPACITY'),True)
        run('new-motion-evicts-critical-event',program.replace(
            'if(action==OS_CONTROL_MOVE && window->event_count>=WINDOW_EVENT_CAPACITY &&',
            'if(false && window->event_count>=WINDOW_EVENT_CAPACITY &&'),True)
        run('missing-control-move-coalescing',program.replace(
            'if (action == OS_CONTROL_MOVE && window->event_count)',
            'if (false && window->event_count)'),True)
        run('control-move-crosses-barrier',program.replace(
            'last->control.action == OS_CONTROL_MOVE && last->control.target == target &&',
            'last->control.target == target &&'),True)
        run('control-move-crosses-tree-revision',program.replace(
            'last->control.tree_revision == control.tree_revision &&', ''),True)
        run('uncleared-slot', program.replace('*queued = (os_window_event_t){0};', '(void)queued;'), True)
        run('duplicate-close', program.replace('queued->type == OS_WINDOW_EVENT_CLOSE_REQUEST) return;',
                                             'queued->type == OS_WINDOW_EVENT_CLOSE_REQUEST) break;'), True)
        run('missing-pointer-snapshot', program.replace('queued->input = *event;\n    window_event_set_pointer_locked(window, queued);',
                                                       'queued->input = *event;'), True)
