"""Production synthetic mount parents and children versus independent namespace oracle."""
from pathlib import Path
import resource
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/fs/vfs/core.c').read_text()
header = (root/'include/uapi/file.h').read_text()
layout = header[header.index('enum os_file_type {'):header.index('typedef struct os_file_enumerate {')]
append = source[source.index('static size_t vfs_file_name_length('):source.index('typedef struct vfs_directory_collection')]
helpers = source[source.index('static const char *vfs_mount_child_name('):source.index('kstatus_t vfs_enumerate_kernel_batch(')]
mount_limit = int(re.search(r'#define VFS_FAT32_MOUNT_LIMIT (\d+)U',
                           (root/'kernel/fs/vfs/internal.h').read_text()).group(1))
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define OS_FILE_NAME_MAX 64U
#define VFS_DIRECTORY_ENTRY_LIMIT 256U
#define VFS_FAT32_MOUNT_LIMIT MOUNT_LIMIT_VALUE
''' + layout + r'''
typedef struct {bool mounted;char prefix[256];} vfs_fat32_mount_t;
static vfs_fat32_mount_t g_fat32_mounts[VFS_FAT32_MOUNT_LIMIT];
static size_t scans;
static size_t vfs_string_length(const char *p){++scans;return strnlen(p,256);}
static bool vfs_path_range_equals(const char *a,size_t n,const char *b,size_t m){return n==m&&!memcmp(a,b,n);}
''' + append + helpers + r'''
typedef struct {uint64_t front[4];os_file_info_t entries[256];uint64_t back[4];} arena_t;
static arena_t actual,expected;
static const char *oracle_child(const vfs_fat32_mount_t *m,const char *path,size_t length,size_t *size){
 if(!m->mounted)return 0;
 size_t n=strnlen(m->prefix,256);if(n<=1)return 0;
 size_t last=0;
 for(size_t i=0;i<n;++i)if(m->prefix[i]=='/')last=i+1;
 size_t parent=last>1?last-1:1;
 if(parent!=length || memcmp(path,m->prefix,length))return 0;
 *size=n-last;return m->prefix+last;
}
static bool oracle_append(const char *path,size_t length,unsigned *count,bool *exists){
 for(unsigned i=0;i<VFS_FAT32_MOUNT_LIMIT;++i){
  size_t size;const char *name=oracle_child(&g_fat32_mounts[i],path,length,&size);
  if(!name)continue;*exists=true;if(!size||size>=64)return false;
  os_file_info_t entry={.type=OS_FILE_TYPE_DIRECTORY,.mode=0040755};memcpy(entry.name,name,size);
  unsigned slot=0;while(slot<*count && strcmp(expected.entries[slot].name,entry.name))++slot;
  if(slot==*count){if(*count==256)return false;++*count;}
  expected.entries[slot]=entry;
 }
 return true;
}
int main(void){
 char short_edge[80]="/",long_edge[80]="/";
 memset(short_edge+1,'x',63);short_edge[64]=0;
 memset(long_edge+1,'x',64);long_edge[65]=0;
 const char *prefixes[]={"/","/vol","/vol/nested","/volx","/vol/","//child",short_edge,long_edge};
 const char *paths[]={"/","/vol","/volx","/vol/nested","//","/v","/vol/","/missing","/x","/vol/nestedx","/bad"};
 unsigned cases=0;
 for(unsigned config=0;config<512;++config)for(unsigned flags=0;flags<8;++flags){
  memset(g_fat32_mounts,0,sizeof(g_fat32_mounts));unsigned value=config;
  const unsigned slots[]={0,VFS_FAT32_MOUNT_LIMIT/2,VFS_FAT32_MOUNT_LIMIT-1};
  for(unsigned i=0;i<3;++i){strcpy(g_fat32_mounts[slots[i]].prefix,prefixes[value%8]);value/=8;
   g_fat32_mounts[slots[i]].mounted=(flags&(1U<<i))!=0;}
  for(unsigned p=0;p<sizeof(paths)/sizeof(paths[0]);++p)for(unsigned c=0;c<3;++c){
   unsigned counts[]={0,1,256};unsigned a=counts[c],b=a;
   memset(&actual,0xa5,sizeof(actual));
   for(unsigned i=0;i<a;++i){actual.entries[i]=(os_file_info_t){.type=OS_FILE_TYPE_REGULAR,.size=i+1};
    snprintf(actual.entries[i].name,64,"node%03u",i);}
   if(a)strcpy(actual.entries[0].name,"vol");
   expected=actual;bool ae=false,be=false,parent=false;
   size_t length=strlen(paths[p]);
   for(unsigned i=0;i<VFS_FAT32_MOUNT_LIMIT;++i){size_t n;if(oracle_child(&g_fat32_mounts[i],paths[p],length,&n))parent=true;}
   scans=0;assert(vfs_mount_parent_exists(paths[p],length)==parent);
   assert(scans<=3);scans=0;
   bool result=oracle_append(paths[p],length,&b,&be);
   assert(vfs_append_mount_children(paths[p],length,actual.entries,&a,&ae)==result);
   assert(a==b && ae==be && !memcmp(&actual,&expected,sizeof(actual)) && scans<=3);
   ++cases;
  }
 }
 puts("Mount children: root/nested/boundary parents, duplicate directory promotion, full tables, 63/64-byte names and canaries passed");
 printf("%u independent lookup/listing cases passed\n",cases);
}
'''
program = program.replace('MOUNT_LIMIT_VALUE', str(mount_limit))
with tempfile.TemporaryDirectory(prefix='liteos-mount-child-') as directory:
    binary = str(Path(directory)/'test')
    def run(code, negative=False):
        subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                       input=code, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, text=True,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        if negative:
            assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
            print('Negative control rejected: unrelated mount counted as direct child')
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
    run(program)
    boundary = '!vfs_path_range_equals(path, path_length, mount->prefix, parent_length)'
    assert boundary in program
    run(program.replace(boundary, '((void)path, false)'), True)
