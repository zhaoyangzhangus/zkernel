set pagination off
set confirm off
target remote /tmp/system-keys.sock
python
import gdb
import time

class ReclaimReturn(gdb.Breakpoint):
    def __init__(self, address, start):
        super().__init__('*%#x' % address, gdb.BP_HARDWARE_BREAKPOINT,
                         temporary=True, internal=True)
        self.start = start

    def stop(self):
        elapsed = time.monotonic() - self.start
        print('EXEC_RECLAIM_RETURN host_elapsed_seconds=%.6f' % elapsed,
              flush=True)
        gdb.post_event(lambda: gdb.execute('detach'))
        gdb.post_event(lambda: gdb.execute('quit'))
        return True

class ReclaimStart(gdb.Breakpoint):
    def __init__(self):
        super().__init__('*exec_release_old_space_work',
                         gdb.BP_HARDWARE_BREAKPOINT, temporary=True,
                         internal=True)

    def stop(self):
        # 函数入口处：x86-64 SysV 的第一个参数和返回地址。
        space = int(gdb.parse_and_eval('$rdi')) & ((1 << 64) - 1)
        address = int(gdb.parse_and_eval('*(unsigned long*)$rsp'))
        print('EXEC_RECLAIM_ENTER space=%#x' % space, flush=True)
        ReclaimReturn(address, time.monotonic())
        return False

ReclaimStart()
end
continue
