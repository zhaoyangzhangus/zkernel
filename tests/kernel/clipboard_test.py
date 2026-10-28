"""执行真实剪贴板系统调用，验证失败原子性、读者快照和并发替换。"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
body='\n'.join(line for line in (root/'kernel/syscall/clipboard.c').read_text().splitlines() if not line.startswith('#include'))
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <uapi/clipboard.h>
#include <uapi/syscall.h>
typedef int kstatus_t;
enum {K_OK=0,K_EINVAL=-22,K_ENOMEM=-12,K_EFAULT=-14,K_ENOSPC=-28};
#define __user
static _Thread_local bool allocation_fail,read_fail,write_fail;
static _Thread_local unsigned read_calls,fail_read_at;
static _Thread_local unsigned depth;
static atomic_uint live;
typedef struct {atomic_flag state;} spinlock_t;
static void spinlock_lock(spinlock_t *lock){assert(depth==1);while(atomic_flag_test_and_set(&lock->state)){} ++depth;}
static void spinlock_unlock(spinlock_t *lock){assert(depth==2);--depth;atomic_flag_clear(&lock->state);}
static void sched_preempt_disable(void){assert(!depth);++depth;}
static void sched_preempt_enable(void){assert(depth==1);--depth;}
static void *kmalloc(size_t size,unsigned flags){assert(!depth&&!flags);if(allocation_fail)return 0;void *p=malloc(size);if(p)atomic_fetch_add(&live,1);return p;}
static void kfree(void *p){assert(!depth);if(p)atomic_fetch_sub(&live,1);free(p);}
static int copy_from_user(void *to,const void *from,size_t size){assert(!depth);if(read_fail||!from||++read_calls==fail_read_at)return K_EFAULT;memcpy(to,from,size);return 0;}
static int copy_to_user(void *to,const void *from,size_t size){assert(!depth);if(write_fail||!to)return K_EFAULT;memcpy(to,from,size);return 0;}
static bool versioned_header_valid(const os_versioned_header_t *h,size_t size){return h->size>=size&&h->version==OS_SYSCALL_ABI_VERSION;}
''' + body + r'''
static int set(const char *text,size_t size){os_clipboard_text_t r={.hdr={sizeof(r),OS_SYSCALL_ABI_VERSION},.data=(uintptr_t)text,.size=size};return syscall_clipboard_set((uintptr_t)&r,0,0,0,0,0);}
static int get(char *text,size_t capacity,size_t *size){os_clipboard_text_t r={.hdr={sizeof(r),OS_SYSCALL_ABI_VERSION},.data=(uintptr_t)text,.size=capacity};int rc=syscall_clipboard_get((uintptr_t)&r,0,0,0,0,0);*size=r.size;return rc;}
static void *worker(void *arg){
 char text[1024],out[1024];memset(text,(int)(uintptr_t)arg,sizeof(text));
 for(unsigned i=0;i<2000;++i){size_t n=1+i%sizeof(text),got=0;assert(!set(text,n));if(i%17U==0)assert(!set(0,0));assert(!get(out,sizeof(out),&got));assert(got<=sizeof(out));for(size_t j=1;j<got;++j)assert(out[j]==out[0]);}
 return 0;
}
int main(void){
 char out[32];size_t size=9;assert(!get(0,0,&size)&&size==0);
 assert(!set("你好 world",12));assert(!get(0,0,&size)&&size==12);
 memset(out,0x7f,sizeof(out));assert(get(out,2,&size)==K_ENOSPC&&size==12&&out[0]==0x7f);
 assert(!get(out,sizeof(out),&size)&&size==12&&!memcmp(out,"你好 world",12));
 allocation_fail=true;assert(set("bad",3)==K_ENOMEM);allocation_fail=false;
 read_fail=true;assert(set("bad",3)==K_EFAULT);read_fail=false;
 assert(set(0,1)==K_EINVAL);assert(set("x",OS_CLIPBOARD_MAX_BYTES+1ULL)==K_EINVAL);
 assert(!get(out,sizeof(out),&size)&&size==12&&!memcmp(out,"你好 world",12));
 /* 已持有的旧快照跨越替换仍然存活。 */
 clipboard_text_t *old=g_clipboard;atomic_fetch_add(&old->references,1);
 assert(!set("new",3));assert(!memcmp(old->text,"你好 world",12));clipboard_put(old);
 write_fail=true;assert(get(out,sizeof(out),&size)==K_EFAULT);write_fail=false;
 /* 用户正文复制失败也不能替换原快照，不只验证请求头读取失败。 */
 read_calls=0;fail_read_at=2;assert(set("partial",7)==K_EFAULT);fail_read_at=0;
 assert(!get(out,sizeof(out),&size)&&size==3&&!memcmp(out,"new",3));
 /* 清空不需要分配；正在读取的旧快照直到读者释放才销毁。 */
 for(unsigned kernel=0;kernel<2;++kernel) {
  old=g_clipboard;atomic_fetch_add(&old->references,1);
  allocation_fail=true;
  assert(!(kernel?clipboard_set_text(0,0):set(0,0)));
  assert(!g_clipboard&&atomic_load(&live)==1);
  memset(out,0x7f,sizeof(out));assert(!get(out,sizeof(out),&size)&&!size&&out[0]==0x7f);
  assert(old->size==3&&!memcmp(old->text,"new",3));
  clipboard_put(old);assert(!atomic_load(&live));
  assert(!(kernel?clipboard_set_text(0,0):set(0,0))&&!atomic_load(&live));
  assert((kernel?clipboard_set_text("bad",3):set("bad",3))==K_ENOMEM);
  allocation_fail=false;assert(!clipboard_set_text("new",3));
 }
 /* 最大长度快照在清空后仍可由旧读者读取，最后一字节不能越界或提前释放。 */
 char *large=malloc(OS_CLIPBOARD_MAX_BYTES);assert(large);
 memset(large,0x5a,OS_CLIPBOARD_MAX_BYTES);
 for(unsigned kernel=0;kernel<2;++kernel) {
  assert(!(kernel?clipboard_set_text(large,OS_CLIPBOARD_MAX_BYTES):set(large,OS_CLIPBOARD_MAX_BYTES)));
  assert(get(out,sizeof(out),&size)==K_ENOSPC&&size==OS_CLIPBOARD_MAX_BYTES);
  old=g_clipboard;atomic_fetch_add(&old->references,1);
  allocation_fail=true;assert(!set(0,0));allocation_fail=false;
  assert(!g_clipboard&&old->text[OS_CLIPBOARD_MAX_BYTES-1]==0x5a);
  clipboard_put(old);assert(!atomic_load(&live));
 }
 free(large);
 pthread_t threads[4];for(unsigned i=0;i<4;++i)assert(!pthread_create(threads+i,0,worker,(void *)(uintptr_t)('a'+i)));
 for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 assert(!set(0,0));assert(!get(out,sizeof(out),&size)&&!size);
 assert(!g_clipboard&&!atomic_load(&live));
 puts("Clipboard: query, UTF-8, short capacity, allocation/copy failure, retained snapshot, allocation-free clear, payload fault and 8000 concurrent replacements/clears PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'test.c';c.write_text(code);exe=Path(tmp)/'test'
 subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
