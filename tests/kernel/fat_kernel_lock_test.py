"""验证 FAT 内核路径：变更锁允许抢占，打开文件表仅使用短临界区。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/nativefs/directory_lifecycle.c').read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'


production = ''.join(function(signature) for signature in (
    'static void fat32_lock_word(', 'VOID fat32_open_files_lock(',
    'VOID fat32_open_files_unlock(', 'VOID fat32_mutation_lock(',
    'VOID fat32_mutation_unlock('))
program = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#define OS_KERNEL_BUILD 1
typedef void VOID;
typedef uint32_t UINT32;
typedef pthread_mutex_t io_mutex_t;
typedef struct {UINT32 OpenFileLock, MutationLock; io_mutex_t MutationMutex;} OS_FAT32;
static _Thread_local unsigned depth;
static void sched_preempt_disable(void){++depth;}
static void sched_preempt_enable(void){assert(depth);--depth;}
static void io_mutex_lock_required(io_mutex_t *mutex){
    assert(!depth); assert(!pthread_mutex_lock(mutex)); assert(!depth);
}
static void io_mutex_unlock_required(io_mutex_t *mutex){
    assert(!depth); assert(!pthread_mutex_unlock(mutex));
}
''' + production + r'''
static OS_FAT32 fs={.MutationLock=0x12345678U, .MutationMutex=PTHREAD_MUTEX_INITIALIZER};
static uint64_t value, checksum;
static void *worker(void *unused){
    (void)unused;
    for(unsigned i=0;i<20000;++i){
        fat32_mutation_lock(&fs); assert(!depth);
        assert(checksum==value*17); ++value; checksum=value*17;
        fat32_open_files_lock(&fs); assert(depth==1 && fs.OpenFileLock==1);
        fat32_open_files_unlock(&fs); assert(!depth && !fs.OpenFileLock);
        fat32_mutation_unlock(&fs); assert(!depth);
    }
    return 0;
}
int main(void){
    /* 打开文件表的短锁必须保留调用方原有的抢占层数。 */
    sched_preempt_disable(); fat32_open_files_lock(&fs); assert(depth==2);
    fat32_open_files_unlock(&fs); assert(depth==1); sched_preempt_enable();
    pthread_t threads[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,worker,0));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    assert(value==80000 && checksum==1360000 && !fs.OpenFileLock && !depth);
    /* 内核路径不能再访问仅供非内核实现使用的原始变更锁字段。 */
    assert(fs.MutationLock==0x12345678U);
    puts("FAT kernel guards: 80000 preemptible mutations; nested short table lock preserves preemption depth");
}
'''
with tempfile.TemporaryDirectory(prefix='fat-kernel-lock-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
