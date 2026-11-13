set pagination off
set confirm off
target remote /tmp/system-exit.sock
add-symbol-file build/current-inspection-types.o 0
python
import gdb

cpus = gdb.Value(int(gdb.parse_and_eval('&g_cpus'))).cast(
    gdb.lookup_type('scheduler_cpu_t').pointer())

def user_word(process, address):
    table = int(process['vm'].dereference()['root_table']['value'])
    mask = 0x000ffffffffff000
    memory = gdb.selected_inferior()
    for shift in (39, 30, 21, 12):
        entry = int.from_bytes(bytes(memory.read_memory(
            0xffff800000000000 + table + ((address >> shift) & 511) * 8, 8)), 'little')
        if entry & 5 != 5:
            raise ValueError('Unmapped user stack')
        if shift in (30, 21) and entry & 128:
            physical = (entry & mask & ~((1 << shift) - 1)) + (address & ((1 << shift) - 1))
            break
        table = entry & mask
    else:
        physical = table + (address & 4095)
    return int.from_bytes(bytes(memory.read_memory(
        0xffff800000000000 + physical, 8)), 'little')

class ThreadExit(gdb.Breakpoint):
    count = 0
    def stop(self):
        current = cpus[gdb.selected_thread().num - 1]['queue']['current']
        if not int(current) or not int(current['process']):
            return False
        process = current['process'].dereference()
        if process['name'].string() != 'chrome':
            return False
        self.count += 1
        frame = gdb.Value(int(current['kernel_stack_top']) -
            gdb.lookup_type('arch_trap_frame_t').sizeof).cast(
                gdb.lookup_type('arch_trap_frame_t').pointer()).dereference()
        print('CHROME_THREAD_EXIT pid=%d tid=%d status=%d syscall=%d user_pc=%#x' % (
            int(process['pid']), int(current['tid']), int(gdb.parse_and_eval('$rdi')),
            int(frame['rax']), int(frame['rip'])), flush=True)
        # glibc 不保留帧指针。栈字只是候选地址，不是栈回溯：只打印可执行区域中的地址，
        # 绝不打印用户文本。
        for offset in range(0, 512, 8):
            try:
                value = user_word(process, int(frame['rsp']) + offset)
                if 0x40000000 <= value < 0x60000000:
                    print(' chrome_stack_candidate offset=%d address=%#x' % (offset, value), flush=True)
            except (ValueError, gdb.error):
                break
        if self.count >= 32:
            self.enabled = False
        return False

ThreadExit('*%#x' % int(gdb.parse_and_eval('&thread_exit')),
           type=gdb.BP_HARDWARE_BREAKPOINT)
print('CHROME_THREAD_EXIT_TRACE_READY', flush=True)
end
continue
detach
quit
