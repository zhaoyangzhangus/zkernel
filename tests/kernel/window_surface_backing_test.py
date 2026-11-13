"""Execute actual backing creation, including each partial-allocation failure."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/window.c').read_text()
def function(name):
    start = source.index('static kstatus_t ' + name + '(')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define PAGE_SIZE 4096ULL
#define K_OK 0
#define K_EINVAL -22
#define K_ENOMEM -12
typedef int kstatus_t;
typedef struct {unsigned pages;} vm_object_t;
typedef struct {vm_object_t *vm_object;} shared_section_t;
static vm_object_t object;
static shared_section_t section = {&object};
static int failure=-1,create_error;
static unsigned creates,marks,puts;
static uint64_t size;
static kstatus_t shared_section_create_populated(uint64_t bytes,shared_section_t **out) {
    ++creates;size=bytes;
    if(create_error)return create_error;
    object.pages=0;
    for(uint64_t offset=0;offset<bytes;offset+=PAGE_SIZE){
        if((int)object.pages==failure){object.pages=0;return K_ENOMEM;}
        ++object.pages;
    }
    *out=&section;return K_OK;
}
static void vm_object_mark_window_surface(vm_object_t *o) {
    assert(o==&object);++marks;
}
static void object_put(shared_section_t *s) {
    assert(s==&section);++puts;object.pages=0;
}
''' + function('window_surface_create_backing') + r'''
int main(void) {
    shared_section_t *out=&section;
    assert(window_surface_create_backing(4096,2,0)==K_EINVAL);
    assert(window_surface_create_backing(0,2,&out)==K_EINVAL && !out);
    assert(window_surface_create_backing(1,2,&out)==K_EINVAL && !out);
    assert(window_surface_create_backing(4096,3,&out)==K_EINVAL && !out);
    assert(window_surface_create_backing(UINT64_MAX-4095,2,&out)==K_EINVAL && !out);
    assert(!creates);
    create_error=-5;
    assert(window_surface_create_backing(4096,2,&out)==-5 && !out);
    assert(!marks && !puts);
    create_error=0;
    for(int i=0;i<8;++i) {
        failure=i;unsigned before=puts;
        assert(window_surface_create_backing(4*PAGE_SIZE,2,&out)==K_ENOMEM);
        assert(!out && object.pages==0 && puts==before && !marks && size==8*PAGE_SIZE);
    }
    failure=-1;
    assert(window_surface_create_backing(4*PAGE_SIZE,2,&out)==K_OK);
    assert(out==&section && object.pages==8);object_put(out);
    assert(window_surface_create_backing(4*PAGE_SIZE,1,&out)==K_OK);
    assert(out==&section && object.pages==4 && size==4*PAGE_SIZE);object_put(out);
}
'''
with tempfile.TemporaryDirectory(prefix='window-surface-backing-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=20)
print('Surface backing: one/two slots fully populated, overflow validation, '
      'create errors and every partial allocation rollback passed')
