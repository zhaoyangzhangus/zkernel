#!/usr/bin/env python3
"""Production epoll description lifecycle, fork aliases and teardown races."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
types = source[source.index('typedef struct linux_epoll_watch {'):
               source.index('typedef struct linux_eventfd_object {')]
helpers = source[source.index('static void linux_description_put('):
                 source.index('static const object_ops_t g_linux_epoll_ops')]
probe = 'linux_epoll_watch_t *watch = &epoll->watches[index];'
assert helpers.count(probe) == 1
helpers = helpers.replace(probe, '++prune_slot_visits; ' + probe)
adopt = source[source.index('static void linux_epoll_watch_adopt('):
               source.index('static int64_t linux_epoll_ctl(')]
entry_put = source[source.index('static void linux_fd_entry_put(linux_fd_entry_t *entry) {'):
                   source.index('static void linux_unix_channel_destroy(')]
tables = source[source.index('void linux_fd_table_reset_stdio('):
                source.index('static kstatus_t linux_fd_set_flags(')]
dup = source[source.index('static int64_t linux_dup('):
             source.index('static int64_t linux_fstat(')]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <string.h>
#define OS_DEBUG_SERIAL 0
#define OS_LINUX_FD_LIMIT 8
#define LINUX_O_CLOEXEC 0x80000
#define LINUX_FD_CLOEXEC 1
#define K_OK 0
#define K_EMFILE -24
#define K_EBADF -9
#define K_EINVAL -22
#define K_ENOMEM -12
typedef int kstatus_t;
#define LINUX_MAX_EPOLL_WATCHES 512
enum { LINUX_FD_UNUSED, LINUX_FD_FILE, LINUX_FD_PROC, LINUX_FD_NVIDIA,
       LINUX_FD_PIPE, LINUX_FD_SOCKET, LINUX_FD_UNIX_SOCKET,
       LINUX_FD_WINDOW, LINUX_FD_EVENTFD, LINUX_FD_EPOLL, LINUX_FD_CONSOLE, LINUX_FD_NULL };
typedef pthread_mutex_t spinlock_t;
typedef struct { atomic_uint refs; bool epoll; } object_header_t;
typedef struct { int record_lock_waitq; } vnode_t;
typedef struct { object_header_t object; vnode_t *vnode; } file_t;
typedef struct { file_t *file; void *private_data; uint8_t kind; uint32_t flags;
 struct linux_fd_description *description; } linux_fd_entry_t;
typedef struct { spinlock_t lock; linux_fd_entry_t entries[8]; } linux_fd_table_t;
typedef struct { uint64_t pid; linux_fd_table_t linux_fds; } process_t;
typedef struct { uint32_t events; uint64_t data; } linux_epoll_event_t;
static atomic_uint allocations;
static unsigned prune_slot_visits;
static void spinlock_init(spinlock_t *l) { assert(!pthread_mutex_init(l,NULL)); }
static void spinlock_lock(spinlock_t *l) { assert(!pthread_mutex_lock(l)); }
static void spinlock_unlock(spinlock_t *l) { assert(!pthread_mutex_unlock(l)); }
static void *alloc(size_t n) { void *p=calloc(1,n);assert(p);++allocations;return p; }
static void kfree(void *p) { if(p) { assert(allocations);--allocations;free(p); } }
static void linux_epoll_destroy(void *);
static bool object_try_get(void *p) {
 object_header_t *h=p;unsigned n=atomic_load(&h->refs);
 while(n && !atomic_compare_exchange_weak(&h->refs,&n,n+1)) {}
 return n!=0;
}
static void object_get(void *p) { if(p) assert(object_try_get(p)); }
static void object_put(void *p) {
 if(!p)return;
 object_header_t *h=p;assert(atomic_load(&h->refs));
 if(atomic_fetch_sub(&h->refs,1)==1 && h->epoll)linux_epoll_destroy(p);
}
static void object_notify_changed(void *p) { assert(p); }
static void file_lock_release_file(vnode_t *node, uint64_t pid) { assert(node); (void)pid; }
static unsigned wake_all(int *queue) { assert(queue); return 0; }
static void linux_file_put(void *p) { object_put(p); }
typedef void linux_proc_file_t;
typedef void nv_frontend_file_t;
static void linux_proc_file_put(void *p) { object_put(p); }
static void nv_frontend_file_put(void *p) { object_put(p); }
static void nv_frontend_file_get(void *p) { object_get(p); }
static void linux_proc_file_get(void *p) { object_get(p); }
typedef void window_server_window_t;
static void window_server_put(void *p) { object_put(p); }
static bool fail_description_allocation;
static void *kzalloc(size_t n,unsigned flags) {
 (void)flags;if(fail_description_allocation)return NULL;return alloc(n);
}
static bool linux_fd_valid(int64_t fd) { return fd>=0 && fd<8; }
static unsigned g_linux_fd_exhaustion_logs;
static void kernel_serial_printf_serial_only(const char *fmt,...) { (void)fmt; }
''' + types + helpers + entry_put + tables + dup + adopt + r'''
static linux_fd_description_t *new_description(void) {
 linux_fd_description_t *d=alloc(sizeof(*d));
 atomic_init(&d->references,1);atomic_init(&d->external_references,1);
 spinlock_init(&d->lock);return d;
}
static linux_epoll_object_t *new_epoll(void) {
 linux_epoll_object_t *e=alloc(sizeof(*e));
 atomic_init(&e->object.refs,1);e->object.epoll=true;spinlock_init(&e->lock);return e;
}
static void attach(linux_epoll_object_t *e,unsigned slot,linux_fd_entry_t *target,int fd) {
 linux_description_get_external(target->description);object_get(target->private_data);
 spinlock_lock(&e->lock);
 linux_epoll_event_t event={.events=1,.data=(uint64_t)fd};
 linux_epoll_watch_adopt(e->watches+slot,e,fd,target,&event);
 if(slot>=e->watch_limit)e->watch_limit=slot+1;
 spinlock_unlock(&e->lock);linux_description_put_external(target->description);
 assert(linux_epoll_watch_matches_entry(e->watches+slot,target));
}
static void alias_get(linux_fd_entry_t *e) {
 object_get(e->private_data);linux_description_get_external(e->description);
}
static void alias_put(linux_fd_entry_t *e) {
 linux_description_put_external(e->description);object_put(e->private_data);
}
static void *close_alias(void *p) { alias_put(p);return NULL; }
static void *close_epoll(void *p) { object_put(p);return NULL; }
int main(void) {
 object_header_t event={0};atomic_init(&event.refs,1);
 linux_fd_entry_t parent={.private_data=&event,.kind=LINUX_FD_EVENTFD,
                          .description=new_description()};
 linux_fd_entry_t child=parent;alias_get(&child);
 linux_fd_entry_t duplicate=child;alias_get(&duplicate);
 linux_epoll_object_t *epoll=new_epoll(), *other=new_epoll();
 attach(epoll,0,&parent,4);attach(other,0,&parent,7);
 alias_put(&child);assert(epoll->watches[0].used && other->watches[0].used);
 alias_put(&duplicate);assert(epoll->watches[0].used);
 linux_fd_entry_t queued=parent;alias_get(&queued);
 alias_put(&parent);assert(epoll->watches[0].used);
 linux_fd_entry_t replacement={.private_data=&event,.kind=LINUX_FD_EVENTFD,
                               .description=new_description()};
 assert(!linux_epoll_watch_matches_entry(epoll->watches,&replacement));
 linux_description_put_external(replacement.description);
 alias_put(&queued);assert(!epoll->watch_limit && !other->watch_limit);
 assert(!atomic_load(&event.refs));object_put(epoll);object_put(other);
 assert(!allocations);
 atomic_store(&event.refs,1);parent.description=new_description();epoll=new_epoll();
 attach(epoll,0,&parent,4);attach(epoll,1,&parent,5);
 spinlock_lock(&epoll->lock);linux_epoll_watch_unlink(epoll->watches);
 linux_epoll_watch_t removed=epoll->watches[0];epoll->watches[0]=(linux_epoll_watch_t){0};
 spinlock_unlock(&epoll->lock);linux_epoll_watch_put(&removed);
 assert(parent.description->watches==epoll->watches+1);
 alias_put(&parent);object_put(epoll);assert(!allocations && !atomic_load(&event.refs));
 /* 同一描述的重复注册与其他描述交错排列，关闭时只扫描一次，且保留其他监听项。 */
 for(unsigned stride=1;stride<=8;stride*=2) {
  object_header_t second={0};atomic_init(&second.refs,1);
  atomic_store(&event.refs,1);parent.description=new_description();epoll=new_epoll();
  linux_fd_entry_t unrelated={.private_data=&second,.kind=LINUX_FD_EVENTFD,
                             .description=new_description()};
  for(unsigned i=0;i<LINUX_MAX_EPOLL_WATCHES;++i)
   attach(epoll,i,i%stride?&unrelated:&parent,4+i);
  prune_slot_visits=0;uint64_t sequence=epoll->control_sequence;
  alias_put(&parent);
  assert(prune_slot_visits<=LINUX_MAX_EPOLL_WATCHES);
  assert(epoll->control_sequence-sequence==LINUX_MAX_EPOLL_WATCHES/stride);
  assert(!atomic_load(&event.refs));
  for(unsigned i=0;i<LINUX_MAX_EPOLL_WATCHES;++i)
   assert(epoll->watches[i].used==(i%stride!=0));
  alias_put(&unrelated);assert(!epoll->watch_limit&&!atomic_load(&second.refs));
  object_put(epoll);assert(!allocations);
 }
 puts("Last-close pruning: 512 duplicate interests use 512 instead of 131328 slot visits; mixed descriptions survive PASS");
 for(unsigned i=0;i<1000;++i) {
  atomic_store(&event.refs,1);parent.description=new_description();epoll=new_epoll();
  attach(epoll,0,&parent,4);
  pthread_t a,b;assert(!pthread_create(&a,NULL,close_alias,&parent));
  assert(!pthread_create(&b,NULL,close_epoll,epoll));
  assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
 assert(!allocations && !atomic_load(&event.refs));
 }
 /* 覆盖真实的描述符安装/查找/fork/dup/exec/exit 路径，而不只是模拟外部引用操作。 */
 process_t p={0},c={0};linux_fd_table_init(&p.linux_fds);linux_fd_table_init(&c.linux_fds);
 /* 分配失败时不得发布描述符，也不得消耗调用者的对象引用。表已满时必须释放新分配的元数据。 */
 atomic_store(&event.refs,1);fail_description_allocation=true;
 assert(linux_fd_install(&p,4,0,&event,LINUX_FD_EVENTFD,0)==K_ENOMEM);
 assert(p.linux_fds.entries[4].kind==LINUX_FD_UNUSED);
 assert(atomic_load(&event.refs)==1 && !allocations);
 fail_description_allocation=false;
 for(unsigned i=3;i<8;++i)p.linux_fds.entries[i].kind=LINUX_FD_NULL;
 assert(linux_fd_install(&p,-1,0,&event,LINUX_FD_EVENTFD,0)==K_EMFILE);
 assert(atomic_load(&event.refs)==1 && !allocations);
 for(unsigned i=3;i<8;++i)p.linux_fds.entries[i]=(linux_fd_entry_t){0};
 atomic_store(&event.refs,1);epoll=new_epoll();
 assert(linux_fd_install(&p,3,0,epoll,LINUX_FD_EPOLL,LINUX_O_CLOEXEC)==3);
 assert(linux_fd_install(&p,4,0,&event,LINUX_FD_EVENTFD,LINUX_O_CLOEXEC)==4);
 attach(epoll,0,&p.linux_fds.entries[4],4);
 assert(linux_fd_table_clone(&p.linux_fds,&c.linux_fds)==K_OK);
 linux_fd_entry_t detached={0};assert(linux_fd_detach(&c,4,&detached)==K_OK);
 linux_fd_entry_put(&detached);assert(epoll->watches[0].used);
 assert(linux_dup_min(&p,4,5,0)==5);
 assert(linux_dup(&p,4,6,0)==6);
 linux_fd_table_close_on_exec(&c);assert(epoll->watches[0].used);
 linux_fd_table_close_on_exec(&p); /* epoll owner closes; destructor cleans the interests */
 assert(atomic_load(&event.refs)==2);
 linux_fd_table_destroy(&p.linux_fds);linux_fd_table_destroy(&c.linux_fds);
 assert(!allocations && !atomic_load(&event.refs));
 puts("Fork/dup/SCM pins, cross-epoll last close, fd reuse, DEL and 1000 teardown races passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-epoll-fork-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-pthread', '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
    # 恢复每次删除后从零扫描，必须触发有界扫描断言。
    marker = 'uint32_t index = next_index; index < epoll->watch_limit;'
    assert code.count(marker) == 1
    negative = code.replace(marker, 'uint32_t index = 0U * next_index; index < epoll->watch_limit;')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-pthread', '-x', 'c', '-', '-o', binary], input=negative, text=True, check=True)
    result = subprocess.run([binary],capture_output=True,text=True,timeout=30,
        preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode == -signal.SIGABRT and 'prune_slot_visits<=LINUX_MAX_EPOLL_WATCHES' in result.stderr
    print('负面对照通过：恢复 epoll 删除后的从头查找会超出单次扫描预算')
