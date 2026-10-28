"""Actual direct I/O: completed progress survives backend/user-copy failures."""
from pathlib import Path
import subprocess
import tempfile
import re
import resource

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/file_io.c').read_text()
helpers = source[source.index('static kstatus_t vfs_write_transaction_begin('):source.index('kstatus_t vfs_truncate_node(')]
code = helpers + source[source.index('static void vfs_read_kernel_yield('):source.index('kstatus_t vfs_fsync(')]
program = r'''
#define _GNU_SOURCE
#include <assert.h>
/* 非缓存测试只桩化读取合并；映射一致性由单独的真实代码回归测试覆盖。 */
#define vfs_page_cache_present(vnode) ((void)(vnode), false)
#define K_ENOMEM (-12)
#define vfs_page_cache_buffer_alias(vnode,buffer,length) \
    ((void)(vnode),(void)(buffer),(void)(length),false)
#define vfs_backend_consistent_read_lock(backend) ((void)(backend), K_OK)
#define vfs_backend_consistent_read_unlock(backend) ((void)(backend))
#define vfs_page_cache_overlay_read(vnode,offset,buffer,length) \
    ((void)(vnode),(void)(offset),(void)(buffer),(void)(length),K_OK)
/* 单句柄测试只桩化偏移锁；共享句柄并发由专门的回归测试覆盖。 */
#define vfs_file_position_init(file) ((void)(file))
#define vfs_file_position_lock(file) ((void)(file), K_OK)
#define vfs_file_position_unlock(file) ((void)(file))
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
static void *kmalloc(size_t n,unsigned flags){(void)flags;return malloc(n);}
static void kfree(void *p){free(p);}
#define __user
#define PAGE_SIZE 4096U
#define KOBJECT_TYPE_FILE 1
#define VFS_OPEN_READ 1
#define VFS_OPEN_WRITE 2
#define VFS_OPEN_APPEND 32
#define K_OK 0
#define K_EIO -5
#define K_EINVAL -22
#define K_EACCES -13
#define K_EOVERFLOW -75
#define K_EAGAIN -11
typedef int kstatus_t;
typedef struct {
 void *context; uint64_t size;int write_mutex;
 kstatus_t (*read)(void *,uint64_t,void *,size_t,uint64_t *);
 kstatus_t (*write)(void *,uint64_t,const void *,size_t,uint64_t *);
} vfs_backend_node_t;
typedef struct { void *fs_private; uint64_t size;} vnode_t;
typedef struct {
 struct { unsigned type; } object; vnode_t *vnode;
 unsigned rights; uint64_t position;unsigned flags;
} file_t;
static unsigned calls,depth,copy_calls,cache_calls,locks;
static unsigned transaction_depth;
typedef struct {unsigned sched_class;} thread_t;
#define SCHED_CLASS_IDLE 2U
static thread_t current_thread={2U};
static thread_t *sched_current_thread(void){return &current_thread;}
static bool sched_preempt_disabled(void){return false;}
static void schedule(void){assert(!"idle test must not yield");}
static int kmutex_try_lock(int *m){(void)m;return K_EAGAIN;}
static int kmutex_lock(int *m,uint64_t timeout){(void)m;assert(timeout==UINT64_MAX&&!depth&&!transaction_depth);transaction_depth=1;return 0;}
static int kmutex_unlock(int *m){(void)m;assert(!depth&&transaction_depth);transaction_depth=0;return 0;}
static int io_mutex_lock(int *m){
 int status=kmutex_try_lock(m);return status==K_EAGAIN?kmutex_lock(m,UINT64_MAX):status;
}
static void io_mutex_unlock_required(int *m){assert(kmutex_unlock(m)==K_OK);}
static void vfs_vnode_lock(vnode_t *v){(void)v;assert(transaction_depth&&!depth);}
static void vfs_vnode_unlock(vnode_t *v){(void)v;}
static int failure_at,copy_failure_at,mode;
static bool wide;
static bool shrink_between_batches;
static uint64_t initial,confirmed,cache_bytes;
static unsigned char disk[16384],cache[16384];
static void vfs_backend_lock(vfs_backend_node_t *b) {
 assert(!depth);++depth;++locks;
 if(shrink_between_batches && locks==2)b->size=initial+confirmed;
}
static void vfs_backend_unlock(vfs_backend_node_t *b) { (void)b;assert(depth==1);--depth; }
static bool x86_user_range_valid(const void *p,size_t n) { return p || !n; }
static kstatus_t transfer(uint64_t pos,void *buf,size_t n,uint64_t *got,bool writing) {
 assert(depth==1 && pos==initial+confirmed && n>0);
 if(wide)assert(n<=1024U*1024U);
 unsigned index=calls++;
 if((int)index==failure_at) {
  if(mode==0) { *got=n;return K_EIO; }
  if(mode==1) { *got=(uint64_t)n+1;return K_OK; }
  *got=mode==2?0:n/2;
 } else *got=n;
 if(!wide && *got) {
  assert(pos+*got<=sizeof(disk));
  if(writing)memcpy(disk+pos,buf,*got);
  else memcpy(buf,disk+pos,*got);
 }
 confirmed+=*got;
 return K_OK;
}
static kstatus_t read_backend(void *c,uint64_t p,void *b,size_t n,uint64_t *g) {
 (void)c;return transfer(p,b,n,g,false);
}
static kstatus_t write_backend(void *c,uint64_t p,const void *b,size_t n,uint64_t *g) {
 (void)c;return transfer(p,(void*)b,n,g,true);
}
static kstatus_t user_copy(void *d,const void *s,size_t n) {
 assert(!depth);
 if((int)copy_calls++==copy_failure_at)return K_EACCES;
 memcpy(d,s,n);return K_OK;
}
#define copy_to_user user_copy
#define copy_from_user user_copy
static void vfs_page_cache_copy_in(vnode_t *v,uint64_t pos,const void *buf,size_t n) {
 (void)v;assert(!depth && pos==initial+cache_bytes && pos+n<=sizeof(cache));
 memcpy(cache+pos,buf,n);cache_bytes+=n;++cache_calls;
}
''' + code + r'''
static void reset(void) {
 assert(!transaction_depth);
 calls=depth=copy_calls=cache_calls=locks=0;confirmed=cache_bytes=0;
 shrink_between_batches=false;
 failure_at=copy_failure_at=-1;mode=0;
 memset(cache,0x73,sizeof(cache));
 for(unsigned i=0;i<sizeof(disk);++i)disk[i]=(unsigned char)(i*17+3);
}
int main(void) {
 vfs_backend_node_t backend={.size=sizeof(disk),.read=read_backend,.write=write_backend};
 vnode_t node={&backend,0};file_t file={{KOBJECT_TYPE_FILE},&node,3,0,0};
 unsigned char arena[10000],input[10000];
 for(unsigned i=0;i<sizeof(input);++i)input[i]=(unsigned char)(i*7+19);
 unsigned cases=0;
 // All four production functions, three initial offsets and four backend outcomes.
 for(unsigned api=0;api<4;++api)for(unsigned start=0;start<3;++start)
 for(int fail=0;fail<3;++fail)for(int outcome=0;outcome<4;++outcome) {
  reset();initial=start==0?0:start==1?13:4090;file.position=initial;
  failure_at=fail;mode=outcome;uint64_t got=UINT64_MAX;
  memset(arena,0x69,sizeof(arena));
  bool kernel=api&1,writing=api>=2;
  size_t unit=kernel?9000:PAGE_SIZE;
  unsigned available=kernel?1:3;
  size_t before=fail< (int)available?(size_t)fail*unit:9000;
  size_t chunk=before<9000?9000-before:0;
  if(chunk>unit)chunk=unit;
  uint64_t expected=before;
  kstatus_t expected_status=K_OK;
  if(fail<(int)available) {
   if(outcome<2)expected_status=K_EIO;
   else if(outcome==3)expected+=chunk/2;
  }
  kstatus_t status=writing?
   (kernel?vfs_write_kernel(&file,input+17,9000,&got):vfs_write(&file,input+17,9000,&got)):
   (kernel?vfs_read_kernel(&file,arena+17,9000,&got):vfs_read(&file,arena+17,9000,&got));
  assert(status==expected_status && got==expected && file.position==initial+expected && !depth);
  if(writing) {
   assert(cache_bytes==expected);
   for(unsigned i=0;i<sizeof(cache);++i)
    assert(cache[i]==(i>=initial&&i<initial+expected?input[17+i-initial]:0x73));
  } else {
   for(unsigned i=0;i<sizeof(arena);++i)
    assert(arena[i]==(i>=17&&i<17+expected?disk[initial+i-17]:0x69));
  }
  ++cases;
 }
 for(unsigned writing=0;writing<2;++writing)for(int fail=0;fail<3;++fail) {
  reset();initial=13;file.position=initial;copy_failure_at=fail;
  uint64_t got=123;memset(arena,0x69,sizeof(arena));
  kstatus_t status=writing?vfs_write(&file,input+17,9000,&got):vfs_read(&file,arena+17,9000,&got);
  assert(status==K_EACCES && got==(uint64_t)fail*PAGE_SIZE && file.position==initial+got && !depth);
  assert(calls==(unsigned)fail+!writing);
  if(writing) {
   assert(cache_bytes==got);
   for(unsigned i=0;i<sizeof(cache);++i) {
    bool changed=i>=initial && i<initial+got;
    assert(cache[i]==(changed?input[17+i-initial]:0x73));
    assert(disk[i]==(changed?input[17+i-initial]:(unsigned char)(i*17+3)));
   }
  }
  else for(unsigned i=0;i<sizeof(arena);++i)
   assert(arena[i]==(i>=17&&i<17+got?disk[initial+i-17]:0x69));
  ++cases;
 }
 // Bounded kernel reads release the backend guard between contiguous batches.
 const size_t batch=1024U*1024U;
 size_t length=2ULL*batch+29;
 void *mapping=mmap(0,length,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
 assert(mapping!=MAP_FAILED);wide=true;backend.size=UINT64_MAX;
 for(int fail=0;fail<3;++fail)for(int outcome=0;outcome<4;++outcome) {
  reset();initial=13;file.position=initial;failure_at=fail;mode=outcome;
  uint64_t got=123,expected=(uint64_t)fail*batch;
  size_t chunk=fail<2?batch:29;
  if(outcome==3)expected+=chunk/2;
  kstatus_t status=vfs_read_kernel(&file,mapping,length,&got);
  assert(status==(outcome<2?K_EIO:K_OK) && got==expected && file.position==initial+expected && !depth);
  ++cases;
 }
 assert(munmap(mapping,length)==0);wide=false;
 // A >4GiB request still completes without narrowing offsets or copying
 // through a temporary page. PROT_NONE makes accidental test access fail.
 length=(size_t)UINT32_MAX+batch+29;
 mapping=mmap(0,length,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
 assert(mapping!=MAP_FAILED);wide=true;
 reset();initial=13;file.position=initial;
 uint64_t large_got=0;
 assert(vfs_read_kernel(&file,mapping,length,&large_got)==K_OK);
 assert(large_got==length && file.position==initial+length && !depth);
 assert(calls==(length+batch-1)/batch);
 assert(locks==calls);
 ++cases;
 reset();initial=13;file.position=initial;shrink_between_batches=true;
 assert(vfs_read_kernel(&file,mapping,length,&large_got)==K_OK);
 assert(large_got==batch && file.position==initial+batch && !depth);
 assert(calls==1 && locks==2);
 ++cases;
 assert(munmap(mapping,length)==0);wide=false;
 backend.size=9000;
 for(unsigned kernel=0;kernel<2;++kernel)for(unsigned offset=0;offset<3;++offset) {
  reset();initial=offset==0?9000:offset==1?9013:UINT64_MAX;file.position=initial;
  uint64_t got=123;memset(arena,0x69,sizeof(arena));
  assert((kernel?vfs_read_kernel(&file,arena,100,&got):vfs_read(&file,arena,100,&got))==K_OK);
  assert(!got && !calls && file.position==initial);
  for(unsigned i=0;i<sizeof(arena);++i)assert(arena[i]==0x69);
  ++cases;
 }
 for(unsigned api=0;api<4;++api) {
  reset();initial=13;file.position=initial;uint64_t got=123;
  kstatus_t status=api==0?vfs_read(&file,arena,0,&got):api==1?vfs_read_kernel(&file,arena,0,&got):
   api==2?vfs_write(&file,input,0,&got):vfs_write_kernel(&file,0,0,&got);
  assert(status==K_OK && !got && !calls && file.position==initial);
  ++cases;
 }
 printf("PASS: direct I/O progress, failures, copies, cache coherence, wide batching (%u cases)\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-io-progress-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
    # Reintroduce the old early error exits: this must fail the same fixture.
    original_errors = re.sub(r'(result = [^;]+;\s*)break;',
                             r'\1return result;', code)
    assert original_errors != code
    (path / 'negative.c').write_text(program.replace(code, original_errors))
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(path / 'negative.c'), '-o', str(path / 'negative')], check=True)
    result = subprocess.run([str(path / 'negative')], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode != 0, 'Old error exits unexpectedly passed'
    print('PASS: negative control rejects the old lost-progress error exits')
    cap = 'if (chunk > 1024U * 1024U) chunk = 1024U * 1024U;'
    assert code.count(cap) == 2
    unbounded = program.replace(cap, 'if (chunk > UINT32_MAX) chunk = UINT32_MAX;', 1)
    (path/'unbounded.c').write_text(unbounded)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(path/'unbounded.c'), '-o', str(path/'unbounded')], check=True)
    result = subprocess.run([str(path/'unbounded')], capture_output=True, text=True,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode != 0 and 'n<=1024U*1024U' in result.stderr, result.stderr
    print('PASS: negative control rejects a multi-gigabyte backend guard')
    helper = code[code.index('static kstatus_t vfs_write_chunk('):code.index('kstatus_t vfs_write(')]
    assert 'status != K_OK || *bytes > length' in helper
    controls = {
        'unchecked-write-count': helper.replace('status != K_OK || *bytes > length',
                                               'status != K_OK'),
        'cache-under-backend-lock': helper.replace('    vfs_backend_unlock(backend);\n', ''),
    }
    # On a normal successful write this control reaches the cache with the
    # backend lock held, independently of any error-path assertions.
    for name, changed in controls.items():
        assert changed != helper
        (path/(name+'.c')).write_text(program.replace(helper, changed))
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(path/(name+'.c')), '-o', str(path/name)], check=True)
        result = subprocess.run([str(path/name)], capture_output=True, text=True,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
        print('PASS: negative control rejects '+name)
