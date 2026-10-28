"""Actual indexed enumeration cursor and removal: linear scans, safe invalidation."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/process.c').read_text()
remove = source[source.index('static void process_registry_invalidate_cursors_locked('):
                source.index('static void process_parent_lock(')]
enumerate_body = source[source.index('kstatus_t process_enumerate('):
                        source.index('kstatus_t process_enumerate_thread(')]
enumerate_body = enumerate_body.replace('process_t *process = list_entry', '++visits; process_t *process = list_entry')
monitor = source[source.index('static process_t *process_monitor_find('):
                 source.index('static bool process_monitor_protected(')]
monitor = monitor.replace('process_t *candidate = list_entry', '++monitor_visits; process_t *candidate = list_entry')
insert_start = source.index('    process_registry_invalidate_cursors_locked();', source.index('kstatus_t process_create_named('))
insert_end = source.index('    process_registry_unlock();', insert_start)
insert = source[insert_start:insert_end]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <uapi/process.h>
#include <uapi/syscall.h>
#define K_OK 0
#define K_EINVAL -22
#define K_ENOENT -2
typedef int kstatus_t;
static kstatus_t sched_monitor_process(os_process_snapshot_t *s) {(void)s;return K_ENOENT;}
typedef struct list_head { struct list_head *next,*prev; } list_head_t;
#define list_entry(n,t,m) ((t *)((char *)(n)-offsetof(t,m)))
typedef struct process {
 list_head_t global_node;bool global_registered;unsigned refs;
 uint64_t pid,create_time_ns;struct process *parent;atomic_uint state;
 unsigned thread_count;int64_t exit_code;char name[OS_PROCESS_NAME_MAX];
} process_t;
static list_head_t g_process_registry;
static list_head_t *g_process_enumeration_cursor;
static uint32_t g_process_enumeration_index;
static list_head_t *g_process_monitor_cursor;
static unsigned registry,owner_lock,visits,destroyed;
static unsigned monitor_visits;
static process_t *detach_after_unlock;
static void list_init(list_head_t *h) { h->next=h->prev=h; }
static void list_add_tail(list_head_t *h,list_head_t *n) { n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n; }
static void list_del(list_head_t *n) { n->prev->next=n->next;n->next->prev=n->prev;list_init(n); }
static void object_get(process_t *p) { assert(registry && p->refs);++p->refs; }
static void object_put(process_t *p) { assert(!registry && !owner_lock && p->refs);if(!--p->refs) { ++destroyed;free(p); } }
static void process_registry_remove(process_t *process);
static void process_registry_lock(void) { assert(!registry);registry=1; }
static void process_registry_unlock(void) {
 assert(registry);registry=0;
 if(detach_after_unlock) {
  process_t *p=detach_after_unlock;detach_after_unlock=NULL;
  process_registry_remove(p);object_put(p); // drop caller ownership too
 }
}
static void process_parent_lock(process_t *p) { assert(!registry && p->refs && !owner_lock);owner_lock=1; }
static void process_parent_unlock(process_t *p) { assert(p && owner_lock);owner_lock=0; }
static void process_thread_lock(process_t *p) { process_parent_lock(p); }
static void process_thread_unlock(process_t *p) { process_parent_unlock(p); }
static void process_snapshot_name(process_t *p,char *out,size_t capacity) { assert(!registry && !owner_lock && p->refs);memcpy(out,p->name,capacity); }
'''
checks = r'''
static process_t *items[256];
static process_t *add_process(uint64_t pid) {
 process_t *process=calloc(1,sizeof(*process));assert(process);process->refs=1;
 process->pid=pid;process->create_time_ns=pid*10;process->thread_count=pid%17;process->exit_code=-(int64_t)pid;
 atomic_init(&process->state,OS_PROCESS_RUNNING);snprintf(process->name,sizeof(process->name),"p%llu",(unsigned long long)pid);
 process_registry_lock();object_get(process);
INSERT
 process_registry_unlock();return process;
}
static void check(unsigned index,uint64_t pid) {
 os_process_snapshot_t snapshot;
 assert(process_enumerate(index,&snapshot)==K_OK);
 assert(snapshot.pid==pid && !snapshot.parent_pid && snapshot.create_time_ns==pid*10);
 assert(snapshot.thread_count==pid%17 && snapshot.exit_code==-(int64_t)pid);
 char expected[OS_PROCESS_NAME_MAX];snprintf(expected,sizeof(expected),"p%llu",(unsigned long long)pid);
 assert(!strcmp(snapshot.name,expected) && !registry && !owner_lock);
}
static void clean(unsigned count) {
 for(unsigned i=0;i<count;++i) { process_registry_remove(items[i]);object_put(items[i]); }
 assert(g_process_registry.next==&g_process_registry && !g_process_enumeration_cursor && !g_process_monitor_cursor);
}
int main(void) {
 list_init(&g_process_registry);os_process_snapshot_t snapshot;unsigned queries=0;
 for(unsigned n=0;n<=256;++n) {
  for(unsigned i=0;i<n;++i)items[i]=add_process(i+1);
  visits=monitor_visits=0;
  for(unsigned i=0;i<n;++i) {
   check(i,i+1);++queries;
   process_t *p=process_monitor_find(i+1);assert(p==items[i] && p->refs==3);object_put(p);
  }
  assert(process_enumerate(n,&snapshot)==K_ENOENT && visits==n);
  assert(!monitor_visits); // enumeration-followed resource queries need no registry scan
  for(unsigned i=n;i>0;--i)check(i-1,i);
  assert(process_enumerate(UINT32_MAX,&snapshot)==K_ENOENT);
  assert(process_enumerate(0,NULL)==K_EINVAL);
  for(unsigned i=0;i<n;++i)assert(items[i]->refs==2);
  clean(n);
 }
 for(unsigned i=0;i<256;++i)items[i]=add_process(1000+i);
 monitor_visits=0;
 process_t *p=process_monitor_find(1255);assert(p==items[255]);object_put(p);
 assert(monitor_visits==256);
 for(unsigned i=0;i<10000;++i) {
  p=process_monitor_find(1255);assert(p==items[255] && p->refs==3);object_put(p);
 }
 assert(monitor_visits==256);
 assert(!process_monitor_find(9999) && monitor_visits==512);
 p=process_monitor_find(1255);assert(p==items[255]);object_put(p);assert(monitor_visits==512);
 uint32_t random=12345;
 for(unsigned i=0;i<5000;++i) { random=random*1664525U+1013904223U;unsigned index=random%300;
  if(index<256)check(index,1000+index);else assert(process_enumerate(index,&snapshot)==K_ENOENT);
 }
 clean(256);
 items[0]=add_process(1500);p=process_monitor_find(1500);assert(p==items[0]);object_put(p);
 process_registry_remove(items[0]);object_put(items[0]);
 assert(!process_monitor_find(1500)); // no dereference of the freed monitor hint
 // Removing the cached entry must not leave a pointer to freed memory.
 for(unsigned i=0;i<18;++i)items[i]=add_process(2000+i);
 check(7,2007);process_registry_remove(items[7]);object_put(items[7]);
 check(7,2008); // same index now refers to its successor
 for(unsigned i=0;i<18;++i)if(i!=7) { process_registry_remove(items[i]);object_put(items[i]); }
 // Insertion invalidates a cached prefix; returning to index zero is exact.
 items[0]=add_process(3000);check(0,3000);items[1]=add_process(3001);
 assert(!g_process_enumeration_cursor);check(1,3001);check(0,3000);clean(2);
 // Removal after registry unlock cannot destroy the pinned snapshot owner.
 items[0]=add_process(4000);items[1]=add_process(4001);
 unsigned before=destroyed;detach_after_unlock=items[0];check(0,4000);
 assert(destroyed==before+1);check(0,4001);
 process_registry_remove(items[1]);object_put(items[1]);
 items[0]=add_process(5000);before=destroyed;detach_after_unlock=items[0];
 p=process_monitor_find(5000);assert(p && p->pid==5000 && p->refs==1 && destroyed==before);
 object_put(p);assert(destroyed==before+1 && !process_monitor_find(5000));
 process_registry_remove(NULL);
 printf("%u sequential identities visit each process once; reverse/random, insertion, freed cursor and snapshot-owner removal passed\n",queries);
}
'''.replace('INSERT', insert)
with tempfile.TemporaryDirectory(prefix='liteos-enumerate-cursor-') as directory:
    path = Path(directory)
    def run(removal, enumeration, verification=checks, lookup=monitor):
        (path / 'test.c').write_text(fixture + removal + enumeration + lookup + verification)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I' + str(root / 'include'), str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True, text=True)
    result = run(remove, enumerate_body)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
    print('32896 resource lookups after enumeration skip list scans; 10000 repeated tail-PID lookups reuse one scan')
    negative_lookup = monitor.replace('if (hint != 0 && list_entry(hint, process_t, global_node)->pid == pid)', 'if (false)')
    result = run(remove, enumerate_body, lookup=negative_lookup)
    assert negative_lookup != monitor and result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: resource queries ignore valid PID hints')
    negative_walk = enumerate_body.replace('if (g_process_enumeration_cursor != 0 &&', 'if (false &&')
    result = run(remove, negative_walk)
    assert negative_walk != enumerate_body and result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: restarting every query at the list head')
    negative = remove.replace('        process_registry_invalidate_cursors_locked();', '')
    verification = checks.replace(' && !g_process_enumeration_cursor', '').replace(' && !g_process_monitor_cursor', '')
    result = run(negative, enumerate_body, verification)
    assert negative != remove and result.returncode != 0
    assert 'heap-use-after-free' in result.stderr, result.stderr
    print('Negative control rejected: removal retains a stale registry cursor')
    negative = remove.replace('    g_process_monitor_cursor = 0;', '')
    result = run(negative, enumerate_body, verification)
    assert negative != remove and result.returncode != 0 and 'heap-use-after-free' in result.stderr, result.stderr
    print('Negative control rejected: monitor hint survives owner removal')
