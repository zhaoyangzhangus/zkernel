"""Check actual preprocessed release/diagnostic boot code, not raw markers."""
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[2]

def preprocess(file, realtest, serial):
    return subprocess.check_output([
        'cc','-E','-P','-DOS_KERNEL_BUILD',
        '-DOS_REALTEST='+str(realtest),'-DOS_DEBUG_SERIAL='+str(serial),
        '-I',str(root/'include'),str(root/file)],text=True)

def function(text, signature):
    start=text.rindex(signature)
    end=text.index('{',start)+1
    depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]

for realtest,serial in ((0,0),(0,1),(1,0),(1,1)):
    diagnostic=bool(realtest or serial)
    scheduler=preprocess('kernel/init/scheduler.c',realtest,serial)
    boot=function(scheduler,'BOOLEAN kernel_init_scheduler(')
    for name in ('scheduler_benchmark_queue_paths','scheduler_benchmark_schedule_paths',
                 'scheduler_benchmark_local_wake'):
        assert (name+'(' in boot)==diagnostic,name
    for name in ('canonical_scheduler_self_test','sched_accounting_self_test',
                 'sched_state_transition_self_test','sched_context_switch_self_test',
                 'kmutex_pi_self_test','process_core_self_test','x86_smp_start_aps'):
        assert name+'(' in boot,name
    post=function(preprocess('kernel/init/self_tests.c',realtest,serial),
                  'BOOLEAN kernel_init_post_scheduler(')
    assert ('kernel_perf_benchmark(' in post)==diagnostic
    for name in ('kernel_init_filesystem','kernel_init_network','kernel_init_graphics',
                 'kernel_init_userspace','resource_core_self_test'):
        assert name+'(' in post,name
    runtime=preprocess('kernel/init/runtime.c',realtest,serial)
    high=function(runtime,'kernel_runtime_main(void *context)')
    assert ('runtime_record_display_cache(' in high)==diagnostic
    assert 'framebuffer_enable_wc(' in high
    assert 'display_core_init(' in high
    print(f'Boot fast path: realtest={realtest} serial={serial}, benchmark/file-flush policy and mandatory initialization PASS')
