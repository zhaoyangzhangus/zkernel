"""Actual VMA unmap must retire only its internally owned backing slices."""
from pathlib import Path
import runpy
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
fixture = runpy.run_path(str(root/'tests/kernel/vm_unmap_seek_test.py'))['program']
fixture = fixture[:fixture.index('int main(void)')]
old = '''static kstatus_t anon_page_retire_range(vm_object_t *o,uint64_t first,uint64_t end,
                                        struct anon_page_node **out) {
    (void)o;(void)first;(void)end;(void)out;assert(0);return K_OK;
}
static void anon_page_release_retired(struct anon_page_node *pages) {assert(!locked && !pages);}'''
new = '''static bool backing[8][64];
static struct anon_page_node detached[16];
static unsigned detached_count,released_count;
static kstatus_t anon_page_retire_range(vm_object_t *o,uint64_t first,uint64_t end,
                                        struct anon_page_node **out) {
    assert(locked && first<end && end<=64 && tlb_calls);
    assert(atomic_load(&space.advice_generation));
    unsigned n;
    if(o>=objects && o<objects+4)n=(unsigned)(o-objects);
    else {assert(o>=shadows && o<shadows+4);n=4+(unsigned)(o-shadows);}
    assert(o->type==VM_OBJECT_ANON && (o->flags&VM_OBJECT_FLAG_MAP_OWNED));
    for(uint64_t p=first;p<end;++p){assert(backing[n][p]);backing[n][p]=false;}
    assert(detached_count<16);
    detached[detached_count].next=*out;*out=&detached[detached_count++];
    return K_OK;
}
static void anon_page_release_retired(struct anon_page_node *pages) {
    assert(!locked);
    while(pages){++released_count;pages=pages->next;}
}'''
assert old in fixture
fixture = fixture.replace(old, new)
main = r'''
int main(void) {
    unsigned cases=0;
    for(unsigned layout=0;layout<3;++layout)
      for(unsigned mode=0;mode<4;++mode)
       for(unsigned first=0;first<40;++first)
        for(unsigned last=first+1;last<=40;++last)
         for(unsigned failure=0;failure<4;++failure) {
            prepare(layout);memset(backing,0,sizeof(backing));
            detached_count=released_count=0;
            for(unsigned i=0;i<4;++i) {
                objects[i].type=mode==3?VM_OBJECT_SHARED:VM_OBJECT_ANON;
                shadows[i].type=VM_OBJECT_ANON;
                objects[i].flags=mode==0||mode==3?VM_OBJECT_FLAG_MAP_OWNED:0;
                shadows[i].flags=mode==1?VM_OBJECT_FLAG_MAP_OWNED:0;
                for(unsigned p=0;p<40;++p)if(initial[p] && p/8==i) {
                    backing[i][3+p%8]=true;backing[4+i][3+p%8]=true;
                }
            }
            if(failure==1)fail_alloc=1;
            else if(failure>=2)fail_tlb=failure-1;
            kstatus_t status=vm_unmap(&space,VM_USER_BASE+first*PAGE_SIZE,
                                      (last-first)*PAGE_SIZE);
            assert(status==K_OK||status==K_EIO||status==K_ENOMEM);
            assert(detached_count==released_count);
            for(unsigned p=0;p<40;++p)if(initial[p]) {
                unsigned i=p/8,index=3+p%8;
                assert(backing[i][index]==!(removed[p]&&mode==0));
                assert(backing[4+i][index]==!(removed[p]&&mode==1));
            }
            ++cases;
         }
    /* Internal slices cannot be exported as aliases into another mapping. */
    prepare(0);objects[0].type=VM_OBJECT_ANON;objects[0].size=64*PAGE_SIZE;
    objects[0].flags=VM_OBJECT_FLAG_MAP_OWNED;
    vaddr_t address=VM_USER_BASE+36*PAGE_SIZE;
    assert(vm_map_object(&space,&objects[0],&address,0,PAGE_SIZE,
                         VM_PROT_READ,VM_MAP_PRIVATE|VM_MAP_FIXED)==K_EINVAL);
    printf("Private backing retirement: %u real unmap head/tail/middle/hole/failure routes; shared/external objects retained, internal aliases rejected PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vm-unmap-backing-') as directory:
    path = Path(directory)
    for negative in (False, 'no-detach', 'locked-release'):
        body = fixture
        if negative == 'no-detach':
            body = body.replace('vm_unmap_discard_backing(area, cut_start, cut_end, retired);', '')
        elif negative == 'locked-release':
            old_release = 'map_unlock(space);\n    anon_page_release_retired(((vm_retired_t *)retired)->pages);'
            assert old_release in body
            body = body.replace(old_release,
                'anon_page_release_retired(((vm_retired_t *)retired)->pages);\n    map_unlock(space);')
        c = path/'test.c'
        c.write_text(body+main)
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                        '-Wno-unused-function','-I'+str(root/'include'),
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        str(c),'-o',str(path/'test')],check=True)
        result = subprocess.run([str(path/'test')],capture_output=bool(negative),
                                text=True,timeout=60)
        if negative:
            assert result.returncode!=0 and 'Assertion' in result.stderr
            print('Negative control: '+negative+' fails PASS')
        else:
            result.check_returncode()
