"""Actual registry lookup: bounded paths and one request scan per lookup."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/vfs/core.c').read_text()
length=source[source.index('size_t vfs_string_length('):source.index('static void vnode_destroy(')]
lookup=source[source.index('static vnode_t *vfs_find_node_locked('):source.index('static bool vfs_fat_relative_path_equal(')]
index_helpers=source[source.index('static uint32_t vfs_namespace_bucket('):source.index('static vnode_t *vfs_find_node_locked(')]
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define VFS_PATH_LIMIT 256U
typedef struct vnode { void *fs_private; } vnode_t;
typedef struct { vnode_t *namespace_next;uint32_t namespace_slot;bool fat32_node;uint8_t reserved[3];char path[VFS_PATH_LIMIT]; } vfs_backend_node_t;
static vnode_t **g_vfs_nodes;
static vnode_t **g_vfs_buckets;
static uint32_t *g_vfs_fat_buckets,*g_vfs_fat_next;
static uint32_t g_vfs_node_capacity;
static const char *request;
static unsigned request_scans,candidate_scans;
''' + length.replace('vfs_string_length(', 'production_length(') + r'''
static size_t vfs_string_length(const char *s) {
    if(s==request) ++request_scans;
    else ++candidate_scans;
    return production_length(s);
}
''' + index_helpers + lookup + r'''
static vnode_t *previous_lookup(const char *path) {
 size_t n=vfs_string_length(path);
 for(uint32_t i=0;i<g_vfs_node_capacity;++i){
  vnode_t *v=g_vfs_nodes[i];
  vfs_backend_node_t *b=v?(vfs_backend_node_t*)v->fs_private:0;
  if(!b)continue;
  if(n==vfs_string_length(b->path)){
   size_t j=0;while(j<n&&path[j]==b->path[j])++j;
   if(j==n)return v;
  }
 }
 return 0;
}
static void check(const char *path,vnode_t *expected) {
    memset(g_vfs_buckets,0,(size_t)g_vfs_node_capacity*sizeof(*g_vfs_buckets));
    for(unsigned i=0;i<g_vfs_node_capacity;++i)vfs_namespace_insert_locked(g_vfs_nodes[i],i);
    /* 移除再插入同一个节点也必须保留桶链顺序。 */
    if(expected){vfs_backend_node_t *backend=expected->fs_private;
        unsigned slot=backend->namespace_slot;
        vfs_namespace_remove_locked(expected);vfs_namespace_insert_locked(expected,slot);}
    request=path;request_scans=candidate_scans=0;
    assert(vfs_find_node_locked(path)==expected);
    assert(request_scans==1&&!candidate_scans);
}
int main(void) {
    const unsigned n=32768;
    vfs_backend_node_t *b=calloc(n,sizeof(*b));
    vnode_t *v=calloc(n,sizeof(*v));
    g_vfs_nodes=calloc(n,sizeof(*g_vfs_nodes));assert(b && v && g_vfs_nodes);
    g_vfs_buckets=calloc(n,sizeof(*g_vfs_buckets));assert(g_vfs_buckets);
    for(unsigned i=0;i<n;++i) {
        snprintf(b[i].path,sizeof(b[i].path),"/tmp/profile/%05u",i);
        v[i].fs_private=&b[i];g_vfs_nodes[i]=&v[i];
    }
    char needle[VFS_PATH_LIMIT];
    g_vfs_node_capacity=n;
    strcpy(needle,b[0].path);check(needle,&v[0]);
    strcpy(needle,b[n-1].path);check(needle,&v[n-1]);
    check("/tmp/profile/99999",0);
    check("/tmp/profile/00000/child",0);
    check("/tmp/profile/0000",0);
    g_vfs_nodes[0]=0;v[1].fs_private=0;
    check("/tmp/profile/00000",0);check("/tmp/profile/00001",0);
    check("",0);check(0,0);
    /* Preserve bounded comparison: no byte beyond the 256-byte limit is read. */
    memset(needle,'x',sizeof(needle));memset(b[n-1].path,'x',sizeof(b[n-1].path));
    check(needle,&v[n-1]);needle[255]='y';check(needle,0);
    g_vfs_node_capacity=0;check("/tmp/empty",0);
    /* Independent original algorithm, all terminated/unterminated lengths. */
    g_vfs_node_capacity=1;g_vfs_nodes[0]=&v[0];v[0].fs_private=&b[0];
    unsigned cases=0;
    for(unsigned a=0;a<=256;++a)for(unsigned c=0;c<=256;++c){
        memset(needle,'x',256);if(a<256)needle[a]=0;
        memset(b[0].path,'x',256);if(c<256)b[0].path[c]=0;
        request=needle;vnode_t *expected=previous_lookup(needle);
        check(needle,expected);++cases;
    }
    uint32_t random=0x51a3c0de;
    for(unsigned i=0;i<50000;++i){
        random=random*1664525U+1013904223U;
        unsigned n=random%257;
        for(unsigned j=0;j<256;++j)needle[j]=(char)(1+(j*29+i)%255);
        if(n<256)needle[n]=0;memcpy(b[0].path,needle,256);
        random=random*1664525U+1013904223U;
        b[0].path[random%256]^=(char)(1+random%255);
        request=needle;vnode_t *expected=previous_lookup(needle);
        check(needle,expected);++cases;
    }
    memset(b[0].path,0,256);request=0;
    check(0,previous_lookup(0));
    /* Candidate ends exactly at a protected page: bound must not read [256]. */
    long page=sysconf(_SC_PAGESIZE);assert(page>=256);
    char *guard=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(guard!=MAP_FAILED);
    assert(!mprotect(guard+page,(size_t)page,PROT_NONE));
    v[0].fs_private=guard+page-sizeof(vfs_backend_node_t);
    for(unsigned n=0;n<=256;++n){
        memset(needle,'x',256);memset(guard+page-256,'x',256);
        if(n<256){needle[n]=0;guard[page-256+n]=0;}
        check(needle,&v[0]);
    }
    assert(!munmap(guard,(size_t)page*2));
    free(g_vfs_nodes);free(g_vfs_buckets);free(v);free(b);
    puts("VFS lookup: 32768-node hit/miss scans request once; empty registry, holes, exact/descendant names, null and bounded nonterminated paths passed");
    printf("Candidate comparison: %u original-oracle length/mutation cases; zero candidate strlen scans; guarded 256-byte boundary passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vfs-lookup-') as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-D_GNU_SOURCE','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-Wno-misleading-indentation',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)
