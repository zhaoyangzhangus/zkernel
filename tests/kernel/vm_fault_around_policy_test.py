"""Actual fault-around/population: demand-only ordinary memory and VMA races."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/fault.c').read_text()
selector = source[source.index('static bool vm_area_fault_around_eligible('):source.index('\n#if VM_FAULT_TELEMETRY_ENABLED', source.index('static bool vm_area_fault_around_eligible('))]
populate = source[source.index('static kstatus_t vm_populate_range_internal('):
                  source.index('kstatus_t vm_populate_range(')]
around = source[source.rindex('static void vm_fault_around('):]
# 编译实际缺页处理的锁内前缀，验证提示的产生、失败清零与保护锁语义。
resolver = source.index('static kstatus_t vm_handle_fault_impl(', source.index('bool vm_handle_current_fault('))
prefix_start = source.index('{', resolver) + 1
prefix_end = source.index('    if (trace != 0)', prefix_start)
prefix = source[prefix_start:prefix_end].replace('retry_fault:\n', '')
hint = ('static kstatus_t resolve_hint(vm_space_t *space, '
        'const vm_fault_info_t *fault, bool *surface_hint) {' + prefix +
        '    map_unlock(space); return K_OK;\n}\n')

vma = (root / 'kernel/mm/vma.c').read_text()
ranges = vma[vma.index('bool range_size_valid('):vma.index('static bool find_gap_subtree(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/vm.h>
#define VM_USER_BASE 0x10000ULL
#define VM_USER_END 0x800000000000ULL
#define X86_CACHE_WB 0U
static vm_space_t space;
static vm_area_t area;
static vm_object_t object,replacement;
static page_t pages[64];
static unsigned held,locks,allocations,mappings,faults,retains,releases;
static bool replace_on_unlock;
static void map_lock(vm_space_t *s){assert(s==&space && !held);held=1;++locks;}
static void map_unlock(vm_space_t *s){
 assert(s==&space && held);held=0;
 if(replace_on_unlock){replace_on_unlock=false;area.object=&replacement;}
}
static vm_area_t *find_area(const vm_space_t *s,uint64_t address){
 assert(s==&space && held);return address>=area.start && address<area.end?&area:NULL;
}
void vm_object_get(vm_object_t *o){
 assert(held && o==area.object);++retains;atomic_fetch_add(&o->refs.value,1);
}
void vm_object_put(vm_object_t *o){
 assert(!held && (o==&object || o==&replacement));++releases;
 assert(atomic_fetch_sub(&o->refs.value,1)>1);
}
static uint32_t hardware_flags(uint32_t prot,bool cow){assert(!cow);return prot;}
static kstatus_t x86_translate_page(paddr_t root,vaddr_t address,paddr_t *p,uint64_t *flags){
 (void)root;(void)address;(void)p;(void)flags;return K_ENOENT;
}
static kstatus_t anon_page_get(vm_object_t *o,uint64_t index,bool create,bool pin,page_t **out){
 assert(!pin && held && create && o==area.object && index<64);
 assert(o->type==VM_OBJECT_SHARED && (o->flags&VM_OBJECT_FLAG_WINDOW_SURFACE));
 ++allocations;*out=&pages[index];return K_OK;
}
paddr_t page_to_phys(const page_t *p){return (paddr_t){(uint64_t)(p-pages)*PAGE_SIZE};}
static kstatus_t x86_map_page(paddr_t root,vaddr_t address,paddr_t physical,uint32_t flags,uint32_t cache){
 (void)root;assert(held && flags==area.prot && cache==X86_CACHE_WB);
 assert(physical.value==area.object_offset+address-area.start);++mappings;return K_OK;
}
kstatus_t vm_handle_fault(vm_space_t *s,const vm_fault_info_t *fault){
 assert(s==&space && !held && fault->access==VM_PROT_READ);++faults;return K_OK;
}
''' + ranges + selector + populate + around + hint + r'''
static unsigned cases;
static void setup(unsigned type,unsigned flags,unsigned area_flags,unsigned count){
 memset(&space,0,sizeof(space));memset(&object,0,sizeof(object));
 memset(&replacement,0,sizeof(replacement));memset(&area,0,sizeof(area));
 object.type=type;object.flags=flags;object.size=(count+3U)*PAGE_SIZE;
 refcount_init(&object.refs,1);replacement=object;refcount_init(&replacement.refs,1);
 area.start=VM_USER_BASE;area.end=area.start+count*PAGE_SIZE;
 area.object_offset=3U*PAGE_SIZE;area.object=&object;area.flags=area_flags;
 area.prot=VM_PROT_READ|VM_PROT_WRITE|VM_PROT_USER;
 held=locks=allocations=mappings=faults=retains=releases=0;replace_on_unlock=false;
}
static void check(void){
 assert(!held && !replace_on_unlock && retains==releases);
 assert(atomic_load(&object.refs.value)==1 && atomic_load(&replacement.refs.value)==1);
 ++cases;
}
int main(void){
 const unsigned types[]={VM_OBJECT_ANON,VM_OBJECT_SHARED,VM_OBJECT_FILE,
                         VM_OBJECT_DEVICE,VM_OBJECT_PAGE_ARRAY};
 const unsigned flags[]={0,VM_OBJECT_FLAG_WINDOW_SURFACE,VM_OBJECT_FLAG_MAP_OWNED,
                         VM_OBJECT_FLAG_WINDOW_SURFACE|VM_OBJECT_FLAG_MAP_OWNED};
 for(unsigned t=0;t<5;++t)for(unsigned f=0;f<4;++f)
 for(unsigned stack=0;stack<2;++stack)for(unsigned count=1;count<=33;++count)
 for(unsigned at=0;at<count;++at){
  setup(types[t],flags[f],stack?VM_MAP_STACK:0,count);
  vm_fault_info_t fault={.address=area.start+at*PAGE_SIZE+37,.access=VM_PROT_WRITE};
  bool surface_hint=true;
  assert(resolve_hint(&space,&fault,&surface_hint)==K_OK && !held && locks==1);
  assert(surface_hint==(types[t]==VM_OBJECT_SHARED &&
         (flags[f]&VM_OBJECT_FLAG_WINDOW_SURFACE) && !stack));
  locks=0;
  vm_fault_around(&space,&fault);
  unsigned expected=0;
  if(types[t]==VM_OBJECT_SHARED && (flags[f]&VM_OBJECT_FLAG_WINDOW_SURFACE) && !stack){
   expected=count-at-1;if(expected>VM_FAULT_AROUND_PAGES-1)expected=VM_FAULT_AROUND_PAGES-1;
  }
  assert(allocations==expected && mappings==expected && !faults);
  assert(locks==(expected?2U:1U));
  assert(atomic_load(&space.rss_pages)==expected);
  assert(atomic_load(&object.fault_around_pages)==expected);
  check();
 }
 /* 缺页重试从旧窗口表面切换到空洞或保护区时，不得沿用旧提示。 */
 for(unsigned guard=0;guard<2;++guard){
  setup(VM_OBJECT_SHARED,VM_OBJECT_FLAG_WINDOW_SURFACE,guard?VM_MAP_GUARD:0,33);
  vm_fault_info_t fault={.address=guard?area.start:area.end,.access=VM_PROT_READ};
  bool surface_hint=true;
  assert(resolve_hint(&space,&fault,&surface_hint)==K_EACCES && !surface_hint);
  assert(locks==1 && !held && !allocations);check();
 }
 /* Sparse user heaps must not turn each isolated touch into sixteen/thirty
  * two pages of zeroed backing. Exercise the real selection repeatedly. */
 setup(VM_OBJECT_ANON,0,0,33);
 for(unsigned i=0;i<4096;++i){
  vm_fault_info_t fault={.address=area.start+(i%32)*PAGE_SIZE,.access=VM_PROT_READ};
  vm_fault_around(&space,&fault);
 }
 assert(!allocations && !mappings && !faults && !retains);check();
 /* The first map unlock is the handoff to population. A replacement VMA
  * must not inherit the old window's permission to allocate speculatively. */
 for(unsigned t=0;t<5;++t)for(unsigned f=0;f<4;++f){
  setup(VM_OBJECT_SHARED,VM_OBJECT_FLAG_WINDOW_SURFACE,0,33);
  replacement.type=types[t];replacement.flags=flags[f];replace_on_unlock=true;
  vm_fault_info_t fault={.address=area.start,.access=VM_PROT_READ};
  vm_fault_around(&space,&fault);
  unsigned expected=types[t]==VM_OBJECT_SHARED && (flags[f]&VM_OBJECT_FLAG_WINDOW_SURFACE)?VM_FAULT_AROUND_PAGES-1:0;
  assert(allocations==expected && mappings==expected && !faults);check();
 }
 /* Explicit population remains available for ordinary/shared memory; only
  * automatic speculation changes. The public caller's requested range wins. */
 for(unsigned t=0;t<2;++t){
  setup(types[t],0,0,33);
  assert(vm_populate_range_internal(&space,area.start,33U*PAGE_SIZE,false)==K_OK);
  assert(faults==33 && !allocations && !mappings && retains==1);check();
 }
 setup(VM_OBJECT_SHARED,VM_OBJECT_FLAG_WINDOW_SURFACE,0,33);
 vm_fault_info_t fault={.address=area.start,.access=VM_PROT_READ,.cpu_error=1};
 vm_fault_around(&space,&fault);assert(!locks && !allocations);check();
 vm_fault_around(NULL,&fault);vm_fault_around(&space,NULL);assert(!locks);check();
 printf("Fault-around policy (%u pages): %u type/flag/boundary/race cases; 4096 sparse heap touches allocate zero speculative pages; explicit population preserved\n",VM_FAULT_AROUND_PAGES,cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vm-fault-around-policy-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-I' + str(root / 'include'), '-fsanitize=address,undefined',
               '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary]

    def run(code, count):
        subprocess.run(command + ['-DVM_FAULT_AROUND_PAGES=' + str(count)],
                       input=code, text=True, check=True)
        return subprocess.run([binary], capture_output=True, text=True, timeout=30,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))

    for count in (16, 32):
        result = run(program, count)
        assert result.returncode == 0, result.stderr
        print(result.stdout, end='')
    for name, old, new in (
        ('stale window hint survives a missing mapping',
         'if (surface_hint != 0) *surface_hint = false;',
         '(void)surface_hint;'),
        ('ordinary heap fault unnecessarily reenters population',
         'area->object->type == VM_OBJECT_SHARED &&\n'
         '           (area->object->flags & VM_OBJECT_FLAG_WINDOW_SURFACE) != 0U',
         '(area->object->type == VM_OBJECT_ANON || area->object->type == VM_OBJECT_SHARED)'),
        ('replaced VMA inherits eager window policy',
         'if (fault_around &&\n        (!window_surface ||',
         'if (false && fault_around &&\n        (!window_surface ||'),
        ('explicit population wrongly suppressed',
         'if (fault_around &&\n        (!window_surface ||',
         'if (true &&\n        (!window_surface ||'),
    ):
        negative = program.replace(old, new)
        assert negative != program, name
        result = run(negative, 16)
        assert result.returncode != 0 and 'Assertion' in result.stderr, (name, result)
        print('Negative control rejected:', name)
    # Restore both old decisions together: either gate alone is enough to
    # stop cold-page population, but the complete old policy must fail too.
    legacy = program.replace(
        'area->object->type == VM_OBJECT_SHARED &&\n'
        '           (area->object->flags & VM_OBJECT_FLAG_WINDOW_SURFACE) != 0U',
        '(area->object->type == VM_OBJECT_ANON || area->object->type == VM_OBJECT_SHARED)')
    legacy = legacy.replace('if (fault_around &&\n        (!window_surface ||',
                            'if (false && fault_around &&\n        (!window_surface ||')
    result = run(legacy, 16)
    assert result.returncode != 0 and 'Assertion' in result.stderr, result
    print('Negative control rejected: original ordinary-memory speculative fault policy')
