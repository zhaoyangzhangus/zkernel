"""Production FAT lookup and mount routing: normalize each request once."""
from pathlib import Path
import subprocess
import tempfile
import re

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
mount_limit = int(re.search(r'#define VFS_FAT32_MOUNT_LIMIT (\d+)U',
                           (root / 'kernel/fs/vfs/internal.h').read_text()).group(1))

def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VFS_PATH_LIMIT 256U
#define VFS_FAT32_MOUNT_LIMIT MOUNT_LIMIT_VALUE
typedef struct { bool mounted; void *filesystem; char prefix[256]; } vfs_fat32_mount_t;
typedef struct { bool fat32_node,detached; char path[256]; } vfs_backend_node_t;
typedef struct { void *fs_private; unsigned drops; } vnode_t;
typedef int kstatus_t;
enum {K_OK,K_ENAMETOOLONG};
static vfs_fat32_mount_t g_fat32_mounts[VFS_FAT32_MOUNT_LIMIT];
static vnode_t **g_vfs_nodes;
static uint32_t g_vfs_node_capacity;
static uint32_t unused_bucket[1],unused_next[1];
static uint32_t *g_vfs_fat_buckets=unused_bucket,*g_vfs_fat_next=unused_next;
static void vfs_namespace_insert_locked(vnode_t *v,uint32_t slot){(void)v;(void)slot;}
static void vfs_namespace_remove_locked(vnode_t *v){(void)v;}
static const char *request;
static unsigned request_routes;
static unsigned total_routes;
static unsigned long compared_characters;
static uint32_t g_vfs_next_free_node;
static unsigned lock_depth;
static unsigned eviction_probes;
static unsigned lock_calls;
static bool remount_after_drop;
static vnode_t *replacement_after_drop;
static void vfs_lock(void){assert(!lock_depth);++lock_depth;++lock_calls;}
static void vfs_unlock(void){assert(lock_depth==1);--lock_depth;}
static void object_put(void *p){
 assert(!lock_depth);++((vnode_t*)p)->drops;
 if(remount_after_drop){g_fat32_mounts[7].filesystem=(void*)(uintptr_t)99;remount_after_drop=false;}
 if(replacement_after_drop){
  assert(!g_vfs_nodes[7]);g_vfs_nodes[7]=replacement_after_drop;
  replacement_after_drop=0;
 }
}
'''
for signature in ('size_t vfs_string_length(',
                  'static uint32_t vfs_fat_name_bucket(',
                  'static bool vfs_path_range_equals(',
                  'static bool vfs_volatile_path(',
                  'static bool vfs_mount_matches('):
    program += function(signature)
program += function('vfs_fat32_mount_t *vfs_fat32_mount_for_path(').replace(
    'vfs_fat32_mount_for_path(', 'production_mount_for_path(')
program += r'''
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *p,char *r){
    ++total_routes;
    if(p==request)++request_routes;
    return production_mount_for_path(p,r);
}
'''
program += function('bool vfs_fat32_relative_path(').replace(
    'vfs_fat32_relative_path(', 'production_relative_path(')
program += r'''
static bool vfs_fat32_relative_path(const char *p, char *r) {
    return production_relative_path(p,r);
}
'''
if 'static bool vfs_fat_relative_path_equal(' in source:
    program += function('static bool vfs_fat_relative_path_equal(').replace(
        'char a = left_relative[index], b = right[suffix + index];',
        '++compared_characters; char a = left_relative[index], b = right[suffix + index];')
program += r'''
/* Previous identity wrapper retained only as a test oracle. */
static bool vfs_fat_path_equal(const char *left,const char *right){
 char relative[256];vfs_fat32_mount_t *m=vfs_fat32_mount_for_path(left,relative);
 return m&&vfs_fat_relative_path_equal(relative,vfs_string_length(relative),m,right);
}
'''
# 线性路径仍用于挂载根；本测试保留它作为名称比较和路由对照。
program += function('static vnode_t *vfs_find_fat_node_locked(').replace(
    'bool indexed = relative_length != 0U && g_vfs_node_capacity != 0U;',
    'bool indexed = false;')
program += function('static void vfs_set_node_slot_locked(')
program += function('static bool vfs_path_matches_subtree_prefix(')
program += function('static kstatus_t vfs_rekey_cached_subtree_locked(')
# Exercise the actual post-delete eviction loop, including lock/reference order.
remove_start = source.index('kstatus_t vfs_remove_typed_kernel(')
detach_start = source.index('        if (mounted_victim != 0) {',
                            source.index('/* 删除磁盘目录项后同步摘除规范 vnode', remove_start))
opening = source.index('{', detach_start)
depth = 1
detach_end = opening + 1
while depth:
    depth += (source[detach_end] == '{') - (source[detach_end] == '}')
    detach_end += 1
detach = source[detach_start:detach_end]
assert 'candidate != mounted_victim' in detach
detach = detach.replace('vnode_t *candidate = g_vfs_nodes[node_index];',
                        '++eviction_probes; vnode_t *candidate = g_vfs_nodes[node_index];')
program += 'static void detach_matching(vnode_t *mounted_victim){\n' + detach + '\n}\n'
program += r'''
static bool previous_relative_equal(const char *left,size_t length,const char *right){
    char relative[256];
    if(!vfs_fat32_relative_path(right,relative))return false;
    if(length!=vfs_string_length(relative))return false;
    for(size_t i=0;i<length;++i){
        char a=left[i],b=relative[i];
        if(a>='A'&&a<='Z')a=(char)(a-'A'+'a');
        if(b>='A'&&b<='Z')b=(char)(b-'A'+'a');
        if(a!=b)return false;
    }
    return true;
}
static bool previous_path_equal(const char *left,const char *right){
    char relative[256];
    if(!vfs_fat32_relative_path(left,relative))return false;
    return previous_relative_equal(relative,vfs_string_length(relative),right);
}
static bool identity_oracle(const char *left,const char *right){
    char a[256],b[256];
    vfs_fat32_mount_t *ma=production_mount_for_path(left,a);
    vfs_fat32_mount_t *mb=production_mount_for_path(right,b);
    return ma&&mb&&ma->filesystem==mb->filesystem&&previous_path_equal(left,right);
}
static void check(const char *p, vnode_t *expected) {
    request=p; request_routes=0;
    assert(vfs_find_fat_node_locked(p)==expected);
    assert(request_routes==1);
}
int main(void) {
    const unsigned n=32768;
    vfs_backend_node_t *b=calloc(n,sizeof(*b));
    vnode_t *v=calloc(n,sizeof(*v));
    g_vfs_nodes=calloc(n,sizeof(*g_vfs_nodes)); assert(b && v && g_vfs_nodes);
    g_vfs_node_capacity=n;
    g_fat32_mounts[0]=(vfs_fat32_mount_t){true,(void *)1,"/"};
    g_fat32_mounts[1]=(vfs_fat32_mount_t){true,(void *)2,"/vol"};
    g_fat32_mounts[2]=(vfs_fat32_mount_t){true,(void *)3,"/vol/nested"};
    for(unsigned i=0;i<n;++i) {
        snprintf(b[i].path,sizeof(b[i].path),"/profile/%05u",i);
        b[i].fat32_node=true; v[i].fs_private=&b[i]; g_vfs_nodes[i]=&v[i];
    }
    total_routes=0;compared_characters=0;check("/PROFILE/32767",&v[n-1]);
    assert(total_routes==2);
    /* 固定输入下统计实际字符访问，而不是用易受宿主负载影响的耗时断言。 */
    unsigned long forward_characters=0;
    const char *expected="PROFILE/32767";
    for(unsigned i=0;i<n;++i)for(size_t j=0;expected[j];++j){
        char a=expected[j],c=b[i].path[j+1];
        if(a>='A'&&a<='Z')a=(char)(a-'A'+'a');
        if(c>='A'&&c<='Z')c=(char)(c-'A'+'a');
        ++forward_characters;
        if(a!=c)break;
    }
    assert(compared_characters>0&&compared_characters*4<forward_characters);
    printf("32768 shared-directory nodes: compared characters %lu -> %lu\n",
           forward_characters,compared_characters);
    total_routes=0;check("/profile/missing",0);
    assert(total_routes==1);
    puts("32768 cached FAT nodes: hit mount routes 32769 -> 2; absent-name routes 32769 -> 1");
    /* A detached inode can remain alive for old handles, not for new opens. */
    b[n-1].detached=true;check("/PROFILE/32767",0);
    strcpy(b[n-2].path,b[n-1].path);check("/PROFILE/32767",&v[n-2]);
    snprintf(b[n-2].path,sizeof(b[n-2].path),"/profile/%05u",n-2);
    b[n-1].detached=false;
    check("/profile/32767/child",0); check("/profile/3276",0);
    g_vfs_nodes[0]=0;v[1].fs_private=0;b[2].fat32_node=false;
    check("/profile/00000",0);check("/profile/00001",0);check("/profile/00002",0);
    /* Different filesystems must not share a vnode just because names match. */
    strcpy(b[n-1].path,"/vol/nested/Deep/File");
    check("/vol/nested/deep/file",&v[n-1]);
    check("/Deep/File",0);
    assert(previous_path_equal("/vol/nested/Deep/File","/deep/file"));
    assert(!vfs_fat_path_equal("/vol/nested/Deep/File","/deep/file"));
    assert(!vfs_fat_path_equal("/vol/nested/Deep/File","/deep/files"));
    assert(!vfs_fat_path_equal("/tmp/file","/file"));
    /* Same-filesystem aliases retain canonical identity and ASCII folding. */
    g_fat32_mounts[2].filesystem=g_fat32_mounts[0].filesystem;
    check("/Deep/File",&v[n-1]);
    assert(vfs_fat_path_equal("/vol/nested/Deep/File","/deep/file"));
    b[n-1].detached=true;check("/Deep/File",0);
    strcpy(b[n-2].path,"/deep/file");
    check("/vol/nested/DEEP/FILE",&v[n-2]);
    snprintf(b[n-2].path,sizeof(b[n-2].path),"/profile/%05u",n-2);
    b[n-1].detached=false;
    strcpy(b[n-1].path,"/vol/nested");check("/",&v[n-1]);
    g_fat32_mounts[2].filesystem=(void*)3;
    check("/",0);
    check("/tmp/file",0);check("/tmp",0);check("relative",0);check("",0);check(0,0);
    char bounded[256]; memset(bounded,'x',sizeof(bounded));bounded[0]='/';check(bounded,0);
    g_fat32_mounts[0].mounted=false;check("/unmounted",0);
    g_vfs_node_capacity=0;check("/vol/empty",0);
    g_fat32_mounts[0].mounted=true;
    unsigned cases=0;char left[256],right[256];
    for(unsigned a=1;a<256;++a)for(unsigned b_length=1;b_length<256;++b_length){
        memset(left,'A',256);memset(right,'a',256);
        left[0]=right[0]='/';left[a]=0;right[b_length]=0;
        assert(vfs_fat_path_equal(left,right)==identity_oracle(left,right));
        ++cases;
    }
    uint32_t random=0x51a3c0de;
    for(unsigned i=0;i<50000;++i){
        random=random*1664525U+1013904223U;unsigned n=1+random%255;
        for(unsigned j=0;j<256;++j){
            random=random*1664525U+1013904223U;
            left[j]=(char)(random>>24);
        }
        left[0]='/';left[n]=0;memcpy(right,left,256);
        random=random*1664525U+1013904223U;
        right[random%256]^=(char)(1+random%255);
        assert(vfs_fat_path_equal(left,right)==identity_oracle(left,right));
        ++cases;
    }
    const char *paths[]={0,"","relative","/","/tmp","/tmp/x","/tmpx",
        "/vol","/vol/","/vol/nested","/vol/nested/Deep/File","/deep/file","/deep/files"};
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);++i)
    for(unsigned j=0;j<sizeof(paths)/sizeof(paths[0]);++j){
        assert(vfs_fat_path_equal(paths[i],paths[j])==identity_oracle(paths[i],paths[j]));
        ++cases;
    }
    memset(right,'x',256);right[0]='/';
    assert(!vfs_fat_path_equal("/x",right));
    /* Exercise every real mount slot and all filesystem-identity pairs. */
    unsigned identities=0;
    for(unsigned i=1;i<VFS_FAT32_MOUNT_LIMIT;++i){
        snprintf(g_fat32_mounts[i].prefix,256,"/disk%u",i);
        g_fat32_mounts[i].mounted=true;
        g_fat32_mounts[i].filesystem=(void*)(uintptr_t)(i+1);
    }
    for(unsigned alias=0;alias<2;++alias){
        for(unsigned i=0;i<VFS_FAT32_MOUNT_LIMIT;++i)
            g_fat32_mounts[i].filesystem=(void*)(uintptr_t)(alias?1:i+1);
        for(unsigned i=0;i<VFS_FAT32_MOUNT_LIMIT;++i)
        for(unsigned j=0;j<VFS_FAT32_MOUNT_LIMIT;++j){
            snprintf(left,256,"%s%s",g_fat32_mounts[i].prefix,i?"/File":"File");
            snprintf(right,256,"%s%s",g_fat32_mounts[j].prefix,j?"/file":"file");
            assert(vfs_fat_path_equal(left,right)==(alias||i==j));
            assert(vfs_fat_path_equal(left,right)==identity_oracle(left,right));
            ++identities;
        }
    }
    printf("Filesystem identity: %u pairs over all %u mount slots; old cross-filesystem alias rejected, same-filesystem aliases preserved\n",identities,VFS_FAT32_MOUNT_LIMIT);
    /* 大注册表中的首、中、尾槽位只扫描到命中位置；空目标完全不获取锁。 */
    g_vfs_node_capacity=n;
    for(unsigned i=0;i<n;++i)g_vfs_nodes[i]=&v[i];
    const unsigned positions[]={0,n/2,n-1};
    for(unsigned k=0;k<3;++k){
        unsigned index=positions[k];
        v[index].drops=0;eviction_probes=0;lock_calls=0;
        detach_matching(&v[index]);
        assert(eviction_probes==index+1&&lock_calls==1&&v[index].drops==1);
        assert(!g_vfs_nodes[index]&&!lock_depth);
        detach_matching(0);
        assert(eviction_probes==index+1&&lock_calls==1);
        g_vfs_nodes[index]=&v[index];
    }
    puts("FAT eviction: 32768-node first/middle/last hits avoid the second full scan; uncached target takes no lock or probes");
    unsigned eviction_cases=0;
    for(unsigned alias=0;alias<2;++alias)for(unsigned remount=0;remount<2;++remount){
        g_vfs_node_capacity=VFS_FAT32_MOUNT_LIMIT;g_vfs_next_free_node=VFS_FAT32_MOUNT_LIMIT;
        for(unsigned i=0;i<VFS_FAT32_MOUNT_LIMIT;++i){
            g_fat32_mounts[i].filesystem=(void*)(uintptr_t)(alias?i%4+1:i+1);
            snprintf(b[i].path,256,"%s%s",g_fat32_mounts[i].prefix,i?"/File":"File");
            b[i].fat32_node=true;v[i].fs_private=&b[i];v[i].drops=0;g_vfs_nodes[i]=&v[i];
        }
        b[11].fat32_node=false;strcat(b[15].path,"s");
        /* A concurrent recreation may already occupy another canonical slot.
         * It has the exact same path, but is a different inode/page cache. */
        strcpy(b[8].path,b[7].path);
        b[7].detached=true;b[8].detached=false;
        request_routes=0;remount_after_drop=remount;
        eviction_probes=0;lock_calls=0;
        detach_matching(&v[7]);
        assert(eviction_probes==8&&lock_calls==1);
        detach_matching(&v[7]);
        assert(eviction_probes==8+VFS_FAT32_MOUNT_LIMIT&&lock_calls==2);
        detach_matching(0);
        assert(eviction_probes==8+VFS_FAT32_MOUNT_LIMIT&&lock_calls==2);
        assert(request_routes==0&&g_vfs_next_free_node==7);
        for(unsigned i=0;i<VFS_FAT32_MOUNT_LIMIT;++i){
            assert(v[i].drops==(unsigned)(i==7));
            assert(g_vfs_nodes[i]==(i==7?0:&v[i]));
        }
        assert(g_fat32_mounts[7].filesystem==(void*)(uintptr_t)(remount?99:alias?4:8));
        assert(!lock_depth);
        ++eviction_cases;
    }
    printf("Actual delete eviction: %u distinct/alias/remount cases; same-name replacement retained, old inode released once outside lock; zero mount scans\n",eviction_cases);
    /* Recreation can also occur at the unlock/reference-release boundary,
     * reusing the slot just freed by removal. Never detach that new inode. */
    g_vfs_nodes[7]=&v[7];g_vfs_nodes[8]=0;
    v[7].drops=v[8].drops=0;
    replacement_after_drop=&v[8];
    detach_matching(&v[7]);
    assert(!replacement_after_drop&&g_vfs_nodes[7]==&v[8]);
    assert(v[7].drops==1&&v[8].drops==0&&!lock_depth);
    puts("Delete release-boundary recreation: reused slot retains the new inode");
    /* FAT rename must rekey case-insensitive and aliased canonical inodes,
     * but never a different filesystem or a detached old version. */
    g_vfs_node_capacity=7;
    g_fat32_mounts[0]=(vfs_fat32_mount_t){true,(void*)1,"/"};
    g_fat32_mounts[1]=(vfs_fat32_mount_t){true,(void*)1,"/disk1"};
    g_fat32_mounts[2]=(vfs_fat32_mount_t){true,(void*)2,"/disk2"};
    const char *initial_paths[]={"/Foo","/disk1/fOO/child","/disk2/Foo",
        "/Foobar","/foo","/Foo/ram","/other"};
    for(unsigned i=0;i<7;++i){
        strcpy(b[i].path,initial_paths[i]);b[i].fat32_node=i!=5;
        b[i].detached=i==4;v[i].fs_private=&b[i];g_vfs_nodes[i]=&v[i];
    }
    assert(vfs_rekey_cached_subtree_locked("/disk1/FOO",10,"/disk1/New",10,true)==K_OK);
    assert(!strcmp(b[0].path,"/disk1/New")&&!strcmp(b[1].path,"/disk1/New/child"));
    for(unsigned i=2;i<7;++i)assert(!strcmp(b[i].path,initial_paths[i]));
    assert(vfs_find_fat_node_locked("/disk1/new")==&v[0]);
    assert(vfs_find_fat_node_locked("/new")==&v[0]);
    assert(vfs_find_fat_node_locked("/disk1/new/child")==&v[1]);
    assert(!vfs_find_fat_node_locked("/disk1/foo"));
    puts("FAT rename rekey: case folding, mount aliases, new canonical lookup; other filesystems, near names, detached and non-FAT nodes retained");
    strcpy(b[0].path,"/Foo");strcpy(b[1].path,"/disk1/foo/child");
    char too_long[256];memset(too_long,'x',250);too_long[0]='/';too_long[250]=0;
    vfs_backend_node_t before[7];memcpy(before,b,sizeof(before));
    assert(vfs_rekey_cached_subtree_locked("/FOO",4,too_long,250,false)==K_ENAMETOOLONG);
    assert(!memcmp(before,b,sizeof(before)));
    assert(vfs_rekey_cached_subtree_locked("/FOO",4,too_long,250,true)==K_ENAMETOOLONG);
    assert(!memcmp(before,b,sizeof(before)));
    assert(vfs_rekey_cached_subtree_locked("/FOO",4,"/New",4,false)==K_OK);
    assert(!memcmp(before,b,sizeof(before)));
    puts("FAT rekey preflight: oversized aliased descendant rejects entire update; dry run leaves every path unchanged");
    printf("FAT comparison: %u independent old/new length, ASCII folding, byte mutation and invalid/mount cases; candidate relative-length scan removed\n",cases);
    free(g_vfs_nodes);free(v);free(b);
    puts("FAT lookup: 32768-node hit/miss normalizes request once; detached old inode exclusion, replacement inode, case, mounts and invalid paths passed");
}
'''
program = program.replace('MOUNT_LIMIT_VALUE', str(mount_limit) + 'U')
with tempfile.TemporaryDirectory(prefix='liteos-fat-lookup-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)
