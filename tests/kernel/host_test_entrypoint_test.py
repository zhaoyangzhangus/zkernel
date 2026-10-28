"""Dry-run the real host test entrypoint; missing tools must not hide errors."""
from pathlib import Path
import subprocess
import re
from collections import Counter

def planned_scripts(output):
    paths=re.findall(r'tests/(?:kernel|user)/[A-Za-z0-9_]+_test\.py',output)
    # 图形组按名称拼接脚本路径；只检查字面路径会漏掉循环与直接调用之间的重复。
    for names in re.findall(r'for name in ([^;]+); do',output):
        paths.extend('tests/kernel/'+name+'_test.py' for name in names.split())
    return paths

root=Path(__file__).resolve().parents[2]
base=['make','-f','tools/host-tests.mk','-n','host-regression-test']
result=subprocess.run(base,cwd=root,text=True,capture_output=True,timeout=10)
assert result.returncode==0,result.stderr
assert all(count==1 for count in Counter(planned_scripts(result.stdout)).values())
assert planned_scripts(result.stdout).count('tests/kernel/gop_cursor_publication_test.py')==1
assert 'cc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined' in result.stdout
assert '-o build/message-box-test.exe' in result.stdout
assert '-o /message-box-test.exe' not in result.stdout
assert result.stdout.count('tests/kernel/smp_started_publication_test.py') == 1
assert result.stdout.count('tests/kernel/exec_load_buffer_test.py') == 1
assert result.stdout.count('tests/kernel/exec_metadata_read_test.py') == 1
# 共享目标必须在独立入口和任意目标顺序中保留覆盖，而非仅从总入口删除重复命令。
shared_suites={
    'background-cleanup-regression-test':('net_reap_fairness','scheduler_command_release','device_async_init'),
    'scheduler-regression-test':('scheduler_command_release',),
    'taskmgr-regression-test':('nv_port_thread_lifetime','task_monitor'),
    'nvidia-deployment-test':('device_async_init','nv_port_thread_lifetime'),
}
for suite,names in shared_suites.items():
    standalone=subprocess.run(['make','-f','tools/host-tests.mk','-n',suite],
                              cwd=root,text=True,capture_output=True,timeout=10)
    assert standalone.returncode==0,standalone.stderr
    for name in names:
        relative=f'tests/kernel/{name}_test.py'
        assert standalone.stdout.count(relative)==1,(suite,relative)
        assert result.stdout.count(relative)==1,relative
for goals in (('taskmgr-regression-test','scheduler-regression-test','network-regression-test',
               'lifecycle-regression-test','host-regression-test'),
              ('host-regression-test','network-regression-test','scheduler-regression-test',
               'taskmgr-regression-test','lifecycle-regression-test')):
    planned=subprocess.run(['make','-f','tools/host-tests.mk','-n','-j2',*goals],
                           cwd=root,text=True,capture_output=True,timeout=10)
    assert planned.returncode==0,planned.stderr
    paths=planned_scripts(planned.stdout)
    repeated={path:count for path,count in Counter(paths).items() if count>1}
    assert not repeated,repeated
lifecycle=subprocess.run(['make','-f','tools/host-tests.mk','-n','lifecycle-regression-test'],
                         cwd=root,text=True,capture_output=True,timeout=10)
assert lifecycle.returncode==0,lifecycle.stderr
lifecycle_tests=re.findall(r'tests/kernel/[A-Za-z0-9_]+_test\.py',lifecycle.stdout)
expected_lifecycle={test.relative_to(root).as_posix()
                    for pattern in ('wait_*_test.py','handle_*_test.py')
                    for test in root.glob('tests/kernel/'+pattern)}
expected_lifecycle.update(f'tests/kernel/{name}_test.py' for name in (
    'process_empty_exit','process_exit_cascade','process_terminate_batch',
    'task_monitor','thread_monitor_cursor','mutex_pi_lifecycle',
    'background_cleanup','deferred_startup','deferred_lock','exec_background_release',
    'net_reap_fairness','scheduler_command_release','device_async_init'))
