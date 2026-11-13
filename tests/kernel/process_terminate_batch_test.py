"""Actual low-memory sibling termination: bounded pins and unordered lists."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/process.c').read_text()
body = source[source.index('static void process_terminate_threads_fallback('):
              source.index('void process_terminate_siblings(')]
body = body.replace('thread_t *candidate = list_entry', '++visits; thread_t *candidate = list_entry')
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#define THREAD_DEAD 4U
typedef struct list_head { struct list_head *next; } list_head_t;
#define list_entry(node,type,member) ((type *)((char *)(node)-offsetof(type,member)))
typedef struct { list_head_t threads; } process_t;
typedef struct { uint64_t tid; atomic_uint state; unsigned refs; bool seen; list_head_t process_node; } thread_t;
static thread_t threads[258];
static process_t process;
static unsigned held,locks,pins,peak,visits,terminated;
static uint64_t last;
static bool detach;
static void object_get(thread_t *t) {
 assert(held && t->refs); ++t->refs; ++pins; if(pins>peak)peak=pins;
}
static void object_put(thread_t *t) { assert(!held && t->refs && pins); --t->refs; --pins; }
static void process_thread_lock(process_t *p) { assert(p==&process && !held);held=1;++locks; }
static void process_thread_unlock(process_t *p) {
 assert(p==&process && held);
 if(detach) {
  list_head_t **link=&p->threads.next;
  while(*link!=&p->threads) {
   thread_t *t=list_entry(*link,thread_t,process_node);
   if(t->refs==2) { *link=t->process_node.next;--t->refs;atomic_store(&t->state,THREAD_DEAD); }
   else link=&t->process_node.next;
  }
 }
 held=0;
}
static int thread_terminate(thread_t *t,int64_t status) {
 assert(!held && status==-9 && t->refs && !t->seen && t->tid>last);
 assert(t->refs==(detach?1U:2U));t->seen=true;last=t->tid;++terminated;
 return -16;
}
'''
checks = r'''
int main(void) {
 unsigned cases=0;
 for(unsigned n=0;n<=256;++n)for(unsigned order=0;order<3;++order)for(unsigned mode=0;mode<2;++mode) {
  held=locks=pins=peak=visits=terminated=0;last=0;detach=mode;
  for(unsigned i=0;i<n+2;++i) {
   threads[i]=(thread_t){.tid=i+1,.refs=1};atomic_init(&threads[i].state,i==n+1?THREAD_DEAD:0);
  }
  // 调用者和已退出的同级进程仍会出现在每次扫描中，但绝不会被固定引用。
  process.threads.next=&threads[n].process_node;
  list_head_t *tail=&threads[n].process_node;
  for(unsigned i=0;i<n;++i) {
   unsigned j=order==0?i:order==1?n-1-i:(i%2==0?i/2:n-1-i/2);
   tail->next=&threads[j].process_node;tail=tail->next;
  }
  tail->next=&threads[n+1].process_node;threads[n+1].process_node.next=&process.threads;
  process_terminate_threads_fallback(&process,&threads[n],-9);
  assert(terminated==n && locks==n/16+1 && peak<=16 && !held && !pins);
  for(unsigned i=0;i<n;++i)assert(threads[i].seen && threads[i].refs==(detach?0U:1U));
  assert(!threads[n].seen && !threads[n+1].seen && threads[n].refs==1 && threads[n+1].refs==1);
  if(!detach)assert(visits==(n+2)*locks);
  ++cases;
 }
 // 可表示的最大 ID 只处理一次，游标不会溢出。
 detach=false;held=locks=pins=peak=visits=terminated=0;last=0;
 threads[0]=(thread_t){.tid=UINT64_MAX,.refs=1};atomic_init(&threads[0].state,0);
 process.threads.next=&threads[0].process_node;threads[0].process_node.next=&process.threads;
 process_terminate_threads_fallback(&process,0,-9);
 assert(terminated==1 && last==UINT64_MAX && locks==1 && !pins);
 printf("%u unordered/boundary/detach cases, max 16 pins, exact scan counts and UINT64_MAX: passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-terminate-batch-') as directory:
    path = Path(directory)
    def run(actual):
        (path / 'test.c').write_text(fixture + actual + checks)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True, text=True)
    result = run(body)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
    negative = body.replace('object_get(targets[index]);', '(void)object_get; (void)targets[index];')
    result = run(negative)
    assert negative != body and result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: batch pointer escapes without a reference')
