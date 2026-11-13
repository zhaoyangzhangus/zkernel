"""Actual FD installer: locked exhaustion snapshot across post-unlock closes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
header = (root / 'include/kernel/linux_abi.h').read_text()
kinds = header[header.index('enum linux_fd_kind {'):header.index('\n};')+3]
body = source[source.index('static int64_t linux_fd_install_description('):
              source.index('static kstatus_t linux_fd_detach(')]
fixture = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#define K_OK 0
#define K_EMFILE -24
#define K_EBADF -9
#define K_EINVAL -22
#define K_ENOMEM -12
#define OS_DEBUG_SERIAL 0
#define OS_LINUX_FD_LIMIT 8U
typedef struct { bool held; } spinlock_t;
typedef struct { unsigned refs; } file_t;
typedef struct { atomic_uint references,external_references;spinlock_t lock; } linux_fd_description_t;
typedef struct { file_t *file;void *private_data;uint32_t flags;uint8_t kind;
 linux_fd_description_t *description; } linux_fd_entry_t;
typedef struct { spinlock_t lock;linux_fd_entry_t entries[8]; } linux_fd_table_t;
typedef struct { uint64_t pid;linux_fd_table_t linux_fds; } process_t;
static bool held,fail_allocation;
static unsigned g_linux_fd_exhaustion_logs,locks,allocations,frees,logs,mutation;
static unsigned expected_kind;
static process_t *current;
static void linux_fd_release_locks(process_t *p, const linux_fd_entry_t *entry) {
 assert(p == current && held && entry);
}
static void linux_fd_notify_locks(const linux_fd_entry_t *entry) {
 assert(!held && entry);
}
static void spinlock_init(spinlock_t *l) { l->held=false; }
static void spinlock_lock(spinlock_t *l) { assert(!held && !l->held);held=l->held=true;++locks; }
static void spinlock_unlock(spinlock_t *l) {
 assert(held && l->held);held=l->held=false;
 if(l==&current->linux_fds.lock && mutation) {
  /* 模拟分配器释放表锁后，关闭方获取该表锁。 */
  unsigned count=mutation==1?1:8;
  for(unsigned i=0;i<count;++i)current->linux_fds.entries[i]=(linux_fd_entry_t){0};
  mutation=0;
 }
}
static bool linux_fd_valid(int64_t fd) { return fd>=0 && fd<8; }
static void *kzalloc(size_t n,unsigned flags) {
 assert(!held && !flags);if(fail_allocation)return NULL;
 ++allocations;return calloc(1,n);
}
static void linux_description_put_external(linux_fd_description_t *d) {
 assert(!held);if(!d)return;
 assert(atomic_fetch_sub(&d->external_references,1)==1);
 assert(atomic_fetch_sub(&d->references,1)==1);free(d);++frees;
}
static void linux_fd_entry_put(linux_fd_entry_t *e) {
 assert(!held);
 linux_description_put_external(e->description);
 if(e->file){assert(e->file->refs);--e->file->refs;}
 if(e->private_data){file_t *p=e->private_data;assert(p->refs);--p->refs;}
 *e=(linux_fd_entry_t){0};
}
static void kernel_serial_printf_serial_only(const char *format,...) {
 (void)format;assert(!held);++logs;va_list args;va_start(args,format);
 assert(va_arg(args,unsigned long long)==42);
 assert(va_arg(args,unsigned)==OS_LINUX_FD_LIMIT);
 assert(va_arg(args,unsigned)==expected_kind);va_end(args);
}
'''
checks = r'''
int main(void) {
 (void)linux_fd_install;
 unsigned cases=0;
 for(unsigned mode=0;mode<3;++mode)for(unsigned close=0;close<3;++close)
 for(unsigned budget=0;budget<4;++budget)for(unsigned supplied=0;supplied<2;++supplied) {
  process_t p={.pid=42};current=&p;
  for(unsigned i=0;i<8;++i)p.linux_fds.entries[i].kind=LINUX_FD_CONSOLE;
  file_t object={1};linux_fd_description_t description={0};
  atomic_init(&description.references,1);atomic_init(&description.external_references,1);
  expected_kind=mode==0?LINUX_FD_CONSOLE:mode==1?LINUX_FD_FILE:LINUX_FD_EVENTFD;
  unsigned initial=budget==0?0:budget==1?15:budget==2?16:100;
  g_linux_fd_exhaustion_logs=initial;allocations=frees=logs=locks=0;mutation=close;
  assert(linux_fd_install_description(&p,-1,mode==1?&object:NULL,
      mode==2?&object:NULL,expected_kind,0x80000,supplied?&description:NULL)==K_EMFILE);
  assert(locks==1 && !held && logs==(initial<16?1U:0U));
  assert(g_linux_fd_exhaustion_logs==initial+1 && object.refs==1);
  assert(allocations==(!supplied && mode!=0?1U:0U) && frees==allocations);
  assert(atomic_load(&description.references)==1 && atomic_load(&description.external_references)==1);
  unsigned empty=close==0?0:close==1?1:8;
  for(unsigned i=0;i<8;++i)assert(p.linux_fds.entries[i].kind==(i<empty?LINUX_FD_UNUSED:LINUX_FD_CONSOLE));
  /* 快照诊断不得改变已经返回的分配结果。
   * 后续调用可以正常重用刚关闭的槽位。 */
  if(close) {
   logs=0;unsigned previous_budget=g_linux_fd_exhaustion_logs;
   assert(linux_fd_install(&p,-1,NULL,NULL,LINUX_FD_CONSOLE,7)==0);
   assert(p.linux_fds.entries[0].flags==7 && !logs);
   assert(g_linux_fd_exhaustion_logs==previous_budget);
  }
  ++cases;
 }
 process_t p={.pid=42};current=&p;file_t object={1};
 allocations=frees=logs=locks=0;fail_allocation=true;
 assert(linux_fd_install(&p,-1,&object,NULL,LINUX_FD_FILE,0)==K_ENOMEM);
 assert(!locks && !logs && !allocations && object.refs==1);
 fail_allocation=false;
 assert(linux_fd_install(&p,8,&object,NULL,LINUX_FD_FILE,0)==K_EBADF);
 assert(linux_fd_install(NULL,-1,&object,NULL,LINUX_FD_FILE,0)==K_EINVAL);
 assert(!locks && !allocations);
 assert(linux_fd_install(&p,3,&object,NULL,LINUX_FD_FILE,9)==3);
 assert(p.linux_fds.entries[3].file==&object && p.linux_fds.entries[3].flags==9);
 assert(allocations==1 && !frees && object.refs==1);
 linux_fd_entry_put(&p.linux_fds.entries[3]);assert(frees==1 && !object.refs);
 printf("%u exhaustion/close/budget/metadata cases: locked snapshot, EMFILE, reference cleanup, reuse, OOM and fixed install passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fd-install-snapshot-') as directory:
    path = Path(directory)
    def run(actual):
        (path / 'test.c').write_text(kinds + fixture + actual + checks)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True, text=True)
    result = run(body)
    assert result.returncode == 0,result.stdout+result.stderr
    print(result.stdout.strip())
    negative = body.replace('        /* EMFILE means', '''        uint32_t occupied=0;
        for(unsigned i=0;i<OS_LINUX_FD_LIMIT;++i)
            if(process->linux_fds.entries[i].kind!=LINUX_FD_UNUSED)++occupied;
        /* EMFILE means''').replace('(unsigned)OS_LINUX_FD_LIMIT, (unsigned)kind',
                                    'occupied, (unsigned)kind')
    result = run(negative)
    assert result.returncode != 0 and 'va_arg(args,unsigned)==OS_LINUX_FD_LIMIT' in result.stderr,result.stderr
    print('Negative control rejected: post-unlock rescan reports a different table state')