assert len(lifecycle_tests)==len(set(lifecycle_tests))
assert set(lifecycle_tests)==expected_lifecycle,(set(lifecycle_tests),expected_lifecycle)
assert './build/wait-subscription-test.exe' in lifecycle.stdout
for relative in lifecycle_tests:
    assert (root/relative).is_file(),relative
    assert result.stdout.count(relative)==1,(relative,'missing or repeated lifecycle regression')
vfs_tests=sorted([*root.glob('tests/kernel/vfs_*_test.py'),
                  *root.glob('tests/kernel/file_page*_test.py'),
                  *root.glob('tests/kernel/fat_*_test.py')])
assert vfs_tests
vfs=subprocess.run(['make','-f','tools/host-tests.mk','-n','vfs-regression-test'],
                   cwd=root,text=True,capture_output=True,timeout=10)
assert vfs.returncode==0,vfs.stderr
network=subprocess.run(['make','-f','tools/host-tests.mk','-n','network-regression-test'],
                       cwd=root,text=True,capture_output=True,timeout=10)
assert network.returncode==0,network.stderr
assert 'tests/kernel/rtl8126_phy_reset_test.py' in network.stdout
network_tests=re.findall(r'tests/kernel/[A-Za-z0-9_]+_test\.py',network.stdout)
assert len(network_tests)==len(set(network_tests)) and network_tests
assert 'tests/kernel/socket_lookup_hint_test.py' in network_tests
assert 'tests/kernel/tcp_loopback_reset_test.py' in network_tests
for test in root.glob('tests/kernel/net_*_test.py'):
    relative=test.relative_to(root).as_posix()
    assert relative in network_tests,(relative,'network fixture omitted from standard gate')
for relative in network_tests:
    assert (root/relative).is_file(),relative
    assert result.stdout.count(relative)==1,(relative,'missing or repeated network test')
for test in vfs_tests:
    relative=test.relative_to(root).as_posix()
    assert result.stdout.count(relative)==1,(relative,'missing or repeated in host test gate')
    assert vfs.stdout.count(relative)==1,(relative,'missing or repeated in standalone VFS gate')
memory=subprocess.run(['make','-f','tools/host-tests.mk','-n','memory-regression-test'],
                      cwd=root,text=True,capture_output=True,timeout=10)
assert memory.returncode==0,memory.stderr
memory_tests=re.findall(r'tests/kernel/[A-Za-z0-9_]+_test\.py',memory.stdout)
assert memory_tests and len(memory_tests)==len(set(memory_tests))
for pattern in ('page_*_test.py','anon_*_test.py','vm_*_test.py',
                'fault_*_test.py','tlb_*_test.py','kmalloc_*_test.py',
                'fork_*_test.py','cow_*_test.py'):
    for test in root.glob('tests/kernel/'+pattern):
        assert test.relative_to(root).as_posix() in memory_tests,test
for relative in memory_tests:
    assert (root/relative).is_file(),relative
    assert result.stdout.count(relative)==1,(relative,'missing or repeated memory regression')
combined=subprocess.run(['make','-f','tools/host-tests.mk','-n',
                         'vfs-regression-test','network-regression-test',
                         'memory-regression-test','lifecycle-regression-test',
                         'wait-subscription-test','host-regression-test'],
                        cwd=root,text=True,capture_output=True,timeout=10)
assert combined.returncode==0,combined.stderr
assert combined.stdout.count('./build/wait-subscription-test.exe')==1
assert combined.stdout.count('tests/kernel/exec_load_buffer_test.py')==1
assert combined.stdout.count('tests/kernel/exec_metadata_read_test.py')==1
for relative in lifecycle_tests:
    assert combined.stdout.count(relative)==1,(relative,'combined goals repeat lifecycle regression')
for name in ('linux_proc_meminfo', 'user_fault_process_exit', 'exiting_user_fault',
             'kzalloc_initialization', 'slab_free_poison', 'surface_populate_batch',
             'process_clone_vm_pin', 'linux_process_clone'):
    relative=f'tests/kernel/{name}_test.py'
    assert result.stdout.count(relative)==1,(relative,'missing or repeated memory/exit regression')
    assert combined.stdout.count(relative)==1,(relative,'combined goals repeat memory/exit regression')
