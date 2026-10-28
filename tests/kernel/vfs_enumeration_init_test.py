"""真实枚举与逐项参考模型对比，并拒绝旧的 FAT 静默截断路径。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
header = (root / 'include/uapi/file.h').read_text()
layout = header[header.index('enum os_file_type {'):header.index('typedef struct os_file_enumerate {')]
equals = source[source.index('static bool vfs_path_range_equals('):source.index('static uint32_t vfs_free_node_slot_locked(')]
append = source[source.index('static size_t vfs_file_name_length('):source.index('typedef struct vfs_directory_collection')]
collection = source[source.index('typedef struct vfs_directory_collection'):source.index('static const char *vfs_mount_child_name(')]
enumerate = source[source.index('kstatus_t vfs_enumerate_kernel_batch('):source.index('kstatus_t vfs_enumerate_file(')]
batch_source, wrapper = enumerate.split('\nkstatus_t vfs_enumerate_kernel(', 1)
old_signature = '''kstatus_t vfs_enumerate_kernel_batch(const char *path, uint32_t index,
                                            os_file_info_t *out, uint32_t capacity,
                                            uint32_t *count)'''
assert old_signature in batch_source
previous_source = batch_source.replace(old_signature,
    'kstatus_t vfs_enumerate_kernel(const char *path, uint32_t index, os_file_info_t *out)')
previous_source = previous_source.replace('    if (count == 0) return K_EINVAL;\n    *count = 0U;\n', '')
previous_source = previous_source.replace(' || capacity == 0U', '')
batch_copy = '''    uint32_t copied = entry_count - index;
    if (copied > capacity) copied = capacity;
    memcpy(out, entries + index, (size_t)copied * sizeof(*out));
    *count = copied;'''
assert batch_copy in previous_source
previous_source = previous_source.replace(batch_copy, '    *out = entries[index];')
comment = '    os_file_info_t entries[VFS_DIRECTORY_ENTRY_LIMIT];'
assert comment in enumerate
bulk = '''        vfs_directory_collection_t collection = {entries, &entry_count, false};
        /* 由收集器判断容量耗尽，不能让底层在第 256 项静默停止，否则后续批次会误报 EOF。
         * 重名项不占新槽，因此必须继续访问到真正的末尾或首次追加失败。 */
        fat32_visit_directory(mount->filesystem, relative,
                                     UINT32_MAX,
                                     vfs_collect_fat_directory_entry, &collection);
        if (collection.failed) return K_ENOMEM;'''
single = '''        for (uint32_t fat_index = 0U; fat_index < UINT32_MAX; ++fat_index) {
            os_file_info_t entry = {0};
            if (!fat32_enumerate_path(mount->filesystem, relative, fat_index, &entry)) break;
            if (!vfs_append_entry(entries, &entry_count, &entry)) return K_ENOMEM;
        }'''
assert bulk in enumerate
truncated = batch_source.replace('vfs_enumerate_kernel_batch(', 'truncated_batch(')
truncated = truncated.replace('                                     UINT32_MAX,',
                              '                                     VFS_DIRECTORY_ENTRY_LIMIT,')
assert truncated != batch_source
old = previous_source.replace('vfs_enumerate_kernel(', 'previous_enumerate(').replace(comment, comment + '''
    for (uint32_t i = 0; i < VFS_DIRECTORY_ENTRY_LIMIT; ++i) {
        entries[i] = (os_file_info_t){0}; ++cleared;
    }''')
old = old.replace(bulk, single)
prefix_check = '''        size_t matched = 0U;
        while (matched < directory_length && backend->path[matched] != 0 &&
               backend->path[matched] == path[matched]) ++matched;
        if (matched != directory_length) continue;
'''
assert prefix_check in old
child_offset = '''        if (directory_length != 1U && backend->path[directory_length] != '/') continue;
        remainder_index = directory_length == 1U ? 1U : directory_length + 1U;
'''
previous_offset = '''        if (directory_length == 1U) {
            if (node_length <= 1U || backend->path[0] != '/') continue;
            remainder_index = 1U;
        } else {
            if (node_length <= directory_length ||
                backend->path[directory_length] != '/') continue;
            remainder_index = directory_length + 1U;
        }
'''
assert child_offset in old
old = old.replace('        size_t remainder_index;', '        size_t node_length;\n        size_t remainder_index;')
old = old.replace('        if (backend->path[directory_length] == 0)',
    '        node_length = vfs_string_length(backend->path);\n        if (node_length == directory_length)')
old = old.replace('remainder_index >= VFS_PATH_LIMIT || backend->path[remainder_index] == 0',
    'remainder_index >= node_length')
bounded_leaf = '''        while (name_length < OS_FILE_NAME_MAX &&
               remainder_index + name_length < VFS_PATH_LIMIT &&
               remainder[name_length] != '\\0' && remainder[name_length] != '/') ++name_length;
        if (name_length == 0U || name_length >= OS_FILE_NAME_MAX ||
            remainder_index + name_length >= VFS_PATH_LIMIT)'''
assert bounded_leaf in old
old = old.replace(bounded_leaf, '''        while (remainder[name_length] != '\\0' && remainder[name_length] != '/') ++name_length;
        if (name_length == 0U || name_length >= OS_FILE_NAME_MAX)''')
old = old.replace(child_offset, previous_offset)
old = old.replace(prefix_check, '').replace(
    'if (node_length == directory_length)',
    'if (vfs_path_range_equals(backend->path, node_length, path, directory_length))').replace(
    "backend->path[directory_length] != '/') continue;",
    "backend->path[directory_length] != '/' ||\n"
    "                !vfs_path_range_equals(backend->path, directory_length,\n"
    "                                       path, directory_length)) continue;")
# Poison is fixture-only; the real new path does not write unused entries.
enumerate = enumerate.replace('os_file_info_t entries[VFS_DIRECTORY_ENTRY_LIMIT];',
    'os_file_info_t entries[VFS_DIRECTORY_ENTRY_LIMIT]; memset(entries,0x75,sizeof(entries));')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sanitizer/asan_interface.h>
#include <sys/wait.h>
#include <unistd.h>
#define VFS_PATH_LIMIT 256U
#define VFS_DIRECTORY_ENTRY_LIMIT 256U
#define OS_FILE_NAME_MAX 64U
typedef int kstatus_t;
typedef bool BOOLEAN;
typedef void VOID;
enum {K_OK,K_EINVAL,K_ENOMEM,K_ENOENT};
''' + layout + r'''
typedef struct {char path[256];bool directory,fat32_node;} vfs_backend_node_t;
typedef struct {void *fs_private;uint32_t mode;uint64_t size;} vnode_t;
typedef struct {void *filesystem;} vfs_fat32_mount_t;
static vnode_t *g_vfs_nodes[300];static uint32_t g_vfs_node_capacity;
static vfs_fat32_mount_t mount={(void*)(uintptr_t)1};
static unsigned mount_mode,fat_count,child_count,cleared,locks,depth,stats,reads;
static bool child_failure, fat_duplicate_prefix;
static unsigned node_scans,avoided_scans;
static size_t vfs_string_length(const char *p){
 for(unsigned i=0;i<g_vfs_node_capacity;++i)
  if(g_vfs_nodes[i]&&g_vfs_nodes[i]->fs_private&&
     p==((vfs_backend_node_t*)g_vfs_nodes[i]->fs_private)->path){++node_scans;break;}
 size_t n=0;if(p)while(n<256&&p[n])++n;return n;
}
static void vfs_initialize(void){}
static void vfs_lock(void){assert(!depth);++depth;++locks;}
static void vfs_unlock(void){assert(depth==1);--depth;}
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *p,char *relative){
 assert(p&&relative);strcpy(relative,"dir");return mount_mode?&mount:0;
}
static bool fat32_stat_path(void *fs,const char *p,os_file_info_t *out){
 assert(fs==mount.filesystem&&p);++stats;
 *out=(os_file_info_t){.type=mount_mode==1?OS_FILE_TYPE_REGULAR:OS_FILE_TYPE_DIRECTORY};
 return mount_mode!=3;
}
static bool fat32_enumerate_path(void *fs,const char *p,unsigned index,os_file_info_t *out){
 assert(fs==mount.filesystem&&p);++reads;if(index>=fat_count)return false;
 *out=(os_file_info_t){.type=OS_FILE_TYPE_REGULAR,.mode=0644,.size=index+1};
 snprintf(out->name,sizeof(out->name),"node%03u",fat_duplicate_prefix&&index<300?0:index);return true;
}
''' + equals + append + collection + r'''
static void fat32_visit_directory(void *fs,const char *p,unsigned limit,
                                        BOOLEAN (*visitor)(VOID*,const os_file_info_t*),VOID *context){
 for(unsigned i=0;i<limit;++i){
  os_file_info_t entry={0};if(!fat32_enumerate_path(fs,p,i,&entry))break;
  if(!visitor(context,&entry))break;
 }
}
static bool vfs_append_mount_children(const char *p,size_t n,os_file_info_t *entries,
                                     uint32_t *count,bool *exists){
 assert(p&&n);if(child_failure)return false;
 for(unsigned i=0;i<child_count;++i){
  os_file_info_t e={.type=OS_FILE_TYPE_DIRECTORY,.mode=0040755};
  /* First child deliberately replaces a same-name FAT regular entry. */
  if(!i)strcpy(e.name,"node000");else snprintf(e.name,sizeof(e.name),"mount%03u",i);
  *exists=true;if(!vfs_append_entry(entries,count,&e))return false;
 }
 return true;
}
''' + old + truncated + enumerate + r'''
static unsigned cases,clear_routes;
static void compare(const char *p,unsigned index){
 os_file_info_t before,after;memset(&before,0x81,sizeof(before));after=before;
 cleared=locks=stats=reads=depth=node_scans=0;
 kstatus_t expected=previous_enumerate(p,index,&before);
 assert(!depth);unsigned count=cleared,l=locks,s=stats,r=reads,old_scans=node_scans;
 cleared=locks=stats=reads=node_scans=0;
 assert(vfs_enumerate_kernel(p,index,&after)==expected);
 assert(!depth&&!cleared&&locks==l&&stats==s&&reads==r);
 assert(node_scans<=old_scans);avoided_scans+=old_scans-node_scans;
 assert(!memcmp(&before,&after,sizeof(before)));++cases;
 if(count){assert(count==256);++clear_routes;}
}
int main(void){
 vfs_backend_node_t b[300];vnode_t v[300];
 memset(b,0,sizeof(b));memset(v,0,sizeof(v));
 for(unsigned i=0;i<300;++i){
  snprintf(b[i].path,sizeof(b[i].path),"/dir/node%03u",i);
  b[i].directory=(i%3)==0;v[i].fs_private=&b[i];
  v[i].mode=0600+i;v[i].size=100+i;g_vfs_nodes[i]=&v[i];
 }
 strcpy(b[2].path,"/dir/nested/file");strcpy(b[3].path,"/other/file");
 g_vfs_nodes[4]=0;v[5].fs_private=0;
 unsigned counts[]={0,1,16,255,256,257};
 unsigned indices[]={0,1,15,255,256,UINT32_MAX};
 const char *paths[]={0,"","relative","/","/dir","/dir/","/missing","/dir/node001"};
 for(unsigned mode=0;mode<4;++mode)for(unsigned n=0;n<6;++n)
 for(unsigned f=0;f<6;++f)for(unsigned c=0;c<4;++c){
  mount_mode=mode;g_vfs_node_capacity=counts[n];fat_count=counts[f];
  child_count=c==3?257:c;child_failure=false;
  for(unsigned p=0;p<8;++p)for(unsigned i=0;i<6;++i)compare(paths[p],indices[i]);
 }
 child_failure=true;compare("/dir",0);child_failure=false;
 /* Explicit directory, overlong leaf, and invalid bounded path exits. */
 mount_mode=fat_count=child_count=0;g_vfs_node_capacity=1;
 strcpy(b[0].path,"/dir");b[0].directory=true;compare("/dir",0);
 b[0].directory=false;compare("/dir",0);
 strcpy(b[0].path,"/dir/");memset(b[0].path+5,'x',64);b[0].path[69]=0;
 compare("/dir",0);
 char invalid[256];memset(invalid,'x',sizeof(invalid));invalid[0]='/';compare(invalid,0);
 /* Every directory length, early NUL and mismatch position; near-prefix
  * siblings must not become children, even when the request ends at 255. */
 for(unsigned length=1;length<256;++length){
  char request[256];memset(request,'a',length);request[0]='/';request[length]=0;
  strcpy(b[0].path,request);b[0].directory=true;compare(request,0);
  b[0].directory=false;compare(request,0);
  for(unsigned position=0;position<length;++position){
   char saved=b[0].path[position];b[0].path[position]='z';compare(request,0);
   b[0].path[position]=0;compare(request,0);b[0].path[position]=saved;
  }
  if(length<255){
   b[0].path[length]='x';b[0].path[length+1]=0;compare(request,0);
   b[0].path[length]='/';compare(request,0);
  }
 }
 char edge_request[256];memset(edge_request,'a',255);edge_request[0]='/';edge_request[255]=0;
 memcpy(b[0].path,edge_request,255);b[0].path[255]='/';compare(edge_request,0);
 b[0].path[255]='a';compare(edge_request,0);
 memset(b[0].path,'x',sizeof(b[0].path));compare("/missing",0);
 /* Poison descendant bytes: listing /dir needs only child and its slash,
  * not any byte of the deeper path. The previous full scan must fail. */
 strcpy(b[0].path,"/dir/child/deep/file");b[0].directory=false;
 __asan_poison_memory_region(b[0].path+11,245);
 os_file_info_t guarded={0};depth=0;
 assert(vfs_enumerate_kernel("/dir",0,&guarded)==K_OK&&!depth);
 assert(!strcmp(guarded.name,"child")&&guarded.type==OS_FILE_TYPE_DIRECTORY&&!guarded.size);
 pid_t pid=fork();assert(pid>=0);
 if(!pid){assert(freopen("/dev/null","w",stderr));previous_enumerate("/dir",0,&guarded);_exit(0);}
 int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==1);
 __asan_unpoison_memory_region(b[0].path+11,245);
 /* Reject a 64-byte leaf before touching its following bytes. */
 strcpy(b[0].path,"/dir/");memset(b[0].path+5,'x',64);
 __asan_poison_memory_region(b[0].path+69,187);
 memset(&guarded,0x81,sizeof(guarded));os_file_info_t untouched=guarded;depth=0;
 assert(vfs_enumerate_kernel("/dir",0,&guarded)==K_EINVAL&&!depth);
 assert(!memcmp(&guarded,&untouched,sizeof(guarded)));
 __asan_unpoison_memory_region(b[0].path+69,187);
 /* A short leaf that reaches the path-array end has no terminator. */
 memset(edge_request,'a',253);edge_request[0]='/';edge_request[253]=0;
 memcpy(b[0].path,edge_request,253);b[0].path[253]='/';b[0].path[254]='x';b[0].path[255]='y';
 assert(vfs_enumerate_kernel(edge_request,0,&guarded)==K_EINVAL&&!depth);
 assert(!memcmp(&guarded,&untouched,sizeof(guarded)));
 puts("Enumeration scan bounds: poisoned descendant tails untouched; old full scan fails; overlong and unterminated leaves reject without publishing or retaining lock");
 assert(vfs_enumerate_kernel("/dir",0,0)==K_EINVAL);
 /* Batch outputs equal successive legacy calls, including full output
  * canaries, final short batches and no publication on errors. */
 os_file_info_t expected_batch[260],actual_batch[260];unsigned batch_cases=0;
 g_vfs_node_capacity=0;child_count=0;mount_mode=2;child_failure=false;
 unsigned batch_capacities[]={0,1,2,16,255,256,260};
 unsigned batch_sizes[]={0,1,15,16,17,255,256};
 os_file_info_t legacy_entries[256];
 for(unsigned sample=0;sample<7;++sample){
 unsigned n=batch_sizes[sample];fat_count=n;
 for(unsigned i=0;i<n;++i)assert(previous_enumerate("/dir",i,&legacy_entries[i])==K_OK);
 os_file_info_t end={0};assert(previous_enumerate("/dir",n,&end)==K_ENOENT);
 for(unsigned start=0;start<=n+1;++start)
 for(unsigned c=0;c<7;++c){
  fat_count=n;unsigned capacity=batch_capacities[c],written=999;
  memset(expected_batch,0x81,sizeof(expected_batch));memcpy(actual_batch,expected_batch,sizeof(actual_batch));
  kstatus_t expected_status=capacity?K_OK:K_EINVAL;unsigned expected_count=0;
  if(capacity){
   if(start>=n)expected_status=K_ENOENT;
   else{expected_count=n-start;if(expected_count>capacity)expected_count=capacity;
    memcpy(expected_batch,legacy_entries+start,expected_count*sizeof(*expected_batch));}
  }
  assert(vfs_enumerate_kernel_batch("/dir",start,actual_batch,capacity,&written)==expected_status);
  assert(written==expected_count&&!memcmp(expected_batch,actual_batch,sizeof(actual_batch))&&!depth);++batch_cases;
 }
 }
 unsigned written=999;
 assert(vfs_enumerate_kernel_batch("/dir",0,actual_batch,16,0)==K_EINVAL);
 assert(vfs_enumerate_kernel_batch(0,0,actual_batch,16,&written)==K_EINVAL&&!written);
 assert(vfs_enumerate_kernel_batch("/dir",0,0,16,&written)==K_EINVAL&&!written);
 /* 负对照必须重现旧行为：第 257 项被省略，但首批仍成功、末批被误报 EOF。 */
 fat_count=257;mount_mode=2;g_vfs_node_capacity=child_count=0;
 memset(actual_batch,0x81,sizeof(actual_batch));
 memcpy(expected_batch,actual_batch,sizeof(actual_batch));
 assert(truncated_batch("/dir",0,actual_batch,16,&written)==K_OK&&written==16);
 assert(truncated_batch("/dir",256,actual_batch,16,&written)==K_ENOENT&&!written);
 memcpy(actual_batch,expected_batch,sizeof(actual_batch));
 assert(vfs_enumerate_kernel_batch("/dir",0,actual_batch,16,&written)==K_ENOMEM&&!written);
 assert(!memcmp(actual_batch,expected_batch,sizeof(actual_batch))&&!depth);
 assert(vfs_enumerate_kernel_batch("/dir",256,actual_batch,16,&written)==K_ENOMEM&&!written);
 assert(!memcmp(actual_batch,expected_batch,sizeof(actual_batch))&&!depth);
 puts("FAT overflow: 257th entry rejects incomplete snapshots; legacy truncation negative control reproduced");
 /* 超过 256 次回调但只有两个唯一名称，不应以回调次数为容量上限。 */
 fat_count=301;fat_duplicate_prefix=true;
 assert(truncated_batch("/dir",0,actual_batch,16,&written)==K_OK&&written==1);
 assert(vfs_enumerate_kernel_batch("/dir",0,actual_batch,16,&written)==K_OK&&written==2);
 assert(!strcmp(actual_batch[0].name,"node000")&&!strcmp(actual_batch[1].name,"node300"));
 fat_duplicate_prefix=false;
 /* 打开缓存的大小写别名不再作为额外目录项；内存后端仍正常参与合并。 */
 fat_count=1;mount_mode=2;g_vfs_node_capacity=2;child_count=0;
 memset(b,0,sizeof(b));v[0].fs_private=&b[0];v[1].fs_private=&b[1];
 g_vfs_nodes[0]=&v[0];g_vfs_nodes[1]=&v[1];
 strcpy(b[0].path,"/dir/NODE000");b[0].fat32_node=true;
 strcpy(b[1].path,"/dir/virtual");
 assert(vfs_enumerate_kernel_batch("/dir",0,actual_batch,16,&written)==K_OK&&written==2);
 assert(!strcmp(actual_batch[0].name,"node000")&&actual_batch[0].size==1);
 assert(!strcmp(actual_batch[1].name,"virtual"));
 fat_count=0;
 assert(vfs_enumerate_kernel_batch("/dir",0,actual_batch,16,&written)==K_OK&&written==1);
 assert(!strcmp(actual_batch[0].name,"virtual"));
 puts("FAT cached aliases: one authoritative disk name; removed/replaced cache entries never create ghost files; memory children retained");
 puts("FAT duplicate names: 301 callbacks retain the final unique child; legacy callback limit misses it");
 printf("VFS batch: %u exact legacy output/count/error/canary cases, capacities 0..260 and every start/EOF preserved\n",batch_cases);
 assert(avoided_scans);
 printf("Enumeration direct-child scan: %u complete length scans removed; descendant tails no longer scanned\n",avoided_scans);
 printf("Enumeration: %u exact old/new status/output/callback/lock cases with poisoned unused slots; %u valid requests avoid %zu-byte table clears; holes, dedup/replacement, limits and failure outputs preserved\n",cases,clear_routes,256*sizeof(os_file_info_t));
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-enumeration-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
