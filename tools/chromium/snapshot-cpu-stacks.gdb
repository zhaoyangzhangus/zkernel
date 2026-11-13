set pagination off
target remote /tmp/system-keys.sock
set $cpu_registry_address = &g_cpus
set $kernel_wait_entry = &futex_wait_keyed
set $kernel_wake_entry = &sched_wake
set $kernel_ready_entry = &scheduler_complete_wake_local
add-symbol-file build/scheduler-inspection-types.o 0
add-symbol-file build/process-inspection-types.o 0
python
import os
if os.environ.get('OS_CAPTURE_ALLOCATOR_WAIT'):
    gdb.execute('break *$kernel_wait_entry')
    # SysV 入口参数：进程、用户地址。规范启动中的浏览器 PID 保持稳定；重建后必须重新
    # 验证锁地址。
    gdb.execute('condition $bpnum $rsi == 0x537ecbc0 && ((process_t*)$rdi)->pid == 125')
    gdb.execute('continue')
if os.environ.get('OS_CAPTURE_ALLOCATOR_WAKE'):
    gdb.execute('break *$kernel_wake_entry')
    gdb.execute('condition $bpnum ((thread_t*)$rdi)->tid == 126 && ((unsigned long long*)(((thread_t*)$rdi)->kernel_stack_top-176))[14] == 202 && ((unsigned long long*)(((thread_t*)$rdi)->kernel_stack_top-176))[9] == 0x537ecbc0')
    gdb.execute('continue')
if os.environ.get('OS_CAPTURE_ALLOCATOR_READY'):
    gdb.execute('break *$kernel_ready_entry')
    gdb.execute('condition $bpnum ((thread_t*)$rsi)->tid == 126')
    gdb.execute('continue')
    if int(gdb.parse_and_eval('$rip')) != int(gdb.parse_and_eval('$kernel_ready_entry')):
        print('ALLOCATOR_READY_CAPTURE_MISSED: bounded wait ended without matching breakpoint')
        gdb.execute('detach')
        gdb.execute('quit')
end
python
import gdb
def cast(address, kind):
    return gdb.Value(address).cast(gdb.lookup_type(kind).pointer()).dereference()
def user_word(process, address):
    table = int(process['vm'].dereference()['root_table']['value'])
    mask = 0x000ffffffffff000
    for shift in (39, 30, 21, 12):
        pte = int.from_bytes(bytes(gdb.selected_inferior().read_memory(
            0xffff800000000000 + table + ((address >> shift) & 511) * 8, 8)), 'little')
        if not pte & 1:
            raise gdb.MemoryError('unmapped user frame')
        if shift in (30, 21) and pte & 128:
            physical = (pte & mask & ~((1 << shift) - 1)) + (address & ((1 << shift) - 1))
            break
        table = pte & mask
    else:
        physical = table + (address & 4095)
    return int.from_bytes(bytes(gdb.selected_inferior().read_memory(
        0xffff800000000000 + physical, 8)), 'little')
cpus = gdb.Value(int(gdb.parse_and_eval('$cpu_registry_address'))).cast(
    gdb.lookup_type('scheduler_cpu_t').pointer())
if os.environ.get('OS_CAPTURE_ALLOCATOR_WAKE') or os.environ.get('OS_CAPTURE_ALLOCATOR_READY'):
    waking = cast(int(gdb.parse_and_eval('$rsi' if os.environ.get('OS_CAPTURE_ALLOCATOR_READY') else '$rdi')), 'thread_t')
    print('ROOT_WAKE tid', int(waking['tid']), 'owner_cpu',
          int(waking['owner_cpu']), 'vruntime', int(waking['sched']['vruntime']))
    saved = bytes(gdb.selected_inferior().read_memory(int(waking['kernel_stack_top'])-176,176))
    values = [int.from_bytes(saved[i:i+8], 'little') for i in range(0,176,8)]
    print('WAKING_TRAP syscall', values[14], 'arg0', hex(values[9]),
          'arg1', hex(values[8]), 'rip', hex(values[17]))
    for index in range(4):
        queue = cpus[index]['queue']
        current = queue['current']
        print('QUEUE cpu', index, 'floor', int(queue['min_vruntime']),
              'running', int(queue['nr_running']), 'rt_bitmap', int(queue['rt_bitmap']),
              'current_tid', int(current['tid']) if int(current) else 0,
              'current_vruntime', int(current['sched']['vruntime']) if int(current) else 0)
        def visit(node, depth=0):
            if not int(node) or depth > 24: return
            visit(node['left'], depth+1)
            offset = int(gdb.parse_and_eval('(unsigned long)&((thread_t*)0)->sched.fair_node'))
            entity = cast(int(node)-offset, 'thread_t')
            print('FAIR cpu', index, 'tid', int(entity['tid']),
                  'vruntime', int(entity['sched']['vruntime']))
            visit(node['right'], depth+1)
        visit(queue['fair_root']['root'])
for thread in gdb.selected_inferior().threads():
    thread.switch()
    print('CPU', thread.num, 'rip', gdb.parse_and_eval('$rip'),
          'rsp', gdb.parse_and_eval('$rsp'))
    if thread.num == 1:
        local = int(gdb.parse_and_eval('&syscall_cpu_local'))
        raw = bytes(gdb.selected_inferior().read_memory(local+72, 12))
        print('BSP_FLAGS preempt_disable', int.from_bytes(raw[:4], 'little'),
              'syscall_active', int.from_bytes(raw[8:12], 'little'))
    gdb.execute('bt 12')
    try:
        gdb.execute('x/64ag $rsp')
    except gdb.error:
        pass  # An idle stack can end before this bounded dump.
    current = cpus[thread.num - 1]['queue']['current']
    if int(current) and int(current['process']):
        process = current['process'].dereference()
        print('CURRENT pid', int(process['pid']), 'tid', int(current['tid']))
        top = int(current['kernel_stack_top'])
        # arch_trap_frame_t 包含 22 个 uint64_t 字段（见 context.h）。检查对象可能省略
        # 此未使用类型，因此直接按固定 ABI 解码。
        frame_address = top - 22 * 8
        raw = bytes(gdb.selected_inferior().read_memory(frame_address, 22 * 8))
        trap = [int.from_bytes(raw[i:i+8], 'little') for i in range(0, len(raw), 8)]
        print('TRAP_ARGS rax', hex(trap[14]), 'rdi', hex(trap[9]),
              'rsi', hex(trap[8]), 'rdx', hex(trap[11]))
        if trap[18] == 0x23:
            print('USER_TRAP rip', hex(trap[17]), 'rsp', hex(trap[20]))
            user_frame = trap[10]
            for depth in range(20):
                if not 0x10000 <= user_frame < 0x800000000000:
                    break
                try:
                    next_frame = user_word(process, user_frame)
                    ret = user_word(process, user_frame + 8)
                except gdb.MemoryError:
                    break
                print('SAVED_USER_FRAME', depth, hex(user_frame), hex(ret))
                if next_frame <= user_frame or next_frame - user_frame > 0x100000:
                    break
                user_frame = next_frame
    frame = int(gdb.parse_and_eval('$rbp'))
    for depth in range(20):
        if not 0x10000 <= frame < 0x800000000000:
            break
        try:
            data = bytes(gdb.selected_inferior().read_memory(frame, 16))
        except gdb.MemoryError:
            break
        next_frame = int.from_bytes(data[:8], 'little')
        ret = int.from_bytes(data[8:], 'little')
        print('USER_FRAME', depth, hex(frame), hex(ret))
        if next_frame <= frame or next_frame - frame > 0x100000:
            break
        frame = next_frame
end
detach
quit