for test in vfs_tests:
    relative=test.relative_to(root).as_posix()
    assert combined.stdout.count(relative)==1,(relative,'combined goals repeat VFS tests')
for name in ('io_mutex_boot','block_cache_copy','block_cache_perf',
             'block_cache_reservation','block_cache_idle_ipi'):
    relative=f'tests/kernel/{name}_test.py'
    assert result.stdout.count(relative)==1,(relative,'filesystem support regression missing or repeated')
    assert combined.stdout.count(relative)==1,(relative,'combined goals repeat filesystem support regression')
for relative in network_tests:
    assert combined.stdout.count(relative)==1,(relative,'combined goals repeat network tests')
for relative in memory_tests:
    assert combined.stdout.count(relative)==1,(relative,'combined goals repeat memory tests')
missing='__entrypoint_missing_fixture__'
assert not (root/f'tests/kernel/{missing}_test.py').exists()
failed=subprocess.run(['make','-f','tools/host-tests.mk','network-regression-test',
                       f'HOST_NETWORK_REGRESSIONS={missing} socket_lookup_hint'],
                      cwd=root,text=True,capture_output=True,timeout=10)
assert failed.returncode!=0 and missing in failed.stderr,failed
assert 'NETWORK_REGRESSION tests/kernel/socket_lookup_hint_test.py' not in failed.stdout,failed
failed=subprocess.run(['make','-f','tools/host-tests.mk','memory-regression-test',
                       f'HOST_MEMORY_REGRESSIONS=tests/kernel/{missing}_test.py tests/kernel/page_alloc_fast_test.py'],
                      cwd=root,text=True,capture_output=True,timeout=10)
assert failed.returncode!=0 and missing in failed.stderr,failed
assert 'MEMORY_REGRESSION tests/kernel/page_alloc_fast_test.py' not in failed.stdout,failed
failed=subprocess.run(['make','-f','tools/host-tests.mk','lifecycle-regression-test',
                       '-o','wait-subscription-test',
                       f'HOST_LIFECYCLE_REGRESSIONS=tests/kernel/{missing}_test.py tests/kernel/wait_thread_pin_test.py'],
                      cwd=root,text=True,capture_output=True,timeout=10)
assert failed.returncode!=0 and missing in failed.stderr,failed
assert 'LIFECYCLE_REGRESSION tests/kernel/wait_thread_pin_test.py' not in failed.stdout,failed
for assignment,message in (('HOSTCC=','HOSTCC must not be empty'),('BUILD=','BUILD must not be empty')):
    result=subprocess.run([*base,assignment],cwd=root,text=True,capture_output=True,timeout=10)
    assert result.returncode!=0 and message in result.stderr,(assignment,result)
result=subprocess.run([*base,'HOSTCC=custom-compiler'],cwd=root,text=True,capture_output=True,timeout=10)
assert result.returncode==0 and 'custom-compiler -O2' in result.stdout,result
print('Host test entrypoint: standalone defaults, canonical output, overrides and empty-variable rejection passed')
print('Shared regressions: five cross-suite fixtures retain standalone coverage; both parallel goal orders have no duplicate expanded script paths')
print(f'Lifecycle entrypoint: all {len(lifecycle_tests)} termination/wait fixtures and real object waits run once; missing fixture fails closed')
print(f'VFS entrypoint: all {len(vfs_tests)} fixtures scheduled exactly once, including combined goals')
print(f'Network entrypoint: all {len(network_tests)} fixtures scheduled exactly once, including combined goals')
print('Network entrypoint: missing fixture fails immediately without running the next test')
print('Memory/exit entrypoint: meminfo and fatal-fault regressions scheduled exactly once')
print(f'Memory entrypoint: all {len(memory_tests)} allocator/VM fixtures exactly once, combined goals deduplicated and missing fixture fails closed')
