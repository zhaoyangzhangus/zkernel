"""fork 页表查找/权限修改融合：真实实现与旧两步路径逐项对照。"""
from pathlib import Path
import subprocess
import resource
import signal
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/arch/x86_64/mm/page_table.c').read_text()
def function(name):
    start = source.index(name)
    return source[start:source.index('\n}',start)+2]
actual = '\n'.join(function(name) for name in (
    'static kstatus_t page_table_next_present_locked(',
    'kstatus_t x86_next_present_page(',
    'static kstatus_t page_table_protect_user_entry_locked(',
    'kstatus_t x86_protect_user_page_deferred(',
    'kstatus_t x86_protect_next_user_page_deferred('))
defines = '\n'.join(line for line in
    (root/'kernel/arch/x86_64/mm/mmu_internal.h').read_text().splitlines()
    if line.startswith('#define PTE_'))
protection = (root/'kernel/mm/protection.c').read_text()
vm_space = (root/'kernel/mm/vm_space.c').read_text()
vm_flags = vm_space[vm_space.index('uint32_t hardware_flags('):vm_space.index('/*\n * 只有在远端 CPU')]
resident = protection[protection.index('static kstatus_t protect_resident_pages('):protection.index('static bool range_fully_mapped(')]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <arch/x86_64/paging.h>
#include <kernel/vm.h>
static uint64_t tables[8][512];
static unsigned locks,resolutions,depth;
static void table_lock(paddr_t root,vaddr_t va) {
 (void)root;(void)va;assert(!depth);++depth;++locks;
}
static void table_unlock(void) { assert(depth==1);--depth; }
static uint64_t *table_from_physical(uint64_t entry) {
 assert(depth==1);++resolutions;
 unsigned index=(entry&0x000ffffffffff000ULL)>>12;
 return index<8?tables[index]:0;
}
bool x86_is_canonical(uint64_t va) { return va<=X86_64_USER_TOP; }
uint64_t x86_pte_cache_bits(enum x86_cache_mode cache,bool large) {
 assert(!large);return cache==X86_CACHE_UC?16:0;
}
static unsigned shadow_lookups;
#define VM_AREA_COW (1U << 31)
static page_t *anon_page_lookup(vm_object_t *object,uint64_t index) {
 assert(object&&!depth);++shadow_lookups;
 return index%2 ? (page_t *)(uintptr_t)1 : 0;
}
kstatus_t x86_protect_page(paddr_t root,vaddr_t va,uint32_t flags,enum x86_cache_mode cache) {
 (void)root;(void)va;(void)flags;(void)cache;
 assert(!"本夹具只验证用户权限路径");return K_EIO;
}
''' + defines + '\n' + actual + '\n' + vm_flags + resident + r'''
static void initialize(void) {
 memset(tables,0,sizeof(tables));
 tables[1][0]=0x2001;tables[2][0]=0x3001;tables[3][0]=0x4001;
 locks=resolutions=depth=0;
}
static void resident_integration(void) {
 vm_space_t space={.root_table={4096}};
 vm_object_t file={.type=VM_OBJECT_FILE},shadow={0};
 vm_area_t area={.start=0,.end=512*PAGE_SIZE};
 for(unsigned mode=0;mode<4;++mode){
  initialize();shadow_lookups=0;
  area.object=&file;area.private_object=mode>=2?&shadow:0;
  area.flags=mode==1||mode==3?VM_AREA_COW:0;
  for(unsigned i=0;i<512;++i)tables[4][i]=0x900007ULL+i*PAGE_SIZE;
  bool changed=false;
  assert(protect_resident_pages(&space,&area,
   VM_PROT_USER|VM_PROT_READ|VM_PROT_WRITE,&changed)==K_OK);
  assert(locks==(mode<2?512U:1024U)&&!depth);
  assert(shadow_lookups==(mode>=2?512U:0U));
  for(unsigned i=0;i<512;++i){
   bool writable=mode==0||(mode==2&&i%2);
   assert(((tables[4][i]&PTE_WRITE)!=0)==writable);
   assert((tables[4][i]&PTE_NO_EXECUTE)!=0);
  }
  assert(changed);
  changed=false;locks=0;
  assert(protect_resident_pages(&space,&area,
   VM_PROT_USER|VM_PROT_READ|VM_PROT_WRITE,&changed)==K_OK&&!changed);
 }
 puts("mprotect 真实页表集成：普通/COW 区域加锁 1024 -> 512，私有文件逐页影子权限与重复保护结果正确");
}
int main(void) {
 resident_integration();
 const paddr_t root={4096};const uint64_t end=512*PAGE_SIZE;
 initialize();
 for(unsigned i=0;i<512;++i)tables[4][i]=(0x900000ULL+i*PAGE_SIZE)|
  PTE_PRESENT|PTE_USER|PTE_WRITE|PTE_ACCESSED|PTE_DIRTY|PTE_NO_EXECUTE;
 for(unsigned i=0;i<512;++i) {
  vaddr_t found=99,combined=88;bool changed=false,fused=false;
  uint64_t old=tables[4][i];locks=resolutions=0;
  assert(x86_next_present_page(root,i*PAGE_SIZE,end,&found)==K_OK);
  assert(x86_protect_user_page_deferred(root,found,X86_PAGE_USER,X86_CACHE_WB,&changed)==K_OK);
  assert(locks==2&&resolutions==8&&changed&&found==i*PAGE_SIZE);
  uint64_t expected=tables[4][i];tables[4][i]=old;locks=resolutions=0;
  assert(x86_protect_next_user_page_deferred(root,i*PAGE_SIZE,end,
   X86_PAGE_USER,X86_CACHE_WB,&combined,&fused)==K_OK);
  assert(locks==1&&resolutions==4&&fused&&combined==found&&tables[4][i]==expected&&!depth);
  assert(x86_protect_next_user_page_deferred(root,i*PAGE_SIZE,end,
   X86_PAGE_USER,X86_CACHE_WB,&combined,&fused)==K_OK&&!fused);
 }
 // 稀疏分支仍跳过洞；不应按 64 TiB 的虚拟页数遍历。
 initialize();tables[4][500]=0x900007;
 tables[2][100]=0x5001;tables[5][17]=0x6001;tables[6][8]=0xa00007;
 vaddr_t found=99;bool changed=false;
 assert(x86_protect_next_user_page_deferred(root,0,1ULL<<46,
  X86_PAGE_USER,X86_CACHE_WB,&found,&changed)==K_OK&&found==500*PAGE_SIZE&&changed);
 resolutions=0;uint64_t next=(100ULL<<30)+(17ULL<<21)+8*PAGE_SIZE;
 assert(x86_protect_next_user_page_deferred(root,found+PAGE_SIZE,1ULL<<46,
  X86_PAGE_USER,X86_CACHE_WB,&found,&changed)==K_OK&&found==next&&changed&&resolutions<10);
 assert(x86_protect_next_user_page_deferred(root,found+PAGE_SIZE,1ULL<<46,
  X86_PAGE_USER,X86_CACHE_WB,&found,&changed)==K_ENOENT&&!changed);
 // 权限、缓存、硬件记账位与旧单页实现逐项对照。
 unsigned cases=0;
 for(unsigned bits=0;bits<32;++bits)for(unsigned flags=0;flags<4;++flags)
 for(unsigned cache=0;cache<2;++cache) {
  initialize();uint64_t old=0x900001ULL|
   (bits&1?PTE_USER:0)|(bits&2?PTE_GLOBAL:0)|(bits&4?PTE_WRITE:0)|
   (bits&8?PTE_ACCESSED:0)|(bits&16?PTE_DIRTY:0);
  tables[4][5]=old;bool a=false,b=false;
  uint32_t permissions=X86_PAGE_USER|(flags&1?X86_PAGE_WRITE:0)|(flags&2?X86_PAGE_EXEC:0);
  enum x86_cache_mode mode=cache?X86_CACHE_UC:X86_CACHE_WB;
  kstatus_t expected=x86_protect_user_page_deferred(root,5*PAGE_SIZE,permissions,mode,&a);
  uint64_t pte=tables[4][5];tables[4][5]=old;
  assert(x86_protect_next_user_page_deferred(root,0,end,permissions,mode,&found,&b)==expected);
  assert(found==5*PAGE_SIZE&&tables[4][5]==pte&&a==b&&!depth);++cases;
 }
 initialize();tables[2][7]=0x80000000|PTE_PRESENT|PTE_LARGE;
 uint64_t large=tables[2][7];
 assert(x86_protect_next_user_page_deferred(root,7ULL<<30,8ULL<<30,
  X86_PAGE_USER,X86_CACHE_WB,&found,&changed)==K_OK&&!changed&&found==(7ULL<<30));
 assert(tables[2][7]==large);
 initialize();tables[3][0]=0x99001;found=99;
 assert(x86_protect_next_user_page_deferred(root,0,end,X86_PAGE_USER,
  X86_CACHE_WB,&found,&changed)==K_EIO&&!changed&&found==99&&!depth);
 locks=0;
 assert(x86_protect_next_user_page_deferred(root,1,end,X86_PAGE_USER,X86_CACHE_WB,&found,&changed)==K_EINVAL);
 assert(x86_protect_next_user_page_deferred(root,0,end,X86_PAGE_GLOBAL,X86_CACHE_WB,&found,&changed)==K_EINVAL);
 assert(x86_protect_next_user_page_deferred(root,0,end,X86_PAGE_USER,X86_CACHE_WB,0,&changed)==K_EINVAL);
 assert(x86_protect_next_user_page_deferred(root,0,end,X86_PAGE_USER,X86_CACHE_WB,&found,0)==K_EINVAL);
 assert(!locks&&!depth);
 printf("PASS: 512 dense pages: locks 1024 -> 512, table resolutions 4096 -> 2048; 64 TiB sparse range, %u permission/cache comparisons, malformed/large mappings and invalid inputs\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='fork-protect-walk-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)
    guard = 'bool combined = (uniform_flags &'
    assert code.count(guard)==1
    negative = code.replace(guard, 'bool combined = false && (uniform_flags &')
    (path/'test.c').write_text(negative)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    result = subprocess.run([str(path/'test')],capture_output=True,text=True,timeout=15,
        preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode == -signal.SIGABRT and 'locks==' in result.stderr
    print('负面对照通过：恢复两次页表遍历会触发真实锁次数断言')

