"""Actual mount publication versus duplicate full-length-scan oracle."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
limit = re.search(r'#define VFS_FAT32_MOUNT_LIMIT (\d+)U',
                 (root / 'kernel/fs/vfs/internal.h').read_text()).group(1)
def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]
mount = function('kstatus_t vfs_mount_fat32(')
equals = function('static bool vfs_path_range_equals(')
new = '''            g_fat32_mounts[index].prefix[length] == 0 &&
            vfs_path_range_equals(g_fat32_mounts[index].prefix,
                                  length, prefix, length)'''
assert new in mount
old = mount.replace('vfs_mount_fat32(', 'previous_mount(').replace(new, '''            vfs_path_range_equals(g_fat32_mounts[index].prefix,
                                  vfs_string_length(g_fat32_mounts[index].prefix),
                                  prefix, length)''')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sanitizer/asan_interface.h>
#include <sys/wait.h>
#include <unistd.h>
#define VFS_PATH_LIMIT 256U
#define VFS_FAT32_MOUNT_LIMIT LIMIT_VALUE
typedef int kstatus_t;
enum {K_OK,K_EINVAL,K_EBUSY,K_ENOMEM};
typedef struct {bool Mounted;} OS_FAT32;
typedef struct {char prefix[256];OS_FAT32 *filesystem;bool mounted;} vfs_fat32_mount_t;
static vfs_fat32_mount_t g_fat32_mounts[VFS_FAT32_MOUNT_LIMIT];
static unsigned depth,locks,initializations,scans,cases,avoided;
static const char *request;
static void vfs_initialize(void){assert(!depth);++initializations;}
static void vfs_lock(void){assert(!depth);++depth;++locks;}
static void vfs_unlock(void){assert(depth==1);--depth;}
static size_t vfs_string_length(const char *p){
 if(p!=request)++scans;
 size_t n=0;while(n<256&&p[n])++n;return n;
}
''' + equals + old + mount + r'''
static OS_FAT32 fs={true},invalid_fs={false};
static void reset(unsigned mask){
 memset(g_fat32_mounts,0x71,sizeof(g_fat32_mounts));
 for(unsigned i=0;i<VFS_FAT32_MOUNT_LIMIT;++i){
  snprintf(g_fat32_mounts[i].prefix,256,"/existing/%u/long/deep/path",i);
  g_fat32_mounts[i].filesystem=&fs;
  g_fat32_mounts[i].mounted=mask==1||(mask==2&&i%2==0)||(mask==3&&i%2!=0);
 }
}
static void compare(const char *p,OS_FAT32 *f){
 vfs_fat32_mount_t initial[VFS_FAT32_MOUNT_LIMIT],expected[VFS_FAT32_MOUNT_LIMIT];
 memcpy(initial,g_fat32_mounts,sizeof(initial));request=p;
 depth=locks=initializations=scans=0;kstatus_t status=previous_mount(p,f);
 assert(!depth);unsigned l=locks,n=initializations,s=scans;
 memcpy(expected,g_fat32_mounts,sizeof(expected));memcpy(g_fat32_mounts,initial,sizeof(initial));
 depth=locks=initializations=scans=0;
 assert(vfs_mount_fat32(p,f)==status&&!depth&&locks==l&&initializations==n&&!scans);
 assert(!memcmp(g_fat32_mounts,expected,sizeof(expected)));avoided+=s;++cases;
}
int main(void){
 char path[256];
 for(unsigned length=1;length<=256;++length)for(unsigned fill=0;fill<4;++fill)
 for(unsigned duplicate=0;duplicate<VFS_FAT32_MOUNT_LIMIT+1;++duplicate){
  memset(path,'x',256);path[0]='/';if(length<256)path[length]=0;
  reset(fill);
  if(duplicate<VFS_FAT32_MOUNT_LIMIT){
   memcpy(g_fat32_mounts[duplicate].prefix,path,256);
   g_fat32_mounts[duplicate].mounted=true;
  }
  compare(path,&fs);compare(path,&invalid_fs);compare(path,0);
  if(length>1&&length<256){path[length-1]='/';compare(path,&fs);}
 }
 reset(0);compare(0,&fs);compare("",&fs);compare("relative",&fs);
 /* The request may alias the selected free slot or another registry path;
  * retain the existing forward-copy publication semantics. */
 for(unsigned slot=0;slot<VFS_FAT32_MOUNT_LIMIT;++slot){
  reset(0);strcpy(g_fat32_mounts[slot].prefix,"/alias");
  compare(g_fat32_mounts[slot].prefix,&fs);
 }
 /* Requests ending at 6 need no byte after index 6 of other prefixes. */
 reset(0);g_fat32_mounts[1].mounted=true;strcpy(g_fat32_mounts[1].prefix,"/other/deep/path");
 __asan_poison_memory_region(g_fat32_mounts[1].prefix+7,249);
 request="/mount";depth=0;assert(vfs_mount_fat32(request,&fs)==K_OK&&!depth);
 pid_t pid=fork();assert(pid>=0);
 if(!pid){assert(freopen("/dev/null","w",stderr));previous_mount(request,&fs);_exit(0);}
 int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==1);
 __asan_unpoison_memory_region(g_fat32_mounts[1].prefix+7,249);
 assert(avoided);printf("Mount registration: %u exact status/full-table/lock/publication cases; %u candidate length scans eliminated; aliases and poisoned tails passed; old scan fails\n",cases,avoided);
}
'''
code = code.replace('LIMIT_VALUE', limit + 'U')
with tempfile.TemporaryDirectory(prefix='liteos-mount-registration-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
