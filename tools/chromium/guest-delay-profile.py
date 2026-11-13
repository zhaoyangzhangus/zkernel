"""Bounded profiling of the current browser in the sole canonical QEMU."""
import argparse
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
parser = argparse.ArgumentParser()
parser.add_argument('action', choices=('begin', 'read', 'end'))
parser.add_argument('--tid', type=int, default=0,
                    help='For begin only: select a verified browser thread instead of its main thread')
parser.add_argument('--wake-threshold-ms', type=int, default=10,
                    help='For begin: opt-in scheduler diagnostic threshold (1..1000ms, default 10)')
parser.add_argument('--vm-lock-threshold-ms', type=int, default=10,
                    help='For begin: opt-in VM lock sampling threshold (1..1000ms, default 10)')
args = parser.parse_args()
if args.tid < 0 or (args.tid and args.action != 'begin'):
    parser.error('--tid must be positive and used with begin')
if not 1 <= args.wake_threshold_ms <= 1000 or (args.wake_threshold_ms != 10 and args.action != 'begin'):
    parser.error('--wake-threshold-ms must be 1..1000 and used with begin')
if not 1 <= args.vm_lock_threshold_ms <= 1000 or (args.vm_lock_threshold_ms != 10 and args.action != 'begin'):
    parser.error('--vm-lock-threshold-ms must be 1..1000 and used with begin')
subprocess.run(['cc','-g3','-fno-eliminate-unused-debug-types','-ffreestanding',
                '-DOS_KERNEL_BUILD','-Iinclude','-c',
                'tools/chromium/inspection-types.c','-o','build/current-inspection-types.o'],
               cwd=root, check=True)
