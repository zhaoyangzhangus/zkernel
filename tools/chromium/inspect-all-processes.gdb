set pagination off
target remote /tmp/system-processes.sock
add-symbol-file build/current-inspection-types.o 0
python
import gdb
import os
import subprocess
import sys
from pathlib import Path
executable_ranges = []
if os.environ.get('OS_INSPECT_CHROME_STACK_SCAN') == '1':
    sys.path.insert(0, str(Path('tools/chromium').resolve()))
    from trace_build import validate_chrome_symbols
    binary = validate_chrome_symbols()
    for row in subprocess.check_output(['readelf', '-lW', str(binary)], text=True).splitlines():
        fields = row.split()
        if fields and fields[0] == 'LOAD' and 'E' in fields[6:-1]:
            start = 0x40000000 + int(fields[2], 16)
            executable_ranges.append((start, start + int(fields[5], 16)))
    if not executable_ranges:
        raise gdb.GdbError('No verified Chrome executable LOAD range')
def cast(address, name):
    return gdb.Value(address).cast(gdb.lookup_type(name).pointer()).dereference()
def offset(name, field):
    return int(gdb.parse_and_eval('(unsigned long)&((%s*)0)->%s' % (name, field)))
def user_word(process, address):
    table = int(process['vm'].dereference()['root_table']['value'])
    mask = 0x000ffffffffff000
    inferior = gdb.selected_inferior()
    for shift in (39,30,21,12):
        pte = int.from_bytes(bytes(inferior.read_memory(
            0xffff800000000000+table+((address>>shift)&511)*8,8)), 'little')
        if not pte&1:
            raise ValueError('unmapped user page')
        if shift in (30,21) and pte&128:
            physical=(pte&mask&~((1<<shift)-1))+(address&((1<<shift)-1))
            break
        table=pte&mask
    else:
        physical=table+(address&4095)
    return int.from_bytes(bytes(inferior.read_memory(0xffff800000000000+physical,8)), 'little')
registry = int(gdb.parse_and_eval('&g_process_registry'))
node = int(cast(registry, 'list_head_t')['next'])
seen = set()
while node != registry and node not in seen and len(seen) < 256:
    seen.add(node)
    process = cast(node-offset('process_t','global_node'), 'process_t')
    print('PROCESS pid=%d name=%s threads=%d' %
          (int(process['pid']),process['name'].string(),int(process['thread_count'])))
    if os.environ.get('OS_INSPECT_FD_COUNTS') == '1':
        table = process['linux_fds']
        counts = {}
        entries = table['entries']
        low, high = entries.type.range()
        for index in range(low, high + 1):
            descriptor = entries[index]
            kind = int(descriptor['kind'])
            if kind:
                counts[kind] = counts.get(kind, 0) + 1
        print(' FD_COUNTS used=%d kinds=%s' % (sum(counts.values()), counts))
        if os.environ.get('OS_INSPECT_FD_FILES') == '1' and process['name'].string() == 'chrome':
            for index in range(low, high + 1):
                descriptor = entries[index]
                if int(descriptor['file']):
                    file = descriptor['file'].dereference()
                    if int(file['vnode']):
                        vnode = file['vnode'].dereference()
                        print(' FD_FILE fd=%d vnode=%#x inode=%d size=%d ops=%#x' % (
                            index, int(file['vnode']), int(vnode['inode_id']),
                            int(vnode['size']), int(vnode['ops'])))
                        if int(vnode['fs_private']) and int(vnode['ops']) == 0:
                            print('  VFS_PATH ' + vnode['fs_private'].cast(
                                gdb.lookup_type('char').pointer()).string(length=256).split('\0')[0])
    head = int(process['threads'].address)
    link = int(process['threads']['next'])
    threads = set()
    while link != head and link not in threads and len(threads) < 256:
        threads.add(link)
        thread = cast(link-offset('thread_t','process_node'), 'thread_t')
        print(' tid=%d state=%d cpu=%d waiter=%#x' %
              (int(thread['tid']),int(thread['state']),int(thread['owner_cpu']),int(thread['blocked_waiter'])))
        frame = cast(int(thread['kernel_stack_top'])-gdb.lookup_type('arch_trap_frame_t').sizeof,
                     'arch_trap_frame_t')
        if int(frame['cs']) == 0x23:
            print(' frame vector=%#x syscall=%d rip=%#x rbp=%#x args=%s' %
                  (int(frame['vector']),int(frame['rax']),int(frame['rip']),int(frame['rbp']),
                   ','.join('%#x'%int(frame[k]) for k in ('rdi','rsi','rdx','r10','r8','r9'))))
            if executable_ranges and process['name'].string() == 'chrome' and int(frame['rax']) == 202:
                # 仅列出候选项，不展开调用栈。不要打印任意栈字节，其中可能包含用户文本。
                for stack_offset in range(0, 1024, 8):
                    try:
                        value = user_word(process, int(frame['rsp']) + stack_offset)
                        if any(start <= value < end for start, end in executable_ranges):
                            print(' executable_stack_candidate offset=%d address=%#x' % (stack_offset, value))
                    except (ValueError, gdb.error):
                        break
            if process['name'].string() == 'libc-test' or (
                    os.environ.get('OS_INSPECT_CHROME_STACKS') == '1' and
                    process['name'].string() == 'chrome'):
                try:
                    if int(frame['rax']) == 202:
                        print(' wait_word=%#x' % user_word(process,int(frame['rdi'])))
                    fp=int(frame['rbp'])
                    for depth in range(12):
                        print(' user_return=%#x' % user_word(process,fp+8))
                        next_fp=user_word(process,fp)
                        if next_fp<=fp:
                            break
                        fp=next_fp
                except (ValueError,gdb.error):
                    pass
        link = int(thread['process_node']['next'])
    node = int(process['global_node']['next'])
end
detach
quit
