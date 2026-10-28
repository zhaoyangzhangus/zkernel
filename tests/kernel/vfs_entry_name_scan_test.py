"""Actual append: bound name scans, reuse length and retain table publication."""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
header = (root / 'include/uapi/file.h').read_text()
layout = header[header.index('enum os_file_type {'):header.index('typedef struct os_file_enumerate {')]
append = source[source.index('static bool vfs_append_entry('):source.index('typedef struct vfs_directory_collection')]
name_length = source[source.index('static size_t vfs_file_name_length('):source.index('static bool vfs_append_entry(')]
name_length = name_length.replace('    size_t length = 0U;',
                                '    if (name == incoming) ++scans; else ++candidate_scans;\n    size_t length = 0U;')
equals = source[source.index('static bool vfs_path_range_equals('):source.index('static uint32_t vfs_free_node_slot_locked(')]
old = append.replace('vfs_append_entry(', 'previous_append(').replace(
    '    size_t name_length = vfs_file_name_length(entry->name);\n'
    '    if (name_length >= OS_FILE_NAME_MAX) return false;\n', '')
old = old.replace('entries[index].name[name_length] != 0 ||\n'
                  '            !vfs_path_range_equals(entries[index].name, name_length,',
                  '!vfs_path_range_equals(entries[index].name,\n'
                  '                                   vfs_file_name_length(entries[index].name),')
old = old.replace('vfs_file_name_length(entries[index].name)',
                  'vfs_string_length(entries[index].name)')