code = r'''
import gdb,json,struct,time
inferior=gdb.selected_inferior()
def address(name):return int(gdb.parse_and_eval('&'+name))
def cast(p,name):return gdb.Value(p).cast(gdb.lookup_type(name).pointer()).dereference()
def write(name,value):inferior.write_memory(address(name),struct.pack('<Q',value))
def raw(name,count):return struct.unpack('<'+'Q'*count,bytes(inferior.read_memory(address(name),count*8)))
def select_profile_thread(process_address,requested,leader):
    if not requested:return leader
    head=process_address+int(gdb.parse_and_eval('(unsigned long)&((process_t*)0)->threads'))
    offset=int(gdb.parse_and_eval('(unsigned long)&((thread_t*)0)->process_node'))
    node=int(cast(head,'list_head_t')['next']);seen=set()
    while node!=head and node not in seen and len(seen)<4096:
        seen.add(node);thread=cast(node-offset,'thread_t')
        if int(thread['tid'])==requested:
            if int(thread['process'])!=process_address:
                raise gdb.GdbError('Selected thread has a different process owner')
            return requested
        node=int(thread['process_node']['next'])
    raise gdb.GdbError('Selected thread is not a live member of the current browser')
selectors=('g_syscall_delay_process','g_vm_slow_fault_process','g_wait_delay_tid',
           'g_sched_wake_trace_tid','g_exec_load_timing_enabled','g_syscall_cost_tid')
sets=(('g_syscall_delays','g_syscall_delay_guard',64,7),
      ('g_vm_slow_faults','g_vm_slow_fault_guard',32,7),
      ('g_sched_wake_delays','g_sched_wake_trace_guard',16,25),
      ('g_wait_delays','g_timeout_lock',32,5),
      ('g_exec_load_delays','g_exec_load_delay_guard',64,6),
      ('g_syscall_costs','g_syscall_cost_guard',128,4))
action=ACTION
requested_tid=REQUESTED_TID
wake_threshold_ns=WAKE_THRESHOLD_NS
vm_lock_threshold_ns=VM_LOCK_THRESHOLD_NS
if action != 'end':
    if int(gdb.parse_and_eval('sizeof(g_sched_wake_trace_threshold_ns)')) != 8:
        raise gdb.GdbError('Running-kernel wake threshold layout mismatch')
    if int(gdb.parse_and_eval('sizeof(g_vm_lock_trace_threshold_ns)')) != 8:
        raise gdb.GdbError('Running-kernel VM lock threshold layout mismatch')
    for records,guard,rows,columns in sets:
        actual=int(gdb.parse_and_eval('sizeof('+records+')'))
        if actual != rows*columns*8:
            raise gdb.GdbError('Running-kernel record layout mismatch: '+records)
    if int(gdb.parse_and_eval('sizeof(g_sched_wake_trace)')) != 22*8:
        raise gdb.GdbError('Running-kernel active wake-trace layout mismatch')
    if int(gdb.parse_and_eval('sizeof(g_sched_wake_stall)')) != 14*8:
        raise gdb.GdbError('Running-kernel wake-stall layout mismatch')
    if int(gdb.parse_and_eval('sizeof(g_vm_lock_diagnostics)')) != 8*8:
        raise gdb.GdbError('Running-kernel VM lock diagnostic layout mismatch')
    for name,columns in (('g_sched_inbox_stall',17),('g_sched_ipi_send',7)):
        if int(gdb.parse_and_eval('sizeof('+name+')')) != columns*8:
            raise gdb.GdbError('Running-kernel diagnostic layout mismatch: '+name)
if action=='begin':
    if any(raw(selector,1)[0] for selector in selectors):
        raise gdb.GdbError('Another profile is active; do not overwrite its selectors')
    registry=address('g_process_registry')
    offset=int(gdb.parse_and_eval('(unsigned long)&((process_t*)0)->global_node'))
    node=int(cast(registry,'list_head_t')['next']);seen=set();browsers=[]
    while node!=registry and node not in seen and len(seen)<256:
        seen.add(node);p=cast(node-offset,'process_t')
        name=p['name'].string()
        if name.rsplit('/',1)[-1]=='chrome':
            browsers.append((int(p['pid']),int(p['linux_thread_group_leader_tid']),int(p['thread_count']),node-offset))
        node=int(p['global_node']['next'])
    if not browsers:raise gdb.GdbError('No current Chrome process')
    pid,leader,count,process_address=min(browsers)
    tid=select_profile_thread(process_address,requested_tid,leader)
    if not tid or count<2:raise gdb.GdbError('Browser identity not established')
    for records,guard,rows,columns in sets:
        if bytes(inferior.read_memory(address(guard),1))!=b'\x00':
            raise gdb.GdbError('Publisher owns '+guard+'; resume and retry, do not reset')
    if bytes(inferior.read_memory(address('g_vm_lock_diagnostic_guard'),1))!=b'\x00':
        raise gdb.GdbError('Publisher owns VM lock diagnostics; resume and retry')
    for records,guard,rows,columns in sets:
        inferior.write_memory(address(records),bytes(rows*columns*8))
    inferior.write_memory(address('g_sched_wake_trace'),bytes(22*8))
    inferior.write_memory(address('g_sched_wake_stall'),bytes(14*8))
    inferior.write_memory(address('g_sched_inbox_stall'),bytes(17*8))
    inferior.write_memory(address('g_sched_ipi_send'),bytes(7*8))
    # 不要将启动阶段/更早时间窗中的最大值与本次有界测试混在一起。
    inferior.write_memory(address('g_vm_lock_diagnostics'),bytes(8*8))
    inferior.write_memory(address('g_sched_preempt_callers'),
                          bytes(int(gdb.parse_and_eval('sizeof(g_sched_preempt_callers)'))))
    write('g_syscall_cost_dropped',0)
    write('g_sched_wake_trace_threshold_ns',wake_threshold_ns)
    write('g_vm_lock_trace_threshold_ns',vm_lock_threshold_ns)
    for selector,value in zip(selectors,(pid,pid,tid,tid,1,tid)):write(selector,value)
    print('PROFILE '+json.dumps({'pid':pid,'main_tid':leader,'selected_tid':tid,'threads':count,
                                'wake_threshold_ns':wake_threshold_ns,
                                'vm_lock_threshold_ns':vm_lock_threshold_ns,'action':action}))
elif action=='end':
    for selector in selectors:write(selector,0)
    write('g_sched_wake_trace_threshold_ns',10000000)
    write('g_vm_lock_trace_threshold_ns',10000000)
    print('PROFILE '+json.dumps({'action':action,'selectors_disabled':True,
                                'wake_threshold_ns':raw('g_sched_wake_trace_threshold_ns',1)[0],
                                'vm_lock_threshold_ns':raw('g_vm_lock_trace_threshold_ns',1)[0]}))
else:
    result={'action':action,'selected_process':raw(selectors[0],1)[0]}
    result['host_snapshot_ns']=time.monotonic_ns()
    result['selected_tid']=raw('g_syscall_cost_tid',1)[0]
    result['wake_threshold_ns']=raw('g_sched_wake_trace_threshold_ns',1)[0]
    result['vm_lock_threshold_ns']=raw('g_vm_lock_trace_threshold_ns',1)[0]
    result['syscall_cost_scope']='Selected browser thread, cumulative syscall wall time including waits; not CPU time'
    result['syscall_cost_dropped']=raw('g_syscall_cost_dropped',1)[0]
    result['lapic_ticks']=int(gdb.parse_and_eval('g_tick_count'))
    result['cpu_clock_ns']=[int(gdb.parse_and_eval('g_cpus[%d].queue.clock_ns'%cpu))
                            for cpu in range(int(gdb.parse_and_eval('g_cpu_count')))]
    # 仅快照已暂停的客户机；绝不消费 FIFO 或修改引用。
    # CPU 若在发布过程中暂停，可能会短暂显示 head/pending 不匹配。
    result['command_release_queues']=[
        {'cpu':cpu,
         'head':int(gdb.parse_and_eval('g_cpus[%d].command_release_head'%cpu)),
         'pending':bool(gdb.parse_and_eval('g_cpus[%d].command_release_pending'%cpu))}
        for cpu in range(int(gdb.parse_and_eval('g_cpu_count')))]
    if bytes(inferior.read_memory(address('g_sched_wake_trace_guard'),1))==b'\x00':
        result['g_sched_wake_stall']=raw('g_sched_wake_stall',14)
        result['g_sched_inbox_stall']=raw('g_sched_inbox_stall',17)
        result['g_sched_ipi_send']=raw('g_sched_ipi_send',7)
    if bytes(inferior.read_memory(address('g_vm_lock_diagnostic_guard'),1))==b'\x00':
        result['g_vm_lock_diagnostics']=raw('g_vm_lock_diagnostics',8)
    for records,guard,rows,columns in sets:
        if bytes(inferior.read_memory(address(guard),1))!=b'\x00':
            result[records]={'publisher_busy':True};continue
        data=raw(records,rows*columns)
        result[records]=[data[i*columns:(i+1)*columns] for i in range(rows)
                         if data[i*columns+ (1 if records in (
                             'g_syscall_delays','g_vm_slow_faults','g_exec_load_delays',
                             'g_syscall_costs') else 0)]]
    print('PROFILE '+json.dumps(result))
'''.replace('ACTION', repr(args.action)).replace('REQUESTED_TID', str(args.tid)).replace('WAKE_THRESHOLD_NS', str(args.wake_threshold_ms*1000000)).replace('VM_LOCK_THRESHOLD_NS', str(args.vm_lock_threshold_ms*1000000))
qmp = QMPClient()
def command(name, arguments=None):
    return qmp.command(name, arguments)

