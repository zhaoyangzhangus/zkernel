"""文件缺页读取必须按后端锁内的最新 EOF 处理并发截断。"""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

from vfs_demand_page_lifecycle_test import prefix, body, suffix

old_lock = 'static void vfs_backend_lock(vfs_backend_node_t *b){assert(b&&lock_depth==1);lock_depth=2;}'
new_lock = r'''
static bool resize_on_lock;
static uint64_t resized_size;
static void vfs_backend_lock(vfs_backend_node_t *b){
 assert(b&&lock_depth==1);lock_depth=2;
 if(resize_on_lock){b->size=resized_size;resize_on_lock=false;}
}
'''
assert old_lock in prefix
prefix = prefix.replace(old_lock, new_lock)
old_read = ''' (void)context;assert(lock_depth==2 && n<=16U*PAGE_SIZE);++reads;
 for(size_t i=0;i<n;++i)((unsigned char*)out)[i]=(unsigned char)((offset+i)*19+7);'''
new_read = ''' vfs_backend_node_t *b=context;assert(b&&lock_depth==2 && n<=16U*PAGE_SIZE);++reads;
 if(offset>=b->size)n=0;else if(n>b->size-offset)n=(size_t)(b->size-offset);
 for(size_t i=0;i<n;++i)((unsigned char*)out)[i]=(unsigned char)((offset+i)*19+7);'''
assert old_read in prefix
prefix = prefix.replace(old_read, new_read)
setup = suffix[:suffix.index('int main(void)')]
cases = r'''
int main(void){
 const uint64_t old_sizes[]={4096,8192,16384,65536};
 const uint64_t new_sizes[]={0,1,2048,4095,4096,8191,8192,16383,32768,65535,131072};
 unsigned cases=0;
 for(unsigned o=0;o<sizeof(old_sizes)/sizeof(*old_sizes);++o)
 for(unsigned n=0;n<sizeof(new_sizes)/sizeof(*new_sizes);++n)
 for(unsigned index=0;index<4;++index){
  if(new_sizes[n]==old_sizes[o] || (uint64_t)index*PAGE_SIZE>=old_sizes[o])continue;
  reset();vfs_backend_node_t b={.size=old_sizes[o],.read=read_data};b.context=&b;
  vnode_t v={.object={KOBJECT_TYPE_VNODE},.fs_private=&b,.size=b.size};
  page_t sentinel,*out=&sentinel;
  resize_on_lock=true;resized_size=new_sizes[n];
  assert(vfs_file_page_get_batch(&v,index,16,&out)==K_OK);
  assert(!resize_on_lock && out!=&sentinel && b.size==new_sizes[n]);
  vfs_page_cache_t *cache=v.page_cache;
  assert(cache && cache->entry_count && !lock_depth);
  for(vfs_cached_page_t *e=cache->head;e;e=e->next)
   check_bytes(e->page,e->index,b.size);
  vfs_page_cache_destroy(&v);reset();++cases;
 }
 printf("并发缩小或扩展：%u 个完整页、部分页和 EOF 以后的预读场景通过\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-page-truncate-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-x', 'c', '-', '-o', binary]
    def run(production):
        subprocess.run(command, input=prefix+production+setup+cases, text=True, check=True)
        return subprocess.run([binary], capture_output=True, text=True, timeout=20,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    result = run(body)
    assert result.returncode == 0, result.stdout+result.stderr
    print(result.stdout.strip())
    protected = '''    vfs_backend_lock(backend);
    uint64_t read_length = backend->size > file_offset ? backend->size - file_offset : 0U;
    if (read_length > capacity) read_length = capacity;
    uint64_t bytes = 0U;
    status = K_OK;
'''
    stale = '''    uint64_t read_length = backend->size > file_offset ? backend->size - file_offset : 0U;
    if (read_length > capacity) read_length = capacity;
    uint64_t bytes = 0U;
    status = K_OK;
    vfs_backend_lock(backend);
'''
    assert protected in body
    result = run(body.replace(protected, stale))
    assert result.returncode == -signal.SIGABRT and 'vfs_file_page_get_batch' in result.stderr
    print('负对照通过：锁外读取旧 EOF 会导致缺页失败')
