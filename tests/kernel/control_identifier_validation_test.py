"""用独立两两比较基准和比较次数上界验证 ID 唯一性，包含只读输入和敌对排列。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'include/uapi/control_validation.h').read_text()
comparison = 'return nodes[a].identifier>nodes[b].identifier;'
assert comparison in header
header = header.replace('#pragma once', '').replace('#include "control_tree.h"', '#include <uapi/control_tree.h>')
header = header.replace(comparison, '++comparisons; ' + comparison)
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint64_t comparisons;
'''
fixture = r'''
static os_control_node_t nodes[OS_CONTROL_TREE_MAX_NODES],saved[OS_CONTROL_TREE_MAX_NODES];
static uint32_t random_state=0x19850713;
static uint32_t random_next(void){random_state=random_state*1664525U+1013904223U;return random_state;}
static bool oracle(unsigned count) {
 for(unsigned i=0;i<count;++i) {
  if(!nodes[i].identifier)return false;
  for(unsigned j=0;j<i;++j)if(nodes[i].identifier==nodes[j].identifier)return false;
 }
 return true;
}
static void check(unsigned count) {
 uint16_t guarded[OS_CONTROL_TREE_MAX_NODES+2];
 memcpy(saved,nodes,count*sizeof(*nodes));comparisons=0;
 bool expected=oracle(count);
 assert(os_control_tree_validate(nodes,count,0)==expected);
 assert(!memcmp(saved,nodes,count*sizeof(*nodes)));
 assert(comparisons<=32ULL*count);
 uint64_t standalone=comparisons;comparisons=0;
 memset(guarded,0xA5,sizeof(guarded));
 assert(os_control_tree_validate_indexed(nodes,count,0,guarded+1)==expected);
 assert(comparisons==standalone);
 assert(guarded[0]==0xA5A5);
 for(unsigned i=count+1;i<OS_CONTROL_TREE_MAX_NODES+2;++i)assert(guarded[i]==0xA5A5);
 if(expected)for(unsigned i=0;i<count;++i) {
  assert(guarded[i+1]<count);
  if(i)assert(nodes[guarded[i]].identifier<nodes[guarded[i+1]].identifier);
 }
 assert(!memcmp(saved,nodes,count*sizeof(*nodes)));
}
static void prepare(unsigned count,unsigned pattern) {
 for(unsigned i=0;i<count;++i) {
  uint32_t id=pattern==0?i+1:pattern==1?UINT32_MAX-i:
   pattern==2?(i&1?UINT32_MAX-i/2:i/2+1):2654435761U*(i+1);
  nodes[i]=(os_control_node_t){.identifier=id,.parent=i?0:OS_CONTROL_NO_PARENT,
   .kind=i?OS_CONTROL_BUTTON:OS_CONTROL_CONTAINER,.width=1,.height=1,
   .flags=i%3==0?OS_CONTROL_HIDDEN:i%3==1?OS_CONTROL_DISABLED:0};
 }
}
int main(void) {
 assert(!os_control_tree_validate_indexed(nodes,1,0,0));
 unsigned sizes[]={1,2,3,7,16,31,64,127,128,129,511,512,513,4095,4096};
 for(unsigned p=0;p<4;++p)for(unsigned s=0;s<sizeof(sizes)/sizeof(*sizes);++s) {
  unsigned count=sizes[s];prepare(count,p);check(count);
  if(!p)assert(!comparisons);
  if(count>1){nodes[count-1].identifier=nodes[0].identifier;check(count);}
 }
 for(unsigned trial=0;trial<1500;++trial) {
  unsigned count=1+random_next()%512;prepare(count,0);
  for(unsigned i=count;i>1;--i) {
   unsigned j=random_next()%i;uint32_t swap=nodes[i-1].identifier;
   nodes[i-1].identifier=nodes[j].identifier;nodes[j].identifier=swap;
  }
  check(count);
  if(count>1){unsigned j=1+random_next()%(count-1);nodes[j].identifier=nodes[0].identifier;check(count);}
 }
 prepare(OS_CONTROL_TREE_MAX_NODES,1);comparisons=0;check(OS_CONTROL_TREE_MAX_NODES);
 printf("Identifier validation: reverse 4096 comparison_count=%llu (previous=8386560)\n",
   (unsigned long long)comparisons);
 /* 排序必须只动局部索引，读取只读页的真实节点也能完成校验。 */
 size_t bytes=sizeof(nodes);
 void *read_only=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(read_only!=MAP_FAILED);memcpy(read_only,nodes,bytes);
 assert(!mprotect(read_only,bytes,PROT_READ));
 assert(os_control_tree_validate(read_only,OS_CONTROL_TREE_MAX_NODES,0));
 assert(!munmap(read_only,bytes));
 puts("Identifier validation: 4 adversarial orders, 1500 shuffled/duplicate pairs, read-only nodes and nlogn bound PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='control-identifier-') as directory:
    path = Path(directory)
    command = ['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-pie', '-no-pie', '-I' + str(root / 'include'),
               str(path / 'test.c'), '-o', str(path / 'test')]
    (path / 'test.c').write_text(prefix + header + fixture)
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
    old = 'for(uint32_t j=0;j<i;++j){++comparisons;if(nodes[j].identifier==node->identifier)return false;}'
    negative = header.replace('if (i && nodes[i-1U].identifier==node->identifier) return false;', old)
    negative = negative.replace('(ordered || os_control_identifiers_unique(nodes,count))', 'true')
    (path / 'test.c').write_text(prefix + negative + fixture)
    subprocess.run(command, check=True)
    result = subprocess.run([str(path / 'test')], capture_output=True, timeout=30)
    assert result.returncode != 0 and b'comparisons' in result.stderr, result.stderr
    print('Negative control rejected: quadratic duplicate scan')
