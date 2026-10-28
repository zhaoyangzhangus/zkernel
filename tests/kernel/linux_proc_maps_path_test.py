"""真实映射行和 VFS 路径快照：库路径、重命名、删除与换行转义。"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
linux=(root/'kernel/syscall/linux.c').read_text()
vfs=(root/'kernel/fs/vfs/core.c').read_text()
def function(source,signature):
    a=source.index(signature)
    return source[a:source.index('\n}',a)+2]
code=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <kernel/vfs.h>
#include <kernel/vm.h>
#define VFS_PATH_LIMIT 256U
typedef struct {char path[256];bool detached;} vfs_backend_node_t;
typedef struct {char data[2048];size_t length;} linux_proc_file_t;
typedef struct {uint64_t linux_brk_base;} process_t;
static unsigned locks;
static void vfs_initialize(void){}
static void vfs_lock(void){assert(!locks);++locks;}
static void vfs_unlock(void){assert(locks==1);--locks;}
static size_t vfs_string_length(const char *s){return strlen(s);}
static bool linux_proc_append_char(linux_proc_file_t *f,char c){
 if(f->length+1>=sizeof(f->data))return false;
 f->data[f->length++]=c;f->data[f->length]=0;return true;
}
static bool linux_proc_append_text(linux_proc_file_t *f,const char *s){
 while(*s)if(!linux_proc_append_char(f,*s++))return false;return true;
}
static bool linux_proc_append_u64(linux_proc_file_t *f,uint64_t n){
 char text[32];snprintf(text,sizeof(text),"%llu",(unsigned long long)n);
 return linux_proc_append_text(f,text);
}
''' + function(vfs,'kstatus_t vfs_vnode_path_kernel(') + '\n' + function(linux,'static bool linux_proc_append_hex64(') + '\n' + function(linux,'static bool linux_proc_append_maps_label(') + '\n' + function(linux,'static bool linux_proc_append_maps_line(') + r'''
int main(void){
 vfs_backend_node_t backend={.path="/usr/apps/chrome/lib/libGLX_nvidia.so.0"};
 vnode_t vnode={.object={.type=KOBJECT_TYPE_VNODE},.fs_private=&backend,.inode_id=42};
 vm_object_t object={.type=VM_OBJECT_FILE,.u.file={.mapping=&vnode,.file_offset=4096}};
 vm_area_t area={.start=0x40000000,.end=0x40002000,.object=&object,.object_offset=8192,
  .prot=VM_PROT_READ|VM_PROT_EXEC};
 linux_proc_file_t line={0};
 assert(linux_proc_append_maps_line(&line,&area,0));
 assert(strstr(line.data,"r-xp 0000000000003000 00:00 42 /usr/apps/chrome/lib/libGLX_nvidia.so.0\n"));
 strcpy(backend.path,"/usr/apps/chrome/lib/renamed.so");line.length=0;
 assert(linux_proc_append_maps_line(&line,&area,0)&&strstr(line.data,"renamed.so\n"));
 backend.detached=true;line.length=0;
 assert(linux_proc_append_maps_line(&line,&area,0)&&strstr(line.data,"renamed.so (deleted)\n"));
 strcpy(backend.path,"/usr/apps/a/lib/line\nname.so");line.length=0;
 assert(linux_proc_append_maps_line(&line,&area,0)&&strstr(line.data,"line\\012name.so (deleted)\n"));
 memset(backend.path,'\n',sizeof(backend.path));backend.path[0]='/';backend.path[255]=0;
 line.length=0;assert(linux_proc_append_maps_line(&line,&area,0));
 unsigned newlines=0;for(size_t i=0;i<line.length;++i)if(line.data[i]=='\n')++newlines;
 assert(newlines==1 && line.length<128+4*256);
 char path[8];bool deleted;
 assert(vfs_vnode_path_kernel(&vnode,path,sizeof(path),&deleted)==K_ENAMETOOLONG&&!locks);
 assert(vfs_vnode_path_kernel(0,path,sizeof(path),&deleted)==K_EBADF);
 vnode.fs_private=0;line.length=0;
 assert(linux_proc_append_maps_line(&line,&area,0)&&strstr(line.data,"[file]\n"));
 area.object=0;line.length=0;area.flags=VM_MAP_STACK;
 assert(linux_proc_append_maps_line(&line,&area,0)&&strstr(line.data,"[stack]\n"));
 line.length=sizeof(line.data)-1;
 assert(!linux_proc_append_maps_line(&line,&area,0)&&!locks);
}
'''
with tempfile.TemporaryDirectory(prefix='proc-map-path-') as directory:
    binary=Path(directory)/'test'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-x','c','-','-o',str(binary)],
                   input=code,text=True,check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: actual mapped-library paths, offsets, inode, rename/deleted snapshots, newline escaping and bounded output')
