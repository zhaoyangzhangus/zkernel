"""Production FAT word locks: publication, independent fields and read polling."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/nativefs/directory_lifecycle.c').read_text()
def function(signature):
    begin=source.index(signature)
    return source[begin:source.index('\n}',begin)+2]+'\n'
code=''
if 'static void fat32_lock_word(' in source:
    code+=function('static void fat32_lock_word(')
for signature in ('VOID fat32_open_files_lock(', 'VOID fat32_open_files_unlock(',
                  'VOID fat32_mutation_lock(', 'VOID fat32_mutation_unlock('):
    code+=function(signature)
program=r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t UINT32;
typedef void VOID;
typedef struct {UINT32 OpenFileLock,MutationLock;} OS_FAT32;
static atomic_uint attempts,polls,waiting;
static bool __attribute__((unused)) tracked_cas(UINT32 *p,UINT32 *expected,UINT32 value,
                                               bool weak,int success,int failure) {
    atomic_fetch_add(&attempts,1);
    return __atomic_compare_exchange_n(p,expected,value,weak,success,failure);
}
static UINT32 __attribute__((unused)) tracked_load(UINT32 *p,int order) {
    atomic_fetch_add(&polls,1);return __atomic_load_n(p,order);
}
static UINT32 __attribute__((unused)) tracked_exchange(UINT32 *p,UINT32 value,int order) {
    atomic_fetch_add(&attempts,1);
    UINT32 old=__atomic_exchange_n(p,value,order);
    if(old) atomic_fetch_add(&polls,1);
    return old;
}
#define __atomic_compare_exchange_n tracked_cas
#define __atomic_load_n tracked_load
#define __atomic_exchange_n tracked_exchange
''' + code + r'''
#undef __atomic_compare_exchange_n
#undef __atomic_load_n
#undef __atomic_exchange_n
static OS_FAT32 fs;
static void (*acquire)(OS_FAT32 *),(*release)(OS_FAT32 *);
static uint64_t value,checksum;
static void *worker(void *unused) {
    (void)unused;atomic_fetch_add(&waiting,1);
    for(unsigned i=0;i<20000;++i) {
        acquire(&fs);assert(checksum==value*17);
        ++value;checksum=value*17;release(&fs);
    }
    return 0;
}
int main(void) {
    /* 持有一个字段的锁不能阻止获取另一个字段。 */
    fat32_open_files_lock(&fs);fat32_mutation_lock(&fs);
    assert(fs.OpenFileLock==1 && fs.MutationLock==1);
    fat32_open_files_unlock(&fs);assert(!fs.OpenFileLock && fs.MutationLock==1);
    fat32_mutation_unlock(&fs);
    for(unsigned kind=0;kind<2;++kind) {
        acquire=kind?fat32_mutation_lock:fat32_open_files_lock;
        release=kind?fat32_mutation_unlock:fat32_open_files_unlock;
        value=checksum=0;atomic_store(&waiting,0);atomic_store(&attempts,0);atomic_store(&polls,0);
        acquire(&fs);pthread_t t[4];
        for(unsigned i=0;i<4;++i) assert(!pthread_create(&t[i],0,worker,0));
        while(atomic_load(&waiting)<4 || atomic_load(&attempts)<5 || atomic_load(&polls)<20000)
            sched_yield();
        assert(atomic_load(&attempts)==5);
        release(&fs);
        for(unsigned i=0;i<4;++i) assert(!pthread_join(t[i],0));
        assert(value==80000 && checksum==1360000 && !fs.OpenFileLock && !fs.MutationLock);
        printf("FAT %s lock: 80000 publication updates; 20000 held reads, four failed acquire attempts passed\n",
               kind?"mutation":"open-file");
    }
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fat-lock-') as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)
