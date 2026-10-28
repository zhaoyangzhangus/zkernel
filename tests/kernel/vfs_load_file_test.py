"""执行真实有界文件加载器与 PNG 调用者，覆盖失败所有权与输出发布时机。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
io = (root / 'kernel/fs/vfs/file_io.c').read_text()
loader = io[io.index('kstatus_t vfs_load_file_kernel('):io.index('/* 整次写入')]
assets = (root / 'kernel/graphics/assets.c').read_text()
png = assets[assets.index('static bool desktop_load_png_asset('):assets.index('/* UI 冷加载')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int kstatus_t;
enum {K_OK=0,K_EINVAL=-22,K_ENOMEM=-12,K_EIO=-5,K_EACCES=-13,VFS_OPEN_READ=1};
#define DESKTOP_ASSET_MAX_BYTES 1024U
typedef struct {uint64_t size;} vnode_t;
typedef struct {vnode_t *vnode;} file_t;
typedef struct {void *storage;} desktop_asset_image_t;
enum {GOOD,OPEN_ERROR,OPEN_ERROR_WITH_FILE,NO_FILE,NO_NODE,NO_MEMORY,
      READ_ERROR,SHORT_READ,EXCESS_READ,DECODE_ERROR};
static unsigned mode,opens,closes,allocations,frees,reads,decodes,locks;
static bool locked;
static vnode_t node;
static file_t file;
static void *live;
static void **observed_buffer;
static size_t *observed_length;
static void unpublished(void) {
 if(observed_buffer)assert(!*observed_buffer);
 if(observed_length)assert(!*observed_length);
}
static void vfs_vnode_lock(vnode_t *v) {
 assert(v==&node&&!locked);locked=true;++locks;
}
static void vfs_vnode_unlock(vnode_t *v) {assert(v==&node&&locked);locked=false;}
static int vfs_open_kernel(const char *p,unsigned flags,unsigned create,file_t **out) {
 assert(p&&!strcmp(p,"/fixture")&&flags==VFS_OPEN_READ&&!create&&!locked);
 unpublished();++opens;
 if(mode==OPEN_ERROR)return K_EACCES;
 if(mode==NO_FILE)return K_OK;
 file.vnode=mode==NO_NODE?NULL:&node;*out=&file;
 return mode==OPEN_ERROR_WITH_FILE?K_EACCES:K_OK;
}
static void vfs_close(file_t *f) {
 assert(f==&file&&!locked);unpublished();++closes;
}
static void *kmalloc(size_t n,unsigned flags) {
 assert(n==node.size&&!flags&&!locked&&!live);unpublished();++allocations;
 if(mode==NO_MEMORY)return NULL;
 live=malloc(n);assert(live);return live;
}
static void kfree(void *p) {
 assert(p&&p==live&&!locked);++frees;free(p);live=NULL;
}
static int vfs_read_kernel(file_t *f,void *p,size_t n,uint64_t *bytes) {
 assert(f==&file&&p==live&&n==node.size&&!locked&&locks==1);unpublished();++reads;
 memset(p,0x5a,n);
 if(mode==READ_ERROR){*bytes=n/2;return K_EACCES;}
 *bytes=mode==SHORT_READ?n-1:mode==EXCESS_READ?n+1:n;return K_OK;
}
static bool desktop_png_decode(const void *p,size_t n,desktop_asset_image_t *a) {
 assert(p==live&&n==node.size&&closes==1&&!locked);++decodes;
 if(mode==DECODE_ERROR)return false;
 for(size_t i=0;i<n;++i)assert(((const uint8_t*)p)[i]==0x5a);
 a->storage=(void*)1;return true;
}
static void reset(unsigned failure,uint64_t size) {
 assert(!live&&!locked);mode=failure;node.size=size;
 opens=closes=allocations=frees=reads=decodes=locks=0;
 observed_buffer=NULL;observed_length=NULL;
}
'''
main = r'''
int main(void) {
 unsigned cases=0;
 uint64_t sizes[]={0,1,11,12,64,1024,1025,UINT64_MAX};
 for(unsigned f=GOOD;f<DECODE_ERROR;++f)
  for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);++i) {
   reset(f,sizes[i]);void *buffer=(void*)1;size_t size=99;
   observed_buffer=&buffer;observed_length=&size;
   int status=vfs_load_file_kernel("/fixture",12,1024,&buffer,&size);
   bool bounded=node.size>=12&&node.size<=1024;
   int expected=K_OK;
   if(f==OPEN_ERROR||f==OPEN_ERROR_WITH_FILE)expected=K_EACCES;
   else if(f==NO_FILE||f==NO_NODE||!bounded)expected=K_EINVAL;
   else if(f==NO_MEMORY)expected=K_ENOMEM;
   else if(f==READ_ERROR)expected=K_EACCES;
   else if(f==SHORT_READ||f==EXCESS_READ)expected=K_EIO;
   assert(status==expected&&opens==1&&!locked);
   assert(closes==(unsigned)(f!=OPEN_ERROR&&f!=NO_FILE));
   assert(locks==(unsigned)(f!=OPEN_ERROR&&f!=OPEN_ERROR_WITH_FILE&&f!=NO_FILE&&f!=NO_NODE));
   if(status==K_OK) {
    assert(buffer==live&&size==node.size&&reads==1);
    for(size_t j=0;j<size;++j)assert(((uint8_t*)buffer)[j]==0x5a);
    observed_buffer=NULL;observed_length=NULL;kfree(buffer);
   } else assert(!buffer&&!size&&!live);
   assert(frees==allocations-(unsigned)(f==NO_MEMORY&&bounded));
   ++cases;
  }
 for(unsigned bad=0;bad<5;++bad) {
  reset(GOOD,64);void *buffer=(void*)1;size_t size=99;
  assert(vfs_load_file_kernel(bad==0?NULL:"/fixture",bad==1?0:12,
         bad==2?11:1024,bad==3?NULL:&buffer,bad==4?NULL:&size)==K_EINVAL);
  assert(!opens&&!allocations&&!live);
  if(bad!=3)assert(!buffer);
  if(bad!=4)assert(!size);
 }
 for(unsigned f=GOOD;f<=DECODE_ERROR;++f) {
  reset(f,64);desktop_asset_image_t asset={0};
  assert(desktop_load_png_asset("/fixture",&asset)==(f==GOOD));
  assert(!live&&!locked);
  assert(decodes==(unsigned)(f==GOOD||f==DECODE_ERROR));
  assert(asset.storage==(f==GOOD?(void*)1:NULL));
 }
 reset(GOOD,64);desktop_asset_image_t loaded={(void*)1};
 assert(!desktop_load_png_asset("/fixture",&loaded));
 assert(!desktop_load_png_asset(NULL,&loaded));
 assert(!desktop_load_png_asset("/fixture",NULL));
 assert(!opens);
 printf("Bounded file loading: %u size/failure cases, argument guards, lock boundaries, unpublished outputs and actual PNG rollback PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-load-file-') as directory:
    work = Path(directory)
    def run(name, source, negative=False):
        binary = work/name
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-x', 'c', '-', '-o', str(binary)],
                       input=source, text=True, check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        if negative:
            assert result.returncode and 'Assertion' in result.stderr, result.stderr
            print('Negative control rejected: accepting incomplete file contents')
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
    code = prefix+loader+png+main
    run('production', code)
    guard = 'if (status == K_OK && bytes != size) status = K_EIO;'
    assert code.count(guard) == 1
    run('accept-short-read', code.replace(guard, '(void)bytes;'), True)
