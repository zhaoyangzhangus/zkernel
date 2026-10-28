"""Run the actual desktop launcher through allocation and lifecycle failures."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/launcher.c').read_text()
body = source[source.index('kstatus_t desktop_launch_program('):source.index('\n#endif', source.index('kstatus_t desktop_launch_program('))]
io = (root/'kernel/fs/vfs/file_io.c').read_text()
file_loader = io[io.index('kstatus_t vfs_load_file_kernel('):io.index('/* 整次写入')]
init = (root/'kernel/init/user_services.c').read_text()
init_loader = init[init.index('static kstatus_t init_load_path('):init.index('static bool init_wait_for_exit(')]
init_limit = next(line for line in init.splitlines() if line.startswith('#define INIT_IMAGE_SIZE'))
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
enum {K_OK=0,K_EINVAL=-22,K_EIO=-5,K_ENOMEM=-12,K_EBUSY=-16,
 DESKTOP_APP_CHROME=1,VFS_OPEN_READ=1};
#define DESKTOP_PROGRAM_MAX_BYTES (16ULL*1024ULL*1024ULL)
#define DESKTOP_CHROME_LAUNCH_STARTING UINT64_MAX
typedef int kstatus_t;
typedef uintptr_t vaddr_t;
typedef struct {uint64_t size;} vnode_t;
typedef struct {vnode_t *vnode;} file_t;
typedef struct {uint64_t pid;} process_t;
typedef struct {unsigned unused;} thread_t;
typedef struct {vaddr_t entry;} user_elf_image_info_t;
static struct {uint64_t desktop_chrome_launch_pid;} g_window_server;
enum {SUCCESS,OPEN_FAIL,NO_VNODE,EMPTY,OVERSIZE,ALLOC_FAIL,READ_FAIL,
 SHORT_READ,CREATE_FAIL,PUBLISH_FAIL,LOAD_FAIL,THREAD_FAIL,START_FAIL,
 CLAIM_BUSY,NO_PATH};
static unsigned failure,live_images,file_refs,process_refs,thread_refs;
static unsigned closes,aborts,terminates,releases,loads,starts;
static vnode_t node;
static uint64_t image_bytes=64,image_limit=DESKTOP_PROGRAM_MAX_BYTES;
static bool vnode_locked;
static unsigned stage_count,stages[2],names;
enum {OS_DEBUG_PHASE_USER=1,OS_DEBUG_STEP_LOAD=2};
static void kernel_debug_stage(unsigned phase,unsigned step,unsigned stage){
 assert(phase==OS_DEBUG_PHASE_USER&&step==OS_DEBUG_STEP_LOAD&&stage_count<2);
 stages[stage_count++]=stage;
}
static void vfs_vnode_lock(vnode_t *v){assert(v==&node&&!vnode_locked);vnode_locked=true;}
static void vfs_vnode_unlock(vnode_t *v){assert(v==&node&&vnode_locked);vnode_locked=false;}
static file_t file;
static process_t process={42};
static thread_t thread;
static const char *window_shell_program_path(uint32_t app){
 (void)app;return failure==NO_PATH?NULL:"/sbin/test";
}
static bool desktop_chrome_request_launch(void){return failure!=CLAIM_BUSY;}
static bool desktop_chrome_launch_publish_pid(uint64_t *slot,uint64_t pid){
 assert(pid==42);if(failure==PUBLISH_FAIL)return false;*slot=pid;return true;
}
static bool desktop_chrome_launch_release(uint64_t *slot,uint64_t token){
 (void)slot;assert(token==42 || token==UINT64_MAX);++releases;return true;
}
static kstatus_t vfs_open_kernel(const char *path,unsigned flags,unsigned mode,file_t **out){
 assert(path && flags==VFS_OPEN_READ && !mode);
 if(failure==OPEN_FAIL)return K_EIO;
 node.size=failure==EMPTY?0:failure==OVERSIZE?image_limit+1:image_bytes;
 file.vnode=failure==NO_VNODE?NULL:&node;*out=&file;++file_refs;return K_OK;
}
static void vfs_close(file_t *f){assert(f==&file && file_refs==1);--file_refs;++closes;}
static void *kmalloc(size_t size,unsigned flags){
 assert(size==image_bytes && !flags && !live_images && !vnode_locked);
 if(failure==ALLOC_FAIL)return NULL;
 void *p=malloc(size);assert(p);++live_images;return p;
}
static void kfree(void *p){if(p){assert(live_images==1);--live_images;free(p);}}
static kstatus_t vfs_read_kernel(file_t *f,void *buf,size_t size,uint64_t *bytes){
 assert(f==&file && file_refs==1 && live_images==1 && size==image_bytes && !vnode_locked);
 if(failure==READ_FAIL)return K_EIO;
 memset(buf,0x5a,size);*bytes=failure==SHORT_READ?size-1:size;return K_OK;
}
static kstatus_t process_create(void *parent,process_t **out){
 assert(!parent && !file_refs && live_images==1);
 if(failure==CREATE_FAIL)return K_ENOMEM;
 *out=&process;++process_refs;return K_OK;
}
static kstatus_t process_abort(process_t *p){assert(p==&process && process_refs==1);++aborts;return K_OK;}
static void object_put(void *obj){
 if(obj==&process){assert(process_refs==1);--process_refs;}
 else {assert(obj==&thread && thread_refs==1);--thread_refs;}
}
static void process_set_name(process_t *p,const char *path){assert(p==&process && path);++names;}
static kstatus_t process_load_elf_image(process_t *p,void *image,size_t size,
 user_elf_image_info_t *info,vaddr_t *stack){
 assert(p==&process && live_images==1 && !file_refs && size==image_bytes);
 for(unsigned i=0;i<size;++i)assert(((unsigned char*)image)[i]==0x5a);
 ++loads;info->entry=0x400000;*stack=0x800000;return failure==LOAD_FAIL?K_EIO:K_OK;
}
static kstatus_t thread_create_user_suspended(process_t *p,vaddr_t entry,
 vaddr_t stack,unsigned arg,unsigned flags,thread_t **out){
 assert(p==&process && entry==0x400000 && stack==0x800000 && !arg && !flags && !live_images);
 if(failure==THREAD_FAIL)return K_ENOMEM;
 *out=&thread;++thread_refs;return K_OK;
}
static kstatus_t thread_start(thread_t *t){assert(t==&thread);++starts;return failure==START_FAIL?K_EIO:K_OK;}
static kstatus_t thread_terminate(thread_t *t,kstatus_t status){assert(t==&thread && status==K_EIO);++terminates;return K_OK;}
''' + init_limit + '\n' + file_loader + body + init_loader + r'''
int main(void){
 unsigned cases=0;
 for(unsigned app=1;app<=2;++app)for(unsigned fail=0;fail<=NO_PATH;++fail){
  failure=fail;
  if(app!=DESKTOP_APP_CHROME && (fail==PUBLISH_FAIL || fail==CLAIM_BUSY))continue;
  closes=aborts=terminates=releases=loads=starts=0;
  g_window_server.desktop_chrome_launch_pid=0;
  int status=desktop_launch_program(app);
  int expected=K_OK;
  if(fail==NO_PATH || fail==NO_VNODE || fail==EMPTY || fail==OVERSIZE)expected=K_EINVAL;
  if(fail==OPEN_FAIL || fail==READ_FAIL || fail==SHORT_READ || fail==LOAD_FAIL || fail==START_FAIL)expected=K_EIO;
  if(fail==ALLOC_FAIL || fail==CREATE_FAIL || fail==THREAD_FAIL)expected=K_ENOMEM;
  if(fail==PUBLISH_FAIL)expected=K_EBUSY;
  assert(status==expected);
  assert(live_images==0 && file_refs==0 && process_refs==0 && thread_refs==0);
  bool opened=fail!=NO_PATH && fail!=CLAIM_BUSY && fail!=OPEN_FAIL;
  assert(closes==(unsigned)opened);
  bool created=fail==SUCCESS || fail==PUBLISH_FAIL || fail==LOAD_FAIL || fail==THREAD_FAIL || fail==START_FAIL;
  assert(aborts==(unsigned)(created && fail!=SUCCESS && fail!=START_FAIL));
  assert(terminates==(unsigned)(fail==START_FAIL));
  assert(loads==(unsigned)(created && fail!=PUBLISH_FAIL));
  assert(starts==(unsigned)(fail==SUCCESS || fail==START_FAIL));
  assert(releases==(unsigned)(app==DESKTOP_APP_CHROME && expected!=K_OK && fail!=NO_PATH));
  ++cases;
 }
 printf("Desktop launch cleanup: %u native/Chrome success and failure paths, image/file/process/thread ownership passed\n",cases);
 /* 启动服务保持自己的大小上限，不借用桌面应用较宽的上限。 */
 const unsigned init_failures[]={SUCCESS,OPEN_FAIL,NO_VNODE,EMPTY,OVERSIZE,
  ALLOC_FAIL,READ_FAIL,SHORT_READ,LOAD_FAIL};
 image_limit=INIT_IMAGE_SIZE;
 for(unsigned boundary=0;boundary<2;++boundary)
  for(unsigned i=0;i<sizeof(init_failures)/sizeof(*init_failures);++i){
   failure=init_failures[i];image_bytes=boundary?INIT_IMAGE_SIZE:64;
   stage_count=closes=loads=names=0;user_elf_image_info_t info={0};vaddr_t stack=0;
   int expected=failure==SUCCESS?K_OK:
    failure==NO_VNODE||failure==EMPTY||failure==OVERSIZE?K_EINVAL:
    failure==ALLOC_FAIL?K_ENOMEM:K_EIO;
   assert(init_load_path("/sbin/test",&process,&info,&stack)==expected);
   assert(stage_count==2&&stages[0]==0x1a&&stages[1]==0x1d);
   assert(!file_refs&&!live_images&&!vnode_locked);
   assert(closes==(unsigned)(failure!=OPEN_FAIL));
   assert(loads==(unsigned)(failure==SUCCESS||failure==LOAD_FAIL));
   assert(names==(unsigned)(failure==SUCCESS));
  }
 puts("Init ELF loading: 18 failure/boundary cases, independent image limit, ownership and read-stage diagnostics PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='desktop-launch-cleanup-') as directory:
    path = Path(directory)
    def run(name, code, negative=False):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-x', 'c', '-', '-o', str(path/name)],
                       input=code, text=True, check=True)
        result = subprocess.run([str(path/name)], capture_output=True, text=True, timeout=30,
                                preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        if negative:
            assert result.returncode != 0 and 'live_images==0' in result.stderr, result.stderr
            print('Negative control rejected: launch publication failure leaks the image')
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
    run('production', program)
    fixed = '                status = K_EBUSY;\n                goto done;'
    assert program.count(fixed) == 1
    run('missing-image-release', program.replace(fixed, '                return K_EBUSY;'), True)
