"""Actual directory lookup: first-match precedence and bounded prefix scans."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
directory = source[source.index('static bool vfs_directory_exists_locked('):
                   source.index('static bool vfs_parent_directory_exists_locked(')]
equals = source[source.index('static bool vfs_path_range_equals('):
                source.index('static uint32_t vfs_free_node_slot_locked(')]
parent = source[source.index('static bool vfs_parent_directory_exists_locked('):
                source.index('static void vfs_set_leaf_name(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define VFS_PATH_LIMIT 256U
#define OS_FILE_TYPE_DIRECTORY 2
/* Put the path last so the fixture can guard the byte immediately after it. */
typedef struct {bool directory;char path[256];} vfs_backend_node_t;
typedef struct {void *fs_private;} vnode_t;
typedef struct {unsigned type;} os_file_info_t;
static vnode_t *g_vfs_nodes[4];static uint32_t g_vfs_node_capacity;
static bool mounted;static unsigned mounted_type,scans;
static bool path_sensitive;static unsigned fat_calls;
static size_t vfs_string_length(const char *p){
 ++scans;size_t n=0;while(n<256&&p[n])++n;return n;
}
static bool vfs_fat32_stat_absolute(const char *p,os_file_info_t *info,char *relative){
 assert(p&&relative);++fat_calls;
 if(path_sensitive){
  if(!strcmp(p,"/disk/parent")){info->type=OS_FILE_TYPE_DIRECTORY;return true;}
  if(!strcmp(p,"/disk/parent/file")){info->type=1;return true;}
  return false;
 }
 info->type=mounted_type;return mounted;
}
''' + equals + directory + parent + r'''
static bool previous_directory(const char *path,size_t n){
 if(!path||!n||n>256)return false;
 if(n==1&&path[0]=='/')return true;
 if(n==4&&vfs_path_range_equals(path,n,"/tmp",4))return true;
 /* FAT 查询修正为父目录前缀；内存节点仍与原有逐槽算法比较。 */
 char relative[256],prefix[256];os_file_info_t info;
 if(n<256){memcpy(prefix,path,n);prefix[n]=0;
  if(vfs_fat32_stat_absolute(prefix,&info,relative))return info.type==OS_FILE_TYPE_DIRECTORY;}
 for(uint32_t i=0;i<g_vfs_node_capacity;++i){
  vnode_t *v=g_vfs_nodes[i];vfs_backend_node_t *b=v?(vfs_backend_node_t*)v->fs_private:0;
  if(!b)continue;
  size_t length=vfs_string_length(b->path);
  if(vfs_path_range_equals(b->path,length,path,n))return b->directory;
  if(length>n&&b->path[n]=='/'&&vfs_path_range_equals(b->path,n,path,n))return true;
 }
 return false;
}
static unsigned cases,reduced;
static void compare(const char *p,size_t n){
 scans=0;bool expected=previous_directory(p,n);unsigned old=scans;
 scans=0;assert(vfs_directory_exists_locked(p,n)==expected&&!scans);
 ++cases;if(old)++reduced;
}
int main(void){
 vfs_backend_node_t b[4];vnode_t v[4];char request[256];
 memset(b,0,sizeof(b));
 for(unsigned i=0;i<4;++i){v[i].fs_private=&b[i];g_vfs_nodes[i]=&v[i];}
 g_vfs_node_capacity=1;
 for(unsigned mode=0;mode<4;++mode){
  mounted=mode>1;mounted_type=(mode&1)?OS_FILE_TYPE_DIRECTORY:1;
  for(unsigned a=0;a<=256;++a)for(unsigned c=0;c<=256;++c)
  for(unsigned dir=0;dir<2;++dir){
   memset(request,'x',256);memset(b[0].path,'x',256);
   request[0]=b[0].path[0]='/';
   if(a<256)request[a]=0;if(c<256)b[0].path[c]=0;
   b[0].directory=dir;compare(request,a);
  }
 }
 mounted=false;g_vfs_node_capacity=4;
 /* A supplied parent-prefix length need not be the whole request length. */
 g_vfs_node_capacity=1;
 for(unsigned n=0;n<=256;++n){
  memset(request,'x',256);memset(b[0].path,'x',256);
  if(n<256)request[n]=b[0].path[n]=0;
  b[0].directory=true;
  for(unsigned supplied=0;supplied<=256;++supplied)compare(request,supplied);
 }
 g_vfs_node_capacity=4;
 const char *paths[]={"/tmp/a","/tmp/ab","/tmp/a/child","/tmp/b","/tmp","/",""};
 for(unsigned config=0;config<2401;++config){
  unsigned value=config;
  for(unsigned i=0;i<4;++i){strcpy(b[i].path,paths[value%7]);value/=7;}
  for(unsigned flags=0;flags<16;++flags){
   for(unsigned i=0;i<4;++i)b[i].directory=(flags&(1U<<i))!=0;
   for(unsigned p=0;p<7;++p)compare(paths[p],strlen(paths[p]));
  }
 }
 g_vfs_nodes[0]=0;v[1].fs_private=0;
 compare("/tmp/a",6);compare(0,0);compare("/x",SIZE_MAX);
 /* 子文件存在或不存在，都必须查询父目录，不能查询完整子文件名。 */
 path_sensitive=true;g_vfs_node_capacity=0;
 const char *child="/disk/parent/file";
 assert(vfs_parent_directory_exists_locked(child,strlen(child)));
 child="/disk/parent/new-file";
 assert(vfs_parent_directory_exists_locked(child,strlen(child)));
 assert(!vfs_directory_exists_locked("/disk/parent/file",17));
 assert(!vfs_parent_directory_exists_locked("/disk/missing/file",18));
 path_sensitive=false;
 /* Path ends at the protected page: [256] must never be read. */
 long page=sysconf(_SC_PAGESIZE);assert(page>=(long)sizeof(b[0]));
 char *arena=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,
                  MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(arena!=MAP_FAILED);
 assert(!mprotect(arena+page,(size_t)page,PROT_NONE));
 vfs_backend_node_t *edge=(void*)(arena+page-sizeof(b[0]));
 memset(edge,0,sizeof(*edge));edge->directory=true;
 v[0].fs_private=edge;g_vfs_nodes[0]=&v[0];g_vfs_node_capacity=1;
 memset(edge->path,'x',256);memset(request,'x',256);compare(request,256);
 unsigned calls=fat_calls;
 assert(vfs_directory_exists_locked(edge->path,256));
 assert(fat_calls==calls);
 assert(!munmap(arena,(size_t)page*2));
 printf("Directory scans: %u old/new cases, %u candidate-scanning routes now skip strlen; bounded paths, mounted status, holes and first-match precedence preserved\n",cases,reduced);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-directory-scan-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-O2', '-Wall', '-Wextra',
                    '-Werror', '-Wno-misleading-indentation',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
