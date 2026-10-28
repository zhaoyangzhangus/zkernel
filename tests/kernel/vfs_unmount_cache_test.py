"""执行真实卸载路径，防止自测卷的同名 inode 污染随后挂载的系统卷。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
a = source.index('kstatus_t vfs_unmount_fat32(void) {')
body = source[a:source.index('\nstatic bool vfs_directory_exists_locked', a)]
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#define VFS_PATH_LIMIT 256U
typedef int kstatus_t;
enum {K_OK,K_ENOENT,K_EBUSY};
typedef struct vnode vnode_t;
typedef struct {char path[256];bool fat32_node;vnode_t *namespace_next;unsigned size;} vfs_backend_node_t;
struct vnode {struct {struct {atomic_uint value;} refs;} object;void *fs_private;};
typedef struct {char prefix[256];void *filesystem;bool mounted;} mount_t;
static mount_t g_fat32_mounts[2];
static vnode_t *g_vfs_nodes[5];static uint32_t g_vfs_node_capacity=5;
static vnode_t nodes[4];static vfs_backend_node_t backends[4];
static unsigned locked,drops;
static void vfs_initialize(void){}
static void vfs_lock(void){assert(!locked);locked=1;}
static void vfs_unlock(void){assert(locked);locked=0;}
static mount_t *vfs_fat32_mount_for_path(const char *p,char *r){
 assert(locked);(void)r;return !strncmp(p,"/other/",7)?&g_fat32_mounts[1]:&g_fat32_mounts[0];
}
static void vfs_set_node_slot_locked(uint32_t i,vnode_t *n){
 assert(locked&&!n&&g_vfs_nodes[i]);
 ((vfs_backend_node_t*)g_vfs_nodes[i]->fs_private)->namespace_next=0;g_vfs_nodes[i]=n;
}
static void object_put(vnode_t *n){
 assert(!locked&&!g_fat32_mounts[0].mounted&&atomic_load(&n->object.refs.value)==1);
 assert(!((vfs_backend_node_t*)n->fs_private)->namespace_next);
 atomic_store(&n->object.refs.value,0);++drops;
}
''' + body + r'''
static void setup(void){
 assert(!locked);memset(backends,0,sizeof(backends));memset(g_vfs_nodes,0,sizeof(g_vfs_nodes));
 g_fat32_mounts[0]=(mount_t){"/",(void*)1,true};g_fat32_mounts[1]=(mount_t){"/other",(void*)2,true};drops=0;
 const char *paths[]={"/sample.txt","/cached.txt","/ram.txt","/other/file.txt"};
 for(unsigned i=0;i<4;++i){strcpy(backends[i].path,paths[i]);backends[i].fat32_node=i!=2;
 backends[i].size=5;nodes[i].fs_private=&backends[i];atomic_init(&nodes[i].object.refs.value,1);g_vfs_nodes[i]=&nodes[i];}
}
int main(void){
 setup();assert(vfs_unmount_fat32()==K_OK&&!locked);
 assert(!g_vfs_nodes[0]&&!g_vfs_nodes[1]&&drops==2);
 assert(g_vfs_nodes[2]==&nodes[2]&&g_vfs_nodes[3]==&nodes[3]&&g_fat32_mounts[1].mounted);
 /* 新卷上的 sample.txt 为三字节，不得命中自测卷遗留的五字节 inode。 */
 g_fat32_mounts[0]=(mount_t){"/",(void*)3,true};assert(!g_vfs_nodes[0]);
 for(unsigned busy=0;busy<2;++busy){
 setup();atomic_store(&nodes[busy].object.refs.value,2);
 assert(vfs_unmount_fat32()==K_EBUSY&&!locked&&!drops&&g_fat32_mounts[0].mounted);
 for(unsigned i=0;i<4;++i)assert(g_vfs_nodes[i]==&nodes[i]);
 atomic_store(&nodes[busy].object.refs.value,1);assert(vfs_unmount_fat32()==K_OK&&drops==2);
 assert(vfs_unmount_fat32()==K_ENOENT&&!locked&&drops==2);
 }
 setup();g_vfs_node_capacity=0;assert(vfs_unmount_fat32()==K_OK&&!drops);
 puts("Unmount cache: stale sample inode retired; busy rollback, RAM/submount preservation, empty/unmounted cases passed");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-unmount-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(preamble)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

    # 旧实现只隐藏挂载点，会保留自测 inode；相同断言必须将其检出。
    previous = """kstatus_t vfs_unmount_fat32(void) {
        vfs_initialize();vfs_lock();
        if (!g_fat32_mounts[0].mounted) {vfs_unlock();return K_ENOENT;}
        g_fat32_mounts[0].prefix[0]=0;g_fat32_mounts[0].filesystem=0;
        g_fat32_mounts[0].mounted=false;vfs_unlock();return K_OK;
    }"""
    (path / 'previous.c').write_text(preamble.replace(body, previous))
    subprocess.run(['cc','-std=c11',str(path/'previous.c'),'-o',str(path/'previous')],check=True)
    result = subprocess.run([str(path/'previous')],capture_output=True)
    assert result.returncode != 0, '旧卸载实现未被回归测试检出'
    print('旧实现保留 sample 缓存，被同一回归测试拒绝')
