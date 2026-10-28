#!/usr/bin/env python3
"""Exercise production in-memory rename publication and replacement semantics."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
def extract(begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define VFS_PATH_LIMIT 256U
typedef int kstatus_t;
typedef struct {unsigned type;} os_file_info_t;
enum {K_OK, K_EINVAL, K_EXDEV, K_EIO, K_ENOENT, K_EACCES,
      K_ENOTDIR, K_EEXIST, K_ENAMETOOLONG, K_EISDIR, K_ENOTEMPTY};
typedef struct vnode { void *fs_private; unsigned refs; } vnode_t;
typedef struct { char path[256]; bool directory, mutable_node, fat32_node; const char *data;
 vnode_t *namespace_next;uint32_t namespace_slot; } vfs_backend_node_t;
typedef struct { void *filesystem; } vfs_fat32_mount_t;
static vnode_t *g_vfs_nodes[8];
static vnode_t *g_vfs_buckets[8];
static uint32_t g_vfs_fat_buckets[8],g_vfs_fat_next[8];
static uint32_t g_vfs_node_capacity=8;
static uint32_t g_vfs_next_free_node;
static unsigned locked, puts_count;
static bool parent_exists=true;
static void vfs_initialize(void) {}
static void vfs_lock(void) { assert(!locked); locked=1; }
static void vfs_unlock(void) { assert(locked); locked=0; }
static void object_put(vnode_t *v) { assert(!locked && v->refs); --v->refs; ++puts_count; }
static size_t vfs_string_length(const char *p) { return strlen(p); }
''' + extract('static uint32_t vfs_namespace_bucket(', 'static bool vfs_fat_relative_path_equal(') + r'''
static kstatus_t stat_failure;
static unsigned stat_calls;
static kstatus_t vfs_stat_kernel(const char *p,os_file_info_t *info) {
    assert(!locked&&info);++stat_calls;
    if(stat_failure)return stat_failure;
    return vfs_find_node_locked(p)?K_OK:K_ENOENT;
}
static bool vfs_directory_exists_locked(const char *p, size_t n) { (void)p; (void)n; return false; }
static bool vfs_parent_directory_exists_locked(const char *p, size_t n) { (void)p; (void)n; return parent_exists; }
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *p, char *r) { (void)p; r[0]=0; return NULL; }
static kstatus_t vfs_rename_fat32_cached(const char *a,size_t b,const char *c,size_t d,
 vfs_fat32_mount_t *m,const char *r,const char *s) {
 (void)a;(void)b;(void)c;(void)d;(void)m;(void)r;(void)s;assert(0);return K_EIO;
}
''' + extract('static bool vfs_path_range_equals(', 'static uint32_t vfs_free_node_slot_locked(')
code += extract('static void vfs_set_node_slot_locked(', 'static bool vfs_volatile_path(')
code += extract('static bool vfs_path_is_subtree(', 'static kstatus_t vfs_rekey_cached_subtree_locked(')
code += extract('kstatus_t vfs_rename_kernel(', 'static size_t vfs_file_name_length(')
code += r'''
static vfs_backend_node_t b[4]; static vnode_t v[4];
static void reset(void) {
    memset(b,0,sizeof(b)); memset(v,0,sizeof(v)); memset(g_vfs_nodes,0,sizeof(g_vfs_nodes));
    memset(g_vfs_buckets,0,sizeof(g_vfs_buckets));
    memset(g_vfs_fat_buckets,0,sizeof(g_vfs_fat_buckets));
    memset(g_vfs_fat_next,0,sizeof(g_vfs_fat_next));
    puts_count=0; parent_exists=true;stat_failure=0;stat_calls=0;
    g_vfs_next_free_node=2;
    for(unsigned i=0;i<4;++i) { v[i].fs_private=&b[i]; v[i].refs=2; b[i].mutable_node=true; }
    strcpy(b[0].path,"/tmp/new"); b[0].data="new contents"; g_vfs_nodes[0]=&v[0];
    strcpy(b[1].path,"/tmp/Preferences"); b[1].data="old contents"; g_vfs_nodes[1]=&v[1];
    for(unsigned i=0;i<2;++i)vfs_namespace_insert_locked(&v[i],i);
}
int main(void) {
    reset();
    assert(vfs_rename_kernel("/tmp/missing","/tmp/missing")==K_ENOENT);
    assert(stat_calls==1&&!puts_count&&g_vfs_nodes[0]==&v[0]);
    assert(vfs_rename_kernel("/tmp/new","/tmp/new")==K_OK);
    b[0].directory=true;b[0].mutable_node=false;
    assert(vfs_rename_kernel("/tmp/new","/tmp/new")==K_OK);
    assert(stat_calls==3&&!puts_count&&g_vfs_nodes[0]==&v[0]);
    stat_failure=K_EIO;
    assert(vfs_rename_kernel("/tmp/new","/tmp/new")==K_EIO);
    assert(stat_calls==4&&!puts_count&&!locked);
    reset();
    assert(vfs_rename_kernel(b[0].path,b[1].path)==K_OK);
    assert(vfs_find_node_locked("/tmp/Preferences")==&v[0]);
    assert(!vfs_find_node_locked("/tmp/new"));
    assert(v[1].refs==1 && puts_count==1 && !strcmp(b[1].data,"old contents"));
    assert(g_vfs_next_free_node==1);
    assert(!strcmp(b[0].data,"new contents"));
    reset(); b[1].mutable_node=false;
    assert(vfs_rename_kernel("/tmp/new","/tmp/Preferences")==K_EACCES);
    assert(g_vfs_nodes[0]==&v[0] && g_vfs_nodes[1]==&v[1] && !puts_count);
    reset(); b[1].directory=true;
    assert(vfs_rename_kernel("/tmp/new","/tmp/Preferences")==K_EISDIR);
    reset(); b[0].directory=true;
    assert(vfs_rename_kernel("/tmp/new","/tmp/Preferences")==K_ENOTDIR);
    reset(); b[0].directory=b[1].directory=true;
    strcpy(b[2].path,"/tmp/Preferences/child"); vfs_set_node_slot_locked(2,&v[2]);
    assert(vfs_rename_kernel("/tmp/new","/tmp/Preferences")==K_ENOTEMPTY);
    assert(g_vfs_nodes[1]==&v[1] && !puts_count);
    reset(); b[0].directory=b[1].directory=true;
    strcpy(b[2].path,"/tmp/new/child"); b[2].mutable_node=false; vfs_set_node_slot_locked(2,&v[2]);
    assert(vfs_rename_kernel("/tmp/new","/tmp/Preferences")==K_EACCES);
    assert(g_vfs_nodes[1]==&v[1] && !puts_count);
    b[2].mutable_node=true;
    assert(vfs_rename_kernel("/tmp/new","/tmp/Preferences")==K_OK);
    assert(!strcmp(b[2].path,"/tmp/Preferences/child"));
    reset(); b[0].directory=b[1].directory=true;
    vfs_namespace_remove_locked(&v[1]);
    memset(b[1].path,'x',250); memcpy(b[1].path,"/tmp/",5); b[1].path[250]=0;
    vfs_namespace_insert_locked(&v[1],1);
    strcpy(b[2].path,"/tmp/new/child"); vfs_set_node_slot_locked(2,&v[2]);
    assert(vfs_rename_kernel("/tmp/new",b[1].path)==K_ENAMETOOLONG);
    assert(g_vfs_nodes[1]==&v[1] && !puts_count && !strcmp(b[2].path,"/tmp/new/child"));
    reset(); parent_exists=false;
    assert(vfs_rename_kernel("/tmp/new","/tmp/Preferences")==K_ENOTDIR);
    assert(g_vfs_nodes[1]==&v[1] && !puts_count);
    reset(); assert(vfs_rename_kernel("/tmp/missing","/tmp/Preferences")==K_ENOENT);
    assert(g_vfs_nodes[1]==&v[1] && !puts_count);
    assert(!locked);
    puts("VFS rename: same-path existence/error propagation; replacement, old-open identity, directory/type/permission failures passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-rename-replace-') as directory:
    binary = str(pathlib.Path(directory) / 'test')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
