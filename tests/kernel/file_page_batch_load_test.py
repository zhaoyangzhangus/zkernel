"""完整页缓存填充与读取：批量 I/O、失败原子性、邻页保护和 EOF 字节对照。"""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

from vfs_demand_page_lifecycle_test import prefix, body, suffix, function

# 此测试关注缓存页生命周期；共享偏移锁由独立并发测试覆盖。
prefix += "\n#define vfs_file_position_lock(file) ((void)(file), K_OK)\n#define vfs_file_position_unlock(file) ((void)(file))\n"

setup = suffix[:suffix.index('int main(void)')]
production = body + function('kstatus_t vfs_read_cached_kernel(')
cases = r'''
static unsigned floor_power(unsigned n) {
 unsigned power=1;while(power<=n/2)power*=2;return power;
}
static void check_cache(vnode_t *v,uint64_t size,unsigned expected) {
 vfs_page_cache_t *cache=v->page_cache;
 assert(cache && cache->entry_count==expected);
 unsigned entries=0;
 for(vfs_cached_page_t *e=cache->head;e;e=e->next) {
  assert(vfs_page_cache_lookup(cache,e->index)==e && !e->dirty);
  check_bytes(e->page,e->index,size);++entries;
 }
 assert(entries==expected);
}
int main(void) {
 unsigned cases=0,eof_cases=0;
 const uint64_t sizes[]={0,1,4095,4096,4097,8191,8192,8193,
                         65535,65536,65537,131072,UINT64_MAX};
 const unsigned limits[]={0,1,2,3,4,7,8,15,16,17,32,UINT32_MAX};
 const unsigned blockers[]={0,1,2,3,7,8,15,16};
 for(unsigned kind=0;kind<3;++kind)
 for(unsigned s=0;s<sizeof(sizes)/sizeof(*sizes);++s)
 for(unsigned l=0;l<sizeof(limits)/sizeof(*limits);++l)
 for(unsigned c=0;c<sizeof(blockers)/sizeof(*blockers);++c) {
  reset();vfs_backend_node_t b={.size=sizes[s],.detached=kind==2,
   .read=vfs_memory_backend_read};
  vnode_t v={.object={KOBJECT_TYPE_VNODE},.fs_private=&b,.size=b.size};
  vfs_cached_page_t *resident=0;page_t *saved=0;
  if(blockers[c]){
   assert(vfs_file_page_get(&v,blockers[c],&saved)==K_OK);
   resident=vfs_page_cache_lookup(v.page_cache,blockers[c]);
   resident->dirty=true;
   memset(phys_to_direct(page_to_phys(saved)),0xd3,PAGE_SIZE);
  }
  b.read=kind==0?vfs_memory_backend_read:read_data;
  unsigned eligible=1;
  unsigned maximum=limits[l]?limits[l]:2U;
  if(kind==1)while(eligible<maximum && eligible<16 &&
    (uint64_t)eligible*PAGE_SIZE<b.size && eligible!=blockers[c])++eligible;
  unsigned expected=floor_power(eligible);
  unsigned previous_heap=heap_calls;
  reads=physical_queries=0;page_t *out=0;
  assert(vfs_file_page_get_batch(&v,0,limits[l],&out)==K_OK);
  assert(physical_queries==1 && reads==(b.size!=0));
  assert(heap_calls-previous_heap==expected+(resident==0));
  vfs_page_cache_t *cache=v.page_cache;
  assert(cache->entry_count==expected+(resident!=0));
  for(unsigned i=0;i<expected;++i){
   vfs_cached_page_t *entry=vfs_page_cache_lookup(cache,i);
   assert(entry && !entry->dirty && entry->page->u.file.mapping==&v);
   check_bytes(entry->page,i,b.size);
  }
  if(resident){
   assert(vfs_page_cache_lookup(cache,blockers[c])==resident &&
          resident->page==saved && resident->dirty);
   unsigned char *p=phys_to_direct(page_to_phys(saved));
   for(unsigned i=0;i<PAGE_SIZE;++i)assert(p[i]==0xd3);
  }
  vfs_page_cache_destroy(&v);reset();++cases;
 }
 /* 逐字节遍历两页内全部 EOF 边界，沿用完整真实函数而不是截取清零片段。 */
 for(unsigned kind=0;kind<3;++kind)for(unsigned end=0;end<=8193;++end){
  reset();vfs_backend_node_t b={.size=end,.detached=kind==2,
   .read=kind==0?vfs_memory_backend_read:read_data};
  vnode_t v={.object={KOBJECT_TYPE_VNODE},.fs_private=&b,.size=b.size};page_t *out=0;
  assert(vfs_file_page_get(&v,0,&out)==K_OK);
  unsigned expected=kind==1 && end>PAGE_SIZE?2:1;
  check_cache(&v,end,expected);
  vfs_page_cache_destroy(&v);reset();++eof_cases;
 }
 /* 所有元数据失败位置与所有物理阶数失败组合。批次必须逐级退到可用阶数。 */
 for(unsigned heap=0;heap<=18;++heap)for(unsigned mask=0;mask<32;++mask){
  reset();vfs_backend_node_t b={.size=65536,.read=read_data};
  vnode_t v={.object={KOBJECT_TYPE_VNODE},.fs_private=&b,.size=b.size};
  fail_heap_call=heap;fail_order=mask;
  unsigned available=heap==1||heap==2?0:heap>=3&&heap<=17?heap-2:16;
  unsigned expected=available?floor_power(available):0;
  unsigned order=0;for(unsigned p=expected;p>1;p/=2)++order;
  while(expected && (mask&(1U<<order))){expected/=2;if(order)--order;}
  page_t sentinel,*out=&sentinel;
  assert(vfs_file_page_get_batch(&v,0,16,&out)==(expected?K_OK:K_ENOMEM));
  if(expected){assert(out!=&sentinel);check_cache(&v,b.size,expected);}
  else assert(out==&sentinel && !live_pages);
  vfs_page_cache_destroy(&v);reset();++cases;
 }
 /* 每一个复合尾页都必须在发布前验证；错误不得修改已有的碰撞桶和链表。 */
 for(unsigned mode=0;mode<4;++mode)for(unsigned bad=0;bad<18;++bad){
  reset();vfs_backend_node_t b={.size=300U*PAGE_SIZE,.read=vfs_memory_backend_read};
  vnode_t v={.object={KOBJECT_TYPE_VNODE},.fs_private=&b,.size=b.size};page_t *saved=0;
  assert(vfs_file_page_get(&v,257,&saved)==K_OK);
  vfs_page_cache_t *cache=v.page_cache,before=*cache;
  unsigned old_heap=live_heap,old_pages=live_pages;
  b.read=read_data;read_mode=mode;
  bad_tail=bad==0?0:bad<=2?bad:3+1+(bad-2);
  page_t sentinel,*out=&sentinel;
  bool fails=mode!=0 || bad!=0;
  int status=vfs_file_page_get_batch(&v,0,16,&out);
  assert(status==(!fails?K_OK:mode==3?K_ENOMEM:K_EIO));
  if(fails){
   assert(out==&sentinel && !memcmp(cache,&before,sizeof(before)));
   assert(live_heap==old_heap && live_pages==old_pages);
  }else check_cache(&v,b.size,17);
  assert(vfs_page_cache_lookup(cache,257)->page==saved);
  vfs_page_cache_destroy(&v);reset();++cases;
 }
 /* 无效直接映射与参数不能发布缓存或改写输出。 */
 reset();vfs_backend_node_t b={.size=65536,.read=read_data};
 vnode_t v={.object={KOBJECT_TYPE_VNODE},.fs_private=&b,.size=b.size};
 page_t sentinel,*out=&sentinel;bad_direct=true;
 assert(vfs_file_page_get_batch(&v,0,16,&out)==K_EIO && out==&sentinel && !live_pages);
 vfs_page_cache_destroy(&v);reset();
 assert(vfs_file_page_get_batch(0,0,16,&out)==K_EINVAL);
 assert(vfs_file_page_get_batch(&v,UINT64_MAX,16,&out)==K_EINVAL);
 assert(vfs_file_page_get_batch(&v,0,16,0)==K_EINVAL);
 reset();b.size=UINT64_MAX;b.read=read_data;v.page_cache=0;
 out=0;
 assert(vfs_file_page_get_batch(&v,UINT64_MAX/PAGE_SIZE,16,&out)==K_OK);
 assert(((vfs_page_cache_t*)v.page_cache)->entry_count==1);
 check_bytes(out,UINT64_MAX/PAGE_SIZE,b.size);
 vfs_page_cache_destroy(&v);reset();
 /* 真正的缓存读取串联填充：128 KiB 冷读应为两次 64 KiB 后端请求。 */
 const unsigned offsets[]={0,13,4095,65535};
 const unsigned lengths[]={0,1,8192,65536,131072};
 static unsigned char output[131072+32];
 for(unsigned o=0;o<4;++o)for(unsigned l=0;l<5;++l){
  reset();vfs_backend_node_t backend={.size=131072,.read=read_data};
  vnode_t node={.object={KOBJECT_TYPE_VNODE},.fs_private=&backend,.size=backend.size};
  file_t file={.object={KOBJECT_TYPE_FILE},.vnode=&node,.rights=VFS_OPEN_READ,
   .position=offsets[o]};
  memset(output,0x5a,sizeof(output));uint64_t bytes=UINT64_MAX;
  unsigned expected=lengths[l];
  if(expected>backend.size-offsets[o])expected=backend.size-offsets[o];
  assert(vfs_read_cached_kernel(&file,output+16,lengths[l],&bytes)==K_OK);
  assert(bytes==expected && file.position==offsets[o]+expected && !lock_depth);
  for(unsigned i=0;i<bytes;++i)assert(output[16+i]==(unsigned char)((offsets[o]+i)*19+7));
  for(unsigned i=0;i<16;++i)assert(output[i]==0x5a);
  for(unsigned i=16+bytes;i<sizeof(output);++i)assert(output[i]==0x5a);
  if(!o&&lengths[l]==131072)assert(reads==2 && live_pages==32);
  unsigned old_reads=reads;file.position=offsets[o];
  assert(vfs_read_cached_kernel(&file,output+16,lengths[l],&bytes)==K_OK && reads==old_reads);
  vfs_page_cache_destroy(&node);reset();++cases;
 }
 printf("File page batches: %u range/neighbor/metadata/physical/read/rollback cases; %u exhaustive EOF cases; complete cached 128KiB cold read backend calls 16 -> 2; warm reread no I/O\n",cases,eof_cases);
}
'''
with tempfile.TemporaryDirectory(prefix='file-page-batch-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-x', 'c', '-', '-o', binary]
    def run(code):
        subprocess.run(command, input=prefix+code+setup+cases, text=True, check=True)
        return subprocess.run([binary], capture_output=True, text=True, timeout=30,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    result=run(production)
    assert result.returncode==0,result.stdout+result.stderr
    print(result.stdout,end='')
    for label,old,new in (
        ('two-page cold read', 'position / PAGE_SIZE, request_pages, &page)',
         'position / PAGE_SIZE, (request_pages?2U:2U), &page)'),
        ('stale EOF allocation bytes', 'if (read_length < capacity) memset(memory + read_length, 0, capacity - read_length);',
         '(void)capacity;'),
    ):
        assert old in production
        result=run(production.replace(old,new))
        assert result.returncode==-signal.SIGABRT and 'Assertion' in result.stderr
        print('Negative control rejected:',label)
