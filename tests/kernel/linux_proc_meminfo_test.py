"""Production meminfo builder and append helpers, without guest mutation."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()

def function(marker):
    start = source.index(marker)
    opening = source.index('{', start)
    while ';' in source[start:opening]:
        start = source.index(marker, start + len(marker))
        opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

helpers = '\n'.join(function(marker) for marker in (
    'static char *linux_proc_data(', 'static size_t linux_proc_capacity(',
    'static bool linux_proc_append_bytes(', 'static bool linux_proc_append_text(',
    'static bool linux_proc_append_char(',
    'static size_t linux_proc_u64_text(', 'static bool linux_proc_append_u64(',
    'static bool linux_proc_append_cpu_stat_line(',
    'static bool linux_proc_build_meminfo('))
cpu_cases = source[source.index('    case LINUX_PROC_NODE_CPUINFO:'):source.index('    case LINUX_PROC_NODE_MAPS:')]
cpu_cases += source[source.index('    case LINUX_PROC_NODE_STAT:'):source.index('    case LINUX_PROC_NODE_CPU_ONLINE:')]
inline_define = next(line for line in source.splitlines() if line.startswith('#define LINUX_PROC_DATA_MAX '))
shape_start = source.index('struct linux_proc_file {')
shape = source[shape_start:source.index('\n};', shape_start) + 3]
node_enum = source[source.index('enum linux_proc_node {'):source.index('\n};', source.index('enum linux_proc_node {')) + 3]
integration = '\n'.join(function(marker) for marker in (
    'static void linux_proc_file_put(', 'static bool linux_proc_node_is_directory(',
    'static linux_proc_file_t *linux_proc_file_create(',
    'static bool linux_proc_parse_cpu_leaf(', 'static bool linux_proc_parse_cache_leaf(',
    'static kstatus_t linux_proc_open_path(',
    'static bool linux_proc_set_dirent_name(',
    'static uint32_t linux_proc_directory_count(',
    'static bool linux_proc_directory_entry(', 'static int64_t linux_proc_read_file('))
assert 'linux_exact_path(path, "/proc/meminfo")' in source
assert 'case LINUX_PROC_NODE_MEMINFO:\n        return linux_proc_build_meminfo(file);' in source
assert 'case LINUX_PROC_NODE_PROC_DIR: return 6U;' in source
directory = function('static bool linux_proc_directory_entry(')
assert 'if (index == 5U)' in directory and '"meminfo"' in directory

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#define PAGE_SIZE 4096U
#define K_OK 0
#define __user
#define K_EINVAL -22
#define K_ENOENT -2
#define K_ENOMEM -12
#define K_EBADF -9
#define K_EISDIR -21
#define LINUX_EFAULT -14
#define MAX_CPUS 256U
#define OS_LINUX_FD_LIMIT 16U
#define LINUX_FD_UNUSED 0
#define OS_DEBUG_SERIAL 0
@INLINE_DEFINE@
typedef int kstatus_t;
typedef struct { unsigned held; } spinlock_t;
static void spinlock_init(spinlock_t *l){l->held=0;}
static void spinlock_lock(spinlock_t *l){assert(!l->held);l->held=1;}
static void spinlock_unlock(spinlock_t *l){assert(l->held);l->held=0;}
typedef struct {
 uint64_t pid;
 struct {spinlock_t lock;struct {unsigned kind;} entries[OS_LINUX_FD_LIMIT];} linux_fds;
} process_t;
typedef struct {
 uint32_t before,length; char *extended_data; uint32_t extended_capacity;
 char data[LINUX_PROC_DATA_MAX]; uint32_t after;
 atomic_uint refs; spinlock_t lock;
 uint64_t inode,position,process_id;
 uint32_t cpu_index,cpu_cache_index;
 uint8_t node;uint16_t fd_count,fd_entries[OS_LINUX_FD_LIMIT];
} linux_proc_file_t;
static uint64_t total_pages,free_pages;
static int status;
static unsigned calls;
static int page_memory_get_stats(uint64_t *total,uint64_t *free) {
 ++calls;*total=total_pages;*free=free_pages;return status;
}
static void linux_copy(void *d,const void *s,size_t n){memcpy(d,s,n);}
''' + node_enum + helpers + r'''
static atomic_uint_fast64_t g_linux_proc_next_inode;
static unsigned live_allocations;
static bool allocation_fails,copy_fails,extension_fails;
static void *kzalloc(size_t n,unsigned flags){assert(!flags);if(allocation_fails)return NULL;void *p=calloc(1,n);assert(p);++live_allocations;return p;}
static void *kmalloc(size_t n,unsigned flags){assert(!flags);if(extension_fails)return NULL;void *p=malloc(n);assert(p);++live_allocations;return p;}
static void kfree(void *p){if(p){assert(live_allocations);--live_allocations;free(p);}}
static bool linux_exact_path(const char *a,const char *b){return !strcmp(a,b);}
static bool nv_frontend_driver_ready(void){return false;}
static unsigned cpu_count=2;
static uint32_t linux_proc_cpu_count(void){return cpu_count;}
static bool linux_user_pointer(uint64_t p,size_t n){return p && n<4096;}
static int copy_to_user(void *d,const void *s,size_t n){if(copy_fails)return -1;memcpy(d,s,n);return K_OK;}
static bool linux_proc_build_data(linux_proc_file_t *file,process_t *p){
 (void)p;file->length=0;
 switch(file->node){
 case LINUX_PROC_NODE_MEMINFO:return linux_proc_build_meminfo(file);
 @CPU_CASES@
 default:assert(0);return false;
 }
}
''' + integration + r'''
static void test_file_path(void) {
 process_t process={.pid=123};
 for(unsigned chunk=1;chunk<=128;++chunk){
  total_pages=1024;free_pages=100;status=0;
  linux_proc_file_t *f=NULL;
  assert(linux_proc_open_path(&process,"/proc/meminfo",&f)==K_OK);
  assert(f && f->node==LINUX_PROC_NODE_MEMINFO && f->process_id==123);
  char expected[256],output[256];size_t n=f->length;
  memcpy(expected,f->data,n);size_t offset=0;
  free_pages=0; /* Existing descriptor remains a coherent open-time snapshot. */
  while(offset<n){int64_t got=linux_proc_read_file(f,(uintptr_t)(output+offset),chunk);assert(got>0);offset+=(size_t)got;}
  assert(offset==n && !memcmp(output,expected,n));
  assert(linux_proc_read_file(f,(uintptr_t)output,1)==0);
  f->position=0;copy_fails=true;
  assert(linux_proc_read_file(f,(uintptr_t)output,1)==LINUX_EFAULT && !f->position);
  copy_fails=false;
  assert(linux_proc_read_file(f,0,1)==LINUX_EFAULT && !f->position);
  assert(linux_proc_read_file(f,0,0)==0 && !f->position);
  linux_proc_file_t *fresh=NULL;
  assert(linux_proc_open_path(&process,"/proc/meminfo",&fresh)==K_OK);
  assert(fresh->inode!=f->inode && fresh->length!=0);
  assert(memcmp(fresh->data,f->data,n));
  linux_proc_file_put(f);linux_proc_file_put(fresh);assert(!live_allocations);
 }
 linux_proc_file_t *f=NULL;
 assert(linux_proc_open_path(&process,"/proc/meminfo.extra",&f)==K_ENOENT);
 assert(linux_proc_open_path(NULL,"/proc/meminfo",&f)==K_EINVAL);
 allocation_fails=true;
 assert(linux_proc_open_path(&process,"/proc/meminfo",&f)==K_ENOMEM && !f);
 allocation_fails=false;status=-1;
 assert(linux_proc_open_path(&process,"/proc/meminfo",&f)==K_ENOMEM && !f && !live_allocations);
 status=0;assert(linux_proc_open_path(&process,"/proc",&f)==K_OK);
 assert(linux_proc_directory_count(f)==6);
 const char *names[]={".","..","self","cpuinfo","stat","meminfo"};
 for(unsigned i=0;i<6;++i){char name[32];uint8_t type=0;uint64_t inode=0;
  assert(linux_proc_directory_entry(f,i,name,sizeof(name),&type,&inode));
  assert(!strcmp(name,names[i]) && type==(i<3?4:8));
  assert(!linux_proc_directory_entry(f,i,name,1,&type,&inode));
 }
 char name[32];uint8_t type;uint64_t inode;
 assert(!linux_proc_directory_entry(f,6,name,sizeof(name),&type,&inode));
 assert(linux_proc_read_file(f,(uintptr_t)name,1)==K_EISDIR);
 assert(linux_proc_read_file(NULL,(uintptr_t)name,1)==K_EBADF);
 linux_proc_file_put(f);assert(!live_allocations);
 puts("meminfo integration: exact path, 128 chunk sizes, EOF, copy faults, fresh snapshots, allocation cleanup and proc enumeration PASS");
}
static void test_cpu_snapshots(void){
 process_t process={.pid=123};
 for(cpu_count=1;cpu_count<=MAX_CPUS;++cpu_count){
  for(unsigned stat=0;stat<2;++stat){
   linux_proc_file_t *f=NULL;
   assert(linux_proc_open_path(&process,stat?"/proc/stat":"/proc/cpuinfo",&f)==K_OK);
   char expected[20000];size_t length=0;
   if(stat)length+=(size_t)snprintf(expected+length,sizeof(expected)-length,"cpu  0 0 0 0 0 0 0 0 0 0\n");
   for(unsigned cpu=0;cpu<cpu_count;++cpu){
    if(stat)length+=(size_t)snprintf(expected+length,sizeof(expected)-length,"cpu%u 0 0 0 0 0 0 0 0 0 0\n",cpu);
    else length+=(size_t)snprintf(expected+length,sizeof(expected)-length,"processor\t: %u\nmodel name\t: System x86_64\n",cpu);
   }
   assert(length<sizeof(expected) && f->length==length);
   assert(!memcmp(linux_proc_data(f),expected,length));
   bool extended=(cpu_count+1)*64>sizeof(f->data);
   assert((f->extended_data!=NULL)==extended);
   linux_proc_file_put(f);assert(!live_allocations);
  }
 }
 cpu_count=MAX_CPUS;extension_fails=true;linux_proc_file_t *f=NULL;
 assert(linux_proc_open_path(&process,"/proc/cpuinfo",&f)==K_ENOMEM && !f && !live_allocations);
 assert(linux_proc_open_path(&process,"/proc/stat",&f)==K_ENOMEM && !f && !live_allocations);
 extension_fails=false;cpu_count=2;
 puts("proc inline buffer: complete cpuinfo/stat for 1..256 CPUs, inline/extended boundary and allocation-failure cleanup PASS");
}
int main(void) {
 unsigned cases=0;
 for(unsigned n=0;n<=4096;++n) {
  total_pages=1046867;free_pages=n*251ULL;status=0;calls=0;
  linux_proc_file_t f={.before=123,.after=456};
  memset(f.data,0x5a,sizeof(f.data));
  assert(linux_proc_build_meminfo(&f) && calls==1);
  char expected[256];
  int length=snprintf(expected,sizeof(expected),
   "MemTotal:\t%llu kB\nMemFree:\t%llu kB\nMemAvailable:\t%llu kB\n"
   "SwapTotal:\t0 kB\nSwapFree:\t0 kB\n",
   (unsigned long long)total_pages*4,(unsigned long long)free_pages*4,
   (unsigned long long)free_pages*4);
  assert(length>0 && f.length==(unsigned)length);
  assert(!memcmp(f.data,expected,f.length));
  for(size_t i=f.length;i<sizeof(f.data);++i)assert(f.data[i]==0x5a);
  assert(f.before==123 && f.after==456);++cases;
 }
 for(unsigned capacity=0;capacity<160;++capacity) {
  char storage[162];memset(storage,0x5a,sizeof(storage));
  linux_proc_file_t f={.extended_data=storage+1,.extended_capacity=capacity};
  total_pages=1024;free_pages=0;
  char expected[256];int length=snprintf(expected,sizeof(expected),
   "MemTotal:\t4096 kB\nMemFree:\t0 kB\nMemAvailable:\t0 kB\n"
   "SwapTotal:\t0 kB\nSwapFree:\t0 kB\n");
  assert(linux_proc_build_meminfo(&f)==(capacity>=(unsigned)length));
  assert(f.length<=capacity && storage[0]==0x5a);
  assert(!memcmp(storage+1,expected,f.length));
  for(unsigned i=capacity+1;i<sizeof(storage);++i)assert(storage[i]==0x5a);
  ++cases;
 }
 for(unsigned invalid=0;invalid<3;++invalid) {
  linux_proc_file_t f={0};status=invalid==0?-1:0;
  total_pages=invalid==2?UINT64_MAX:8;free_pages=invalid==1?9:0;
  calls=0;assert(!linux_proc_build_meminfo(&f));assert(calls==1 && !f.length);
 }
 printf("meminfo: %u changing free/zero-pressure/capacity cases, exact kB values, guards and invalid stats PASS\n",cases);
 test_file_path();
 test_cpu_snapshots();
}
'''
program = program.replace('@INLINE_DEFINE@', inline_define).replace('@CPU_CASES@', cpu_cases)
with tempfile.TemporaryDirectory(prefix='proc-meminfo-') as directory:
    binary = str(Path(directory) / 'test')
    def run(text):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(root / 'include'),
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-x', 'c', '-', '-o', binary],
                       input=text, text=True, check=True)
        return subprocess.run([binary], capture_output=True, text=True, timeout=15,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    result = run(program)
    assert result.returncode == 0, result.stderr
    print(result.stdout, end='')
    negative = program.replace('const uint64_t pages[] = {total, free, free};',
                               'const uint64_t pages[] = {total, free, total};')
    assert negative != program and run(negative).returncode != 0
    negative = program.replace('pages[index] * (PAGE_SIZE / 1024U)', 'pages[index]')
    assert negative != program and run(negative).returncode != 0
    negative = program.replace('linux_exact_path(path, "/proc/meminfo")',
                               'linux_exact_path(path, "/proc/meminfX")')
    assert negative != program and run(negative).returncode != 0
    negative = program.replace('case LINUX_PROC_NODE_PROC_DIR: return 6U;',
                               'case LINUX_PROC_NODE_PROC_DIR: return 5U;')
    assert negative != program and run(negative).returncode != 0
    negative = program.replace('if (status == K_OK) file->position += count;',
                               'file->position += count;')
    assert negative != program and run(negative).returncode != 0
    negative = program.replace('if (capacity > sizeof(file->data)) {', 'if (false) {')
    assert negative != program and run(negative).returncode != 0
    geometry = '#include <kernel/linux_abi.h>\n#include <stdio.h>\n' + inline_define + '\n' + shape + r'''
_Static_assert(sizeof(((struct linux_proc_file *)0)->data)==4096U,"small proc snapshot budget");
_Static_assert(sizeof(struct linux_proc_file)<8192U-128U,"fit 8 KiB including allocator overhead");
int main(void){printf("Production proc snapshot struct: %zu bytes, 4 KiB inline data, 1024 FD slots PASS\n",sizeof(struct linux_proc_file));}
'''
    result = run(geometry)
    assert result.returncode == 0, result.stderr
    print(result.stdout, end='')
print('Negative controls rejected: MemAvailable reports total RAM, page counts mislabeled as kB')
print('Negative controls rejected: missing open path, hidden directory entry, read fault advances offset')
print('Negative control rejected: fixed inline capacity truncates multi-CPU snapshots')
