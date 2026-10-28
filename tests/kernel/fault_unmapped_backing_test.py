"""An unlocked fault must not resurrect backing for a retired private slice."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/mm/fault.c').read_text()
start = source.index('        anon_page_node_t *retired = 0;')
end = source.index('        if (page_pinned) anon_page_unpin(page);', start)
body = source[start:end]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <kernel/vm.h>
static unsigned calls;
static uint64_t expected;
typedef struct anon_page_node { unsigned detached; } anon_page_node_t;
static anon_page_node_t node;
static bool locked;
static unsigned releases;
static void map_unlock(vm_space_t *space) {assert(space && locked);locked=false;}
static kstatus_t anon_page_retire_range(vm_object_t *o,uint64_t first,uint64_t end,
                                        anon_page_node_t **out) {
    assert(locked && o && first==expected && end==first+1);
    assert(!*out && !node.detached);++calls;node.detached=1;*out=&node;return K_OK;
}
static void anon_page_release_retired(anon_page_node_t *pages) {
    assert(!locked);
    if(pages){assert(pages==&node && node.detached);node.detached=0;++releases;}
}
static void retire(vm_area_t *area,vm_object_t *page_owner,
                   uint64_t page_address,uint64_t object_index) {
    vm_space_t vm={0};vm_space_t *space=&vm;
''' + body + r'''
}
int main(void) {
    unsigned cases=0;
    for(unsigned owned=0;owned<2;++owned)
     for(unsigned mode=0;mode<6;++mode)
      for(unsigned index=0;index<1024;++index) {
        vm_object_t owner={0},other={0};vm_area_t area={0};
        owner.flags=owned?VM_OBJECT_FLAG_MAP_OWNED:0;
        area.start=0x10000;area.end=area.start+4096;
        area.object_offset=(uint64_t)index*4096;
        area.object=mode==1?&owner:&other;
        area.private_object=mode==2?&owner:0;
        if(mode==3){area.object=&owner;area.object_offset+=4096;}
        calls=releases=0;expected=index;locked=true;
        retire(mode==0?0:&area,mode==5?0:&owner,area.start,index);
        assert(calls==(owned && mode!=1 && mode!=2 && mode!=5));
        assert(!locked && !node.detached && releases==calls);
        ++cases;
      }
    printf("Stale fault backing: %u unmap/replacement/split/shadow/external/pin-owner routes PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='fault-unmapped-backing-') as directory:
    path = Path(directory)
    for negative in (False, 'no-retire', 'locked-release'):
        text = program
        if negative == 'no-retire':
            text = text.replace('(void)anon_page_retire_range(page_owner, object_index,\n                                         object_index + 1U, &retired);', '(void)0;')
        elif negative == 'locked-release':
            text = text.replace('map_unlock(space);\n        anon_page_release_retired(retired);',
                                'anon_page_release_retired(retired);\n        map_unlock(space);')
        (path/'test.c').write_text(text)
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                        '-Wno-unused-function','-I'+str(root/'include'),
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        result = subprocess.run([str(path/'test')],capture_output=bool(negative),text=True)
        if negative:
            assert result.returncode!=0 and 'Assertion' in result.stderr
            print('Negative control: '+negative+' fails PASS')
        else:
            result.check_returncode()
