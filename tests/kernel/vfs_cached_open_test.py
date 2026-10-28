"""Production cached-open branch preserves the shared FAT inode on writes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
start = source.index('    vnode_t *existing = vfs_find_fat_node_locked(path);')
branch = source[start:source.index('    uint32_t slot =', start)]
start = source.index('    vfs_initialize();', source.index('static kstatus_t vfs_create_fat32_vnode('))
fast = source[start:source.index('    backend = (vfs_backend_node_t *)kzalloc(', start)]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef struct {unsigned id;} fat_file_t;
typedef struct {fat_file_t *fat_file;} vfs_backend_node_t;
typedef struct {void *fs_private;unsigned refs;} vnode_t;
static unsigned locks,gets,drops,closes;
static bool hit=true;
static fat_file_t inode={42};
static vfs_backend_node_t backend={&inode};
static vnode_t canonical={&backend,2},temporary={0,1};
#define K_OK 0
static vnode_t *vfs_find_fat_node_locked(const char *p){assert(locks&&p);return hit?&canonical:0;}
static void vfs_initialize(void){}
static void vfs_lock(void){assert(!locks);locks=1;}
static void object_get(vnode_t *p){assert(locks&&p==&canonical);++p->refs;++gets;}
static void object_put(vnode_t *p){assert(!locks&&p==&temporary&&p->refs==1);--p->refs;++drops;}
static void vfs_unlock(void){assert(locks==1);locks=0;}
bool fat32_close(fat_file_t *p){assert(p==&inode);++closes;return true;}
void vfs_backend_lock(vfs_backend_node_t *p){assert(p==&backend);}
void vfs_backend_unlock(vfs_backend_node_t *p){assert(p==&backend);}
static int cached_open(bool keep_fat_file,vnode_t **out){
 const char *path="/cached";vnode_t *vnode=&temporary;
 (void)keep_fat_file;
''' + branch + r'''
 return -1;
}
static int fast_open(vnode_t **out){const char *path="/cached";
''' + fast + r'''
 return -1;
}
int main(void){
 for(unsigned i=0;i<2000;++i){
  vnode_t *out=0;locks=1;temporary.refs=1;
  assert(cached_open(i%2!=0,&out)==K_OK);
  assert(out==&canonical&&backend.fat_file==&inode);
  assert(!locks&&!closes&&gets==i+1&&drops==i+1);
  --canonical.refs;
 }
 assert(canonical.refs==2);
 for(unsigned i=0;i<2000;++i){
  vnode_t *out=0;
  assert(fast_open(&out)==K_OK&&out==&canonical&&!locks);
  assert(gets==2001+i&&drops==2000&&!closes&&backend.fat_file==&inode);
  --canonical.refs;
 }
 hit=false;vnode_t *out=0;
 assert(fast_open(&out)==-1&&!out&&!locks&&gets==4000&&canonical.refs==2);
 puts("Cached FAT fast path: 2000 hits return before allocation/native open; miss releases lock for normal creation");
 puts("Cached FAT open: 2000 mixed read/write opens retain inode/seek caches, balance temporary refs, and never close the shared handle");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-cached-open-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
