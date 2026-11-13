"""真实区间锁：单次合并扫描、逐字节参考模型和引用寿命。"""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/record_lock.c').read_text()
marker = 'record_lock_t *added = empty_slot_locked(g_records, &g_record_limit, FILE_LOCK_CAPACITY);'
start = source.index(marker)
loop = source.index('for (size_t i = 0; i < g_record_limit; ++i) {', start)
position = loop + len('for (size_t i = 0; i < g_record_limit; ++i) {')
query_loop = source.index('for (size_t i = 0; i < g_record_limit; ++i) {',
                          source.index('static record_lock_t *conflict_locked('))
query_position = query_loop + len('for (size_t i = 0; i < g_record_limit; ++i) {')
source = source[:query_position] + '\n        ++query_visits;' + source[query_position:]
# 前方插入查询计数后重新定位合并循环。
start = source.index(marker)
loop = source.index('for (size_t i = 0; i < g_record_limit; ++i) {', start)
position = loop + len('for (size_t i = 0; i < g_record_limit; ++i) {')
source = source[:position] + '\n            ++merge_visits;' + source[position:]
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/file_lock.h>
#include <uapi/linux_abi.h>
static unsigned merge_visits,query_visits;
static vnode_t nodes[2];
static unsigned pins[2];
void object_get(void *p){assert(p==nodes||p==nodes+1);++pins[(vnode_t *)p-nodes];}
void object_put(void *p){assert(p==nodes||p==nodes+1);unsigned i=(vnode_t *)p-nodes;assert(pins[i]);--pins[i];}
uint32_t wake_all(wait_queue_t *q){assert(q);return 0;}
'''
suffix = r'''
#define OWNERS 4U
#define BYTES 32U
static short model[2][OWNERS][BYTES];
static void clear(void){
 for(unsigned owner=1;owner<=OWNERS;++owner)file_lock_release_owner(owner);
 assert(!pins[0]&&!pins[1]&&!g_record_limit);memset(model,0,sizeof(model));
}
static void verify(void){
 assert(g_record_limit<=FILE_LOCK_CAPACITY);
 if(g_record_limit)assert(g_records[g_record_limit-1].node);
 for(size_t i=g_record_limit;i<FILE_LOCK_CAPACITY;++i)assert(!g_records[i].node);
 short actual[2][OWNERS][BYTES]={0};unsigned references[2]={0};
 for(unsigned i=0;i<FILE_LOCK_CAPACITY;++i){
  record_lock_t *r=g_records+i;if(!r->node)continue;
  unsigned n=r->node-nodes,o=r->range.owner-1;
  assert(n<2&&o<OWNERS&&r->range.first>=0&&r->range.last<BYTES);
  short type=r->range.type+1;assert(type==1||type==2);++references[n];
  for(int64_t b=r->range.first;b<=r->range.last;++b){assert(!actual[n][o][b]);actual[n][o][b]=type;}
  /* 每个边界外的字节都不能继续是同类型锁，否则区间尚未规范化。 */
  if(r->range.first)assert(model[n][o][r->range.first-1]!=type);
  if(r->range.last+1<BYTES)assert(model[n][o][r->range.last+1]!=type);
 }
 assert(!memcmp(actual,model,sizeof(model)));
 assert(references[0]==pins[0]&&references[1]==pins[1]);
}
static uint32_t random_state=0x581bU;
static unsigned next(void){random_state=random_state*1664525U+1013904223U;return random_state>>8;}
int main(void){
 /* 查询只遍历已使用范围；覆盖空表、小表、满表以及两种关闭路径的表尾回收。 */
 const unsigned counts[]={0,1,8,64,FILE_LOCK_CAPACITY};
 for(unsigned sample=0;sample<sizeof(counts)/sizeof(*counts);++sample){
  unsigned count=counts[sample];
  for(unsigned i=0;i<count;++i){
   file_lock_range_t r={1,i*2,i*2,LINUX_F_RDLCK};assert(file_lock_set(nodes,&r)==K_OK);
  }
  assert(g_record_limit==count&&pins[0]==count);
  file_lock_range_t q={2,count*4,count*4,LINUX_F_WRLCK},found;
  query_visits=0;assert(file_lock_query(nodes,&q,&found)==K_OK);
  assert(found.type==LINUX_F_UNLCK&&query_visits==count);
  if(sample%2)file_lock_release_file(nodes,1);else file_lock_release_owner(1);
  assert(!g_record_limit&&!pins[0]);
 }
 /* 内部空洞不缩短表尾，删除尾部则立即回收连续空槽。 */
 for(unsigned i=0;i<8;++i){file_lock_range_t r={1,i*2,i*2,LINUX_F_RDLCK};assert(file_lock_set(nodes,&r)==K_OK);}
 file_lock_range_t hole={1,4,4,LINUX_F_UNLCK};assert(file_lock_set(nodes,&hole)==K_OK);
 assert(g_record_limit==8&&pins[0]==7);
 hole.first=hole.last=14;assert(file_lock_set(nodes,&hole)==K_OK&&g_record_limit==7);
 clear();
 /* 两个邻居放在表尾，覆盖旧实现每次合并从头扫描的最坏位置。 */
 for(unsigned i=0;i<FILE_LOCK_CAPACITY-3;++i){
  file_lock_range_t r={2,i*2,i*2,LINUX_F_RDLCK};assert(file_lock_set(nodes+1,&r)==K_OK);
 }
 file_lock_range_t r={1,0,1,LINUX_F_RDLCK};assert(file_lock_set(nodes,&r)==K_OK);
 r.first=4;r.last=5;assert(file_lock_set(nodes,&r)==K_OK);
 r.first=2;r.last=3;merge_visits=0;assert(file_lock_set(nodes,&r)==K_OK);
 assert(merge_visits==FILE_LOCK_CAPACITY&&pins[0]==1);
 file_lock_range_t q={2,0,5,LINUX_F_WRLCK},found;
 assert(file_lock_query(nodes,&q,&found)==K_OK&&found.first==0&&found.last==5);
 clear();
 /* INT64_MAX 邻接合并必须避免端点加一溢出。 */
 r=(file_lock_range_t){1,INT64_MAX-3,INT64_MAX-2,LINUX_F_WRLCK};assert(file_lock_set(nodes,&r)==K_OK);
 r.first=INT64_MAX-1;r.last=INT64_MAX;assert(file_lock_set(nodes,&r)==K_OK&&pins[0]==1);
 r.type=LINUX_F_UNLCK;r.first=INT64_MAX-2;r.last=INT64_MAX-1;
 assert(file_lock_set(nodes,&r)==K_OK&&pins[0]==2);clear();
 /* 满表时，中间改型需要拆成三段，必须原子失败，不能丢失原锁或发布变更。 */
 for(unsigned i=0;i<FILE_LOCK_CAPACITY;++i){
  r=(file_lock_range_t){1,i*4,i*4+2,LINUX_F_RDLCK};assert(file_lock_set(nodes,&r)==K_OK);
 }
 unsigned sequence=atomic_load(&nodes[0].record_lock_sequence);
 r=(file_lock_range_t){1,1,1,LINUX_F_WRLCK};assert(file_lock_set(nodes,&r)==K_ENOLCK);
 assert(pins[0]==FILE_LOCK_CAPACITY&&sequence==atomic_load(&nodes[0].record_lock_sequence));
 q=(file_lock_range_t){2,0,2,LINUX_F_WRLCK};
 assert(file_lock_query(nodes,&q,&found)==K_OK&&found.first==0&&found.last==2&&found.type==LINUX_F_RDLCK);
 clear();
 /* 多文件、多所有者、读写冲突、重叠覆盖、解锁和随机关闭，对照独立逐字节模型。 */
 for(unsigned step=0;step<30000;++step){
  unsigned n=next()%2,o=next()%OWNERS;
  if(next()%17==0){
   file_lock_release_file(nodes+n,o+1);memset(model[n][o],0,sizeof(model[n][o]));
  }else{
   unsigned a=next()%BYTES,b=next()%BYTES;if(a>b){unsigned t=a;a=b;b=t;}
   short type=next()%3;bool conflict=false;
   for(unsigned other=0;other<OWNERS;++other)if(other!=o)
    for(unsigned j=a;j<=b;++j)if(type&&model[n][other][j]&&(type==2||model[n][other][j]==2))conflict=true;
   r=(file_lock_range_t){o+1,a,b,type?type-1:LINUX_F_UNLCK};
   unsigned sequence=atomic_load(&nodes[n].record_lock_sequence);
   unsigned before=pins[n];merge_visits=0;kstatus_t status=file_lock_set(nodes+n,&r);
   assert(status==(conflict?K_EAGAIN:K_OK));assert(merge_visits<=FILE_LOCK_CAPACITY);
   if(conflict){assert(before==pins[n]&&sequence==atomic_load(&nodes[n].record_lock_sequence));}
   else for(unsigned j=a;j<=b;++j)model[n][o][j]=type;
  }
  verify();
 }
 clear();puts("record-lock bounded scans: query visits 512 -> 0/1/8/64 for sparse tables, full 512 preserved; merge 1533 -> 512 slot visits; 30000 byte-model operations, conflicts, normalized ranges, close, references and INT64_MAX PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='record-lock-merge-') as directory:
    binary = str(Path(directory)/'test')
    command = ['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-pie','-no-pie',
               '-I'+str(root/'include'),'-x','c','-','-o',binary]
    subprocess.run(command,input=prefix+source+suffix,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    for label, old, new in (
        ('查询扫描整张表', 'for (size_t i = 0; i < g_record_limit; ++i) {',
         'for (size_t i = 0; i < FILE_LOCK_CAPACITY; ++i) {'),
        ('关闭后未收缩表尾', 'while (*limit != 0U && !records[*limit - 1U].node)',
         'while (false && *limit != 0U && !records[*limit - 1U].node)'),
    ):
        assert old in source
        negative = source.replace(old,new,1)
        subprocess.run(command,input=prefix+negative+suffix,text=True,check=True)
        result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
            preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert result.returncode == -signal.SIGABRT and 'Assertion' in result.stderr,label
        print('负面对照通过：' + label)

    # 恢复旧扫描方式必须触发扫描预算断言，防止测试只验证结果而遗漏性能退化。
    mark = 'r->node = 0;\n                ++released;'
    assert source.count(mark)==1
    negative = source.replace(mark,mark+'\n                i = (size_t)-1;')
    subprocess.run(command,input=prefix+negative+suffix,text=True,check=True)
    result = subprocess.run([binary],capture_output=True,text=True,timeout=30,
        preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode == -signal.SIGABRT and 'merge_visits==FILE_LOCK_CAPACITY' in result.stderr
    print('负面对照通过：恢复合并后从头扫描会超出单次扫描预算')
