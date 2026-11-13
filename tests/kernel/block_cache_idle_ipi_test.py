"""Execute the production cache lock's idle contention/IRQ restoration path."""
from pathlib import Path
import subprocess
import tempfile
import resource
import signal

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/pagecache/cache.c').read_text()
start = source.index('static BOOLEAN cache_lock(')
end = source.index('\nstatic VOID cache_unlock(', start)
body = source[start:end]
body = body.replace('__asm__ volatile ("pushfq; popq %0"\n'
                    '                                          : "=r"(flags) : : "memory");',
                    'flags = irq_flags;')
body = body.replace('__asm__ volatile ("sti" : : : "memory");',
                    'enable_irq();')
body = body.replace('__asm__ volatile ("cli" : : : "memory");',
                    'irq_flags &= ~(1ULL << 9);')
body = body.replace('__asm__ volatile ("pause");', 'pause_probe();')
assert '__asm__' not in body
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef int BOOLEAN;
typedef int kstatus_t;
enum { K_OK=0, K_EAGAIN=-11, K_EBUSY=-16, SCHED_CLASS_IDLE=0 };
typedef struct { unsigned sched_class; } thread_t;
typedef struct { int WaitLock; unsigned Lock; } OS_BLOCK_CACHE;
static thread_t thread;
static thread_t *current;
static unsigned depth, outer, tries, contended, pauses, enabled, begins, ends;
static unsigned busy_attempts;
static unsigned diagnostics, sleeps, ack;
static UINT64 irq_flags;
static bool waiting;
static int result;
static thread_t *sched_current_thread(void) { return current; }
static void sched_preempt_disable(void) { ++depth; }
static void sched_preempt_enable(void) { assert(depth); --depth; }
static bool x86_tlb_shootdown_active(void) { return waiting; }
static void x86_tlb_wait_begin(void) { assert(!waiting); waiting=true; ++begins; }
static void x86_tlb_wait_end(void) { assert(waiting); waiting=false; ++ends; }
static void enable_irq(void) {
 assert(depth==outer+1 && waiting); irq_flags |= 1ULL<<9; ++enabled;
 // 模拟 TLB IPI 解除页表所有者阻塞，继而解除远端缓存所有者的阻塞。空闲续执行路径上不发生调度切换。
 ++ack;
}
static void pause_probe(void) { assert(++pauses<=busy_attempts); }
static kstatus_t kmutex_try_lock(int *lock) {
 (void)lock; ++tries;
 if(contended && tries>1)
  assert(ack && (irq_flags&(1ULL<<9)) && waiting && depth==outer+1);
 if(contended && tries<=busy_attempts) return K_EAGAIN;
 return result;
}
static kstatus_t kmutex_lock(int *lock, UINT64 timeout) {
 (void)lock; assert(timeout==UINT64_MAX); ++sleeps; return result;
}
static void block_cache_diag(UINT32 status,UINT64 lba,OS_BLOCK_CACHE *cache) {
 (void)status;(void)lba;(void)cache; ++diagnostics;
}
''' + body + r'''
int main(void) {
 OS_BLOCK_CACHE cache={0};
 // 覆盖关闭/开启 IRQ 的调用方、嵌套抢占和已有的 TLB 等待状态。
 for(unsigned irq=0;irq<2;++irq) for(unsigned nesting=0;nesting<3;++nesting)
 for(unsigned prior=0;prior<2;++prior) for(unsigned fail=0;fail<2;++fail)
 for(busy_attempts=1;busy_attempts<=7;++busy_attempts) {
  current=&thread; thread.sched_class=SCHED_CLASS_IDLE;
  outer=nesting; depth=outer; waiting=prior; irq_flags=0x46|((UINT64)irq<<9);
  tries=pauses=enabled=begins=ends=diagnostics=sleeps=ack=0;
  contended=1; result=fail?K_EBUSY:K_OK;
  assert(cache_lock(&cache,12)==!fail);
  assert(tries==busy_attempts+1 && pauses==busy_attempts && enabled==1 && ack==1 && !sleeps);
  assert(depth==outer && waiting==(bool)prior && irq_flags==(0x46|((UINT64)irq<<9)));
  assert(begins==!prior && ends==!prior && diagnostics==fail);
 }
 // 空闲时无竞争的加锁操作不得改变中断/抢占状态。
 contended=0; tries=enabled=begins=ends=ack=0; result=K_OK;
 depth=outer=2; waiting=false; irq_flags=0;
 assert(cache_lock(&cache,0));
 assert(tries==1 && !enabled && !begins && !ends && !ack && depth==2 && !irq_flags);
 // 普通线程仍走阻塞互斥锁路径；调度器启动前仍使用原始路径。
 thread.sched_class=1; sleeps=0;
 assert(cache_lock(&cache,0) && sleeps==1 && depth==2 && !irq_flags);
 current=0; assert(cache_lock(&cache,0) && cache.Lock==1);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='cache-idle-ipi-') as directory:
    path = Path(directory)
    def compile_case(name, text):
        source_path = path / (name + '.c')
        executable = path / name
        source_path.write_text(text)
        subprocess.run(['cc', '-DOS_KERNEL_BUILD', '-std=gnu11', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', str(source_path), '-o', str(executable)],
                       check=True)
        return executable

    subprocess.run([str(compile_case('test', program))], check=True, timeout=10)
    idle_start = body.index('            UINT64 flags =')
    idle_end = body.index('            if (status != K_OK)', idle_start)
    old_body = body[:idle_start] + '''            do {
                status = kmutex_try_lock(&cache->WaitLock);
                if (status == K_EAGAIN) pause_probe();
            } while (status == K_EAGAIN);
''' + body[idle_end:]
    mutations = {
        'old_irq_off_spin': program.replace(body, old_body),
        'no_ipi_delivery': program.replace('                        enable_irq();',
                                           '                        /* no IPI delivery */'),
        'lost_outer_preemption': program.replace('                sched_preempt_enable();',
                                                 '                depth = 0;'),
        'lost_outer_tlb_wait': program.replace('if (!already_waiting) x86_tlb_wait_end();',
                                               'x86_tlb_wait_end();'),
    }
    for name, mutated in mutations.items():
        assert mutated != program, name
        executable = compile_case(name, mutated)
        # 有意失败的对照测试不得在宿主机上生成 core 文件。
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        result = subprocess.run([str(executable)], capture_output=True, text=True,
                                timeout=10, preexec_fn=no_core)
        assert result.returncode == -signal.SIGABRT and 'Assertion' in result.stderr, (
            name, result.returncode, result.stderr)
        print('Negative control rejected:', name)
print('Cache idle contention: TLB acknowledgement, IRQ/preemption nesting, '
      '168 delayed-owner cases, existing wait state, errors and '
      'uncontended/thread/boot paths passed')
