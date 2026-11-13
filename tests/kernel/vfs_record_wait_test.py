"""真实区间锁等待表：扫描边界、满表、死锁回滚及独立等待图对照。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/record_lock.c').read_text()
loop = 'for (size_t w = 0; w < g_wait_limit; ++w) {'
assert source.count(loop) == 1
source = source.replace(loop, loop + '\n            ++wait_visits;', 1)
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/file_lock.h>
#include <uapi/linux_abi.h>
static vnode_t nodes[5];
static unsigned pins,wait_visits;
void object_get(void *p){assert(p);++pins;}
void object_put(void *p){assert(p&&pins);--pins;}
uint32_t wake_all(wait_queue_t *q){assert(q);return 0;}
'''
suffix = r'''
static void hold(unsigned node,unsigned owner,int type){
 file_lock_range_t r={owner,0,10,type};assert(file_lock_set(nodes+node,&r)==K_OK);
}
static kstatus_t begin(unsigned node,unsigned owner,uint32_t *ticket){
 file_lock_range_t r={owner,0,10,LINUX_F_WRLCK};return file_lock_begin_wait(nodes+node,&r,ticket);
}
static void bounds(void){
 assert(g_wait_limit<=FILE_LOCK_WAIT_CAPACITY);
 if(g_wait_limit)assert(g_lock_waiters[g_wait_limit-1].node);
 for(size_t i=g_wait_limit;i<FILE_LOCK_WAIT_CAPACITY;++i)assert(!g_lock_waiters[i].node);
}
static struct {bool active;unsigned node,owner;uint32_t ticket;} model[256];
static uint32_t seed=371;
static unsigned next(void){seed=seed*1664525U+1013904223U;return seed>>8;}
/* 独立图模型：每个资源的持有者固定，边由测试请求生成，不读取生产等待表。 */
static unsigned blockers(unsigned node,unsigned owner){
 unsigned holders=node==4?3U:1U<<node;
 return holders&~(1U<<owner);
}
static bool cycle(unsigned node,unsigned owner){
 unsigned edges[8]={0};
 for(unsigned i=0;i<256;++i)if(model[i].active)
  edges[model[i].owner]|=blockers(model[i].node,model[i].owner);
 edges[owner]|=blockers(node,owner);
 unsigned reached=1U<<owner,frontier=reached;
 while(frontier){
  unsigned bit=(unsigned)__builtin_ctz(frontier);frontier&=frontier-1;
  unsigned targets=edges[bit];
  if(targets&(1U<<owner))return true;
  frontier|=targets&~reached;reached|=targets;
 }
 return false;
}
int main(void){
 uint32_t tickets[256],extra;
 hold(0,1,LINUX_F_WRLCK);
 for(unsigned i=0;i<256;++i){
  wait_visits=0;assert(begin(0,i+2,&tickets[i])==K_OK);
  assert(g_wait_limit==i+1&&wait_visits==2*(i+1));bounds();
 }
 assert(begin(0,999,&extra)==K_ENOLCK&&extra==UINT32_MAX);
 file_lock_end_wait(tickets[7]);assert(g_wait_limit==256);
 assert(begin(0,1001,&tickets[7])==K_OK);bounds();
 file_lock_release_owner(1001);assert(g_wait_limit==256);
 file_lock_release_owner(257);assert(g_wait_limit==255);
 for(unsigned i=255;i-->0;)file_lock_end_wait(tickets[i]);
 assert(!g_wait_limit);file_lock_release_owner(1);assert(!pins);
 /* 拒绝形成环的候选必须撤销；反复拒绝不能增长表尾或耗尽槽位。 */
 hold(0,1,LINUX_F_WRLCK);hold(1,2,LINUX_F_WRLCK);
 assert(begin(1,1,&tickets[0])==K_OK);
 for(unsigned i=0;i<2000;++i){
  assert(begin(0,2,&extra)==K_EDEADLK&&extra==UINT32_MAX);
  assert(g_wait_limit==1);bounds();
 }
 file_lock_end_wait(tickets[0]);
 file_lock_release_owner(1);file_lock_release_owner(2);assert(!pins&&!g_wait_limit);
 for(unsigned n=0;n<4;++n)hold(n,n+1,LINUX_F_WRLCK);
 hold(4,1,LINUX_F_RDLCK);hold(4,2,LINUX_F_RDLCK);
 unsigned active=0;
 for(unsigned step=0;step<6000;++step){
  if(active&&next()%3==0){
   unsigned i=next()%256;while(!model[i].active)i=(i+1)%256;
   file_lock_end_wait(model[i].ticket);model[i].active=false;--active;
  }else{
   unsigned node=next()%5,owner=next()%8;
   bool blocked=blockers(node,owner)!=0,deadlock=cycle(node,owner);
   kstatus_t expected=!blocked?K_OK:active==256?K_ENOLCK:deadlock?K_EDEADLK:K_OK;
   kstatus_t status=begin(node,owner+1,&extra);assert(status==expected);
   if(status==K_OK&&blocked){
    unsigned i=0;while(model[i].active)++i;
    model[i].active=true;model[i].node=node;model[i].owner=owner;model[i].ticket=extra;++active;
   }else assert(extra==UINT32_MAX);
  }
  bounds();unsigned actual=0;
  for(unsigned i=0;i<256;++i)actual+=g_lock_waiters[i].node!=0;
  assert(actual==active);
 }
 for(unsigned i=0;i<256;++i)if(model[i].active)file_lock_end_wait(model[i].ticket);
 for(unsigned owner=1;owner<=8;++owner)file_lock_release_owner(owner);
 assert(!pins&&!g_wait_limit&&!g_record_limit);
 puts("区间锁等待：单等待扫描 512 -> 2 项，满表/空洞/退出回收、2000 次死锁回滚和 6000 次独立图对照通过");
}
'''
with tempfile.TemporaryDirectory(prefix='record-wait-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=prefix + source + suffix, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    for label, old, new in (
        ('等待图扫描整张表', 'w < g_wait_limit;', 'w < FILE_LOCK_WAIT_CAPACITY;'),
        ('拒绝死锁后未回收', 'trim_slots_locked(g_lock_waiters, &g_wait_limit);', '(void)0;'),
    ):
        assert old in source
        broken = source.replace(old, new, 1)
        subprocess.run(command, input=prefix + broken + suffix, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, text=True, timeout=30,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert result.returncode != 0 and 'Assertion' in result.stderr, label
        print('反例被拒绝：' + label)