assert command('query-status')['running'], 'Guest already stopped; leave it untouched'
command('human-monitor-command',{'command-line':'gdbserver unix:/tmp/system-profile.sock,server=on,wait=off'})
try:
    result = subprocess.run(['gdb','-q','-nx','-batch',str(root/'build/qemu-running-kernel.elf'),
                    '-ex','set pagination off','-ex','target remote /tmp/system-profile.sock',
                    '-ex','add-symbol-file build/current-inspection-types.o 0',
                    '-ex','python exec('+repr(code)+')','-ex','detach'],
                   cwd=root,check=True,timeout=10,text=True,capture_output=True)
    print(result.stdout, end='')
    if result.stderr:
        print(result.stderr, end='')
    reports = [json.loads(line[len('PROFILE '):]) for line in result.stdout.splitlines()
               if line.startswith('PROFILE ')]
    if len(reports) != 1 or reports[0].get('action') != args.action:
        raise RuntimeError('Missing authoritative profile result; GDB exit alone is not success')
    if args.action == 'read':
        (root/'build/chrome-last-delay-profile.json').write_text(
            json.dumps(reports[0],indent=2)+'\n')
finally:
    command('human-monitor-command',{'command-line':'gdbserver none'})
    command('cont')
    assert command('query-status')['running'], 'Guest did not resume after profile'