old = old.replace('entry->name, name_length', 'entry->name, vfs_string_length(entry->name)')
assert old != append
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#define OS_FILE_NAME_MAX 64U
#define VFS_DIRECTORY_ENTRY_LIMIT 256U
''' + layout + r'''
static const char *incoming;static unsigned scans,candidate_scans;
static size_t vfs_string_length(const char *p){
 if(p==incoming)++scans;else ++candidate_scans;
 size_t n=0;while(n<256&&p[n])++n;return n;
}
''' + name_length + equals + old + append + r'''
typedef struct {uint64_t front[8];os_file_info_t entries[256];uint64_t back[8];} arena_t;
static arena_t arena;
int main(void){
 unsigned cases=0,avoided=0,avoided_candidates=0;
 unsigned lengths[]={1,7,8,31,63};
 for(unsigned count=0;count<=256;++count)for(unsigned length=0;length<5;++length)
 for(unsigned type=0;type<4;++type)for(unsigned match=0;match<3;++match){
  memset(&arena,0x71,sizeof(arena));
  for(unsigned i=0;i<256;++i){
   arena.entries[i]=(os_file_info_t){.type=OS_FILE_TYPE_REGULAR,.mode=0600+i,.size=i+1};
   snprintf(arena.entries[i].name,64,"item%03u",i);
  }
  os_file_info_t entry={.type=type,.mode=0040755,.size=0x8175};
  memset(entry.name,'z',lengths[length]);entry.name[lengths[length]]=0;
  if(count&&match){unsigned slot=match==1?0:count-1;strcpy(arena.entries[slot].name,entry.name);}
  arena_t initial=arena;unsigned before_count=count;
  incoming=entry.name;scans=candidate_scans=0;
  bool expected=previous_append(arena.entries,&before_count,&entry);unsigned old_scans=scans;
  unsigned old_candidates=candidate_scans;
  arena_t expected_arena=arena;
  arena=initial;unsigned after_count=count;scans=candidate_scans=0;
  assert(vfs_append_entry(arena.entries,&after_count,&entry)==expected);
  assert(after_count==before_count&&!memcmp(&arena,&expected_arena,sizeof(arena)));
  assert(scans==1);
  assert(!candidate_scans);avoided_candidates+=old_candidates;
  if(count){assert(old_scans>=scans);avoided+=old_scans-scans;}
  else assert(!old_scans);++cases;
 }
 /* Equal prefixes with different lengths must never merge. Bytes after a
  * shorter candidate's NUL need not be zero for the comparison to be safe. */
 for(unsigned candidate_length=1;candidate_length<64;++candidate_length)
 for(unsigned incoming_length=1;incoming_length<64;++incoming_length){
  memset(&arena,0x71,sizeof(arena));
  arena.entries[0].type=OS_FILE_TYPE_REGULAR;
  memset(arena.entries[0].name,'x',candidate_length);
  arena.entries[0].name[candidate_length]=0;
  os_file_info_t entry={.type=OS_FILE_TYPE_DIRECTORY,.mode=0755};
  memset(entry.name,'x',incoming_length);entry.name[incoming_length]=0;
  arena_t initial=arena;unsigned before=1,after=1;
  incoming=entry.name;
  bool expected=previous_append(arena.entries,&before,&entry);
  arena_t result=arena;arena=initial;candidate_scans=0;
  assert(vfs_append_entry(arena.entries,&after,&entry)==expected);
  assert(before==after&&!memcmp(&arena,&result,sizeof(arena))&&!candidate_scans);
  assert(after==(candidate_length==incoming_length?1U:2U));++cases;
 }
 /* Input may alias an existing table entry; publication must stay identical. */
 for(unsigned slot=0;slot<256;++slot){
  for(unsigned i=0;i<256;++i){
   arena.entries[i]=(os_file_info_t){.type=OS_FILE_TYPE_DIRECTORY,.mode=0755,.size=i};
   snprintf(arena.entries[i].name,64,"item%03u",i);
  }
  arena_t initial=arena;unsigned before=256,after=256;
  incoming=arena.entries[slot].name;
  bool expected=previous_append(arena.entries,&before,&arena.entries[slot]);
  arena_t result=arena;arena=initial;
  assert(vfs_append_entry(arena.entries,&after,&arena.entries[slot])==expected);
  assert(before==after&&!memcmp(&arena,&result,sizeof(arena)));++cases;
 }
 unsigned count=0;os_file_info_t empty={0};scans=0;
 assert(!vfs_append_entry(0,&count,&empty));
 assert(!vfs_append_entry(arena.entries,0,&empty));
 assert(!vfs_append_entry(arena.entries,&count,0));
 assert(!vfs_append_entry(arena.entries,&count,&empty)&&!count&&!scans);
 /* An incoming name ends exactly at a protected page. The old 256-byte
  * scanner faults; the bounded production helper rejects without mutation. */
 long page=sysconf(_SC_PAGESIZE);assert(page>=(long)sizeof(os_file_info_t));
 char *memory=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(memory!=MAP_FAILED);
 assert(!mprotect(memory+page,(size_t)page,PROT_NONE));
 os_file_info_t *edge=(void*)(memory+page-sizeof(*edge));
 *edge=(os_file_info_t){.type=OS_FILE_TYPE_DIRECTORY};memset(edge->name,'x',64);
 incoming=edge->name;arena_t saved=arena;
 for(unsigned i=0;i<3;++i){
  unsigned counts[]={0,1,256};count=counts[i];scans=0;
  assert(!vfs_append_entry(arena.entries,&count,edge)&&count==counts[i]&&scans==1);
  assert(!memcmp(&arena,&saved,sizeof(arena)));
 }
 pid_t pid=fork();assert(pid>=0);
 if(!pid){signal(SIGSEGV,SIG_DFL);count=1;previous_append(arena.entries,&count,edge);_exit(0);}
 int status;assert(waitpid(pid,&status,0)==pid&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV);
 assert(!munmap(memory,(size_t)page*2));
 /* Guard the last candidate in a full table as well. */
 size_t table_bytes=256*sizeof(os_file_info_t);
 size_t pages=(table_bytes+(size_t)page-1)/(size_t)page;
 memory=mmap(0,(pages+1)*(size_t)page,PROT_READ|PROT_WRITE,
             MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(memory!=MAP_FAILED);
 assert(!mprotect(memory+pages*(size_t)page,(size_t)page,PROT_NONE));
 os_file_info_t *table=(void*)(memory+pages*(size_t)page-table_bytes);
 for(unsigned i=0;i<256;++i){table[i]=(os_file_info_t){0};strcpy(table[i].name,"other");}
 memset(table[255].name,'x',64);os_file_info_t valid={0};strcpy(valid.name,"new");
 os_file_info_t snapshot[256];memcpy(snapshot,table,table_bytes);count=256;
 assert(!vfs_append_entry(table,&count,&valid)&&count==256&&!memcmp(snapshot,table,table_bytes));
 pid=fork();assert(pid>=0);
 if(!pid){signal(SIGSEGV,SIG_DFL);count=256;previous_append(table,&count,&valid);_exit(0);}
 assert(waitpid(pid,&status,0)==pid&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV);
 assert(!munmap(memory,(pages+1)*(size_t)page));
 puts("Name bounds: incoming/candidate 64-byte protected-page boundaries passed; two old unbounded scans reproduce SIGSEGV; malformed names never publish or overwrite");
 printf("Entry name scans: %u exact old/new table/count/canary cases, %u repeated incoming and %u candidate length scans removed; empty/full, first/last duplicates, directory overwrite, 63-byte names and input aliases preserved\n",cases,avoided,avoided_candidates);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-entry-name-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
    boundary = 'entries[index].name[name_length] != 0 ||'
    assert boundary in code
    negative = code.replace(boundary, 'false ||')
    subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-x', 'c', '-', '-o', binary],
                   input=negative, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, text=True,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
    print('Negative control rejected: prefix-only candidate equality')
