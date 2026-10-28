"""Actual volatile routing: bounded prefix versus full-path oracle."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/vfs/core.c').read_text()
start=source.index('static bool vfs_volatile_path(')
function=source[start:source.index('static bool vfs_mount_matches(',start)]
function=function.replace('path[index + offset]', 'tracked_char(path,index+offset)').replace('path[index + sizeof(runtime) - 1U]', 'tracked_char(path,index+sizeof(runtime)-1U)').replace('path[index]', 'tracked_char(path,index)')
program=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#define VFS_PATH_LIMIT 256U
static unsigned reads,length_reads,cases;
static char tracked_char(const char *p,size_t i){++reads;return p[i];}
''' + function + r'''
static bool previous(const char *path){
 if(!path)return false;size_t n=0;
 while(n<VFS_PATH_LIMIT){++length_reads;if(!path[n])break;++n;}
 return n>=4&&!memcmp(path,"/tmp",4)&&(n==4||path[4]=='/');
}
static void check(const char *p){
 reads=length_reads=0;bool expected=previous(p);
 assert(vfs_volatile_path(p)==expected&&reads<=32);++cases;
}
int main(void){
 const char *fixed[]={0,"","/","/t","/tm","/tmp","/tmp/","/tmpx","/tmpfoo/","/var/tmp","/TMP/"};
 for(unsigned i=0;i<sizeof(fixed)/sizeof(fixed[0]);++i)check(fixed[i]);
 char path[256];
 for(unsigned pos=0;pos<5;++pos)for(unsigned byte=0;byte<256;++byte){
  memset(path,'x',sizeof(path));memcpy(path,"/tmp/",5);path[255]=0;path[pos]=(char)byte;
  check(path);
 }
 uint64_t seed=17;
 for(unsigned trial=0;trial<50000;++trial){
  for(unsigned i=0;i<256;++i){seed=seed*6364136223846793005ULL+1;path[i]=(char)(seed>>32);}
  if(trial%3==0)memcpy(path,"/tmp",4);
  if(trial%7==0)path[4]='/';
  check(path);
 }
 memset(path,'x',sizeof(path));memcpy(path,"/tmp/",5);check(path);
 assert(length_reads==256&&reads<=10);
 const char *apps[]={"/usr/apps/chrome/run","/usr/apps/chrome/run/socket","/usr/apps/a-b_2/run/profile"};
 for(unsigned i=0;i<sizeof(apps)/sizeof(apps[0]);++i)assert(vfs_volatile_path(apps[i]));
 const char *persistent[]={"/usr/apps/chrome/data","/usr/apps/chrome/runtime","/usr/apps//run","/usr/apps/../run","/usr/apps/chrome/lib","/usr/apps/chrome/runx"};
 for(unsigned i=0;i<sizeof(persistent)/sizeof(persistent[0]);++i)assert(!vfs_volatile_path(persistent[i]));
 long unit=sysconf(_SC_PAGESIZE);assert(unit>256);
 char *area=mmap(0,2*(size_t)unit,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(area!=MAP_FAILED&&mprotect(area+unit,(size_t)unit,PROT_NONE)==0);
 for(unsigned n=0;n<=5;++n){
  char *edge=area+unit-n-1;memcpy(edge,"/tmp/",n);edge[n]=0;check(edge);
 }
 /* The prefix alone is sufficient; no sixth byte may be read. */
 memcpy(area+unit-5,"/tmp/",5);reads=0;
 assert(vfs_volatile_path(area+unit-5)&&reads<=10);
 assert(munmap(area,2*(size_t)unit)==0);
 printf("Volatile prefix: %u full-path oracle cases; all byte mutations, short strings and guard-page boundaries; application runtime isolation and bounded prefix reads\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-volatile-prefix-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True)
