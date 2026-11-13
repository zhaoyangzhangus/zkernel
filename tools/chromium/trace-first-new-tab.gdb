set pagination off
set confirm off
python
import gdb, subprocess, struct, json, time, os, sys
from pathlib import Path
sys.path.insert(0, str(Path("tools/chromium").resolve()))
from trace_build import validate_chrome_build
expected = 'Build ID: 2ea4a62ae34e05c9'
try:
    validate_chrome_build(expected)
except RuntimeError as error:
    raise gdb.GdbError("Refusing stale Chrome diagnostic: " + str(error))
end
target remote /tmp/system-keys.sock
# 仅适用于 Chrome 构建 ID 2ea4a62ae34e05c9；地址来自该版本的 llvm-nm。
# 内核 ELF_PIE_BIAS=0x40000000。硬件断点会影响经过时间：
# 本脚本仅用于定位阶段，不可作为延迟基准。
python
import gdb, time, os

# 每个符号均通过受保护构建的 llvm-nm 解析。每次运行只选择一个嵌套阶段，
# 以确保不超过四个硬件断点槽位。
stage = os.environ.get('OS_NEW_TAB_STAGE', 'renderer')
stages = {
    'renderer': (0x480310d0, 'RENDERER_INIT'),
    'navigate': (0x4b35d3f0, 'NAVIGATE'),
    'tabstrip': (0x4a99ccd0, 'TABSTRIP_ADD'),
    'loadurl': (0x47f3fc30, 'LOAD_URL'),
    'contents': (0x4b35d630, 'CREATE_TARGET_CONTENTS'),
    'webcontents': (0x481fbce0, 'WEB_CONTENTS_CREATE'),
    'helpers': (0x520ccba0, 'ATTACH_TAB_HELPERS'),
}
if stage not in stages:
    raise gdb.GdbError('Unknown OS_NEW_TAB_STAGE')
stage_address, stage_label = stages[stage]

started = None
ui_tid = None
init_traced = False

def current_tid():
    # QEMU 的 GDB 线程号是从 1 开始的 vCPU 索引。读取当前暂停 CPU 的所有者；
    # 绝不能把其他线程的返回归到当前线程。
    cpu = gdb.selected_thread().num-1
    count = int(gdb.parse_and_eval('g_cpu_count'))
    if not 0 <= cpu < count:
        raise gdb.GdbError('Unexpected QEMU vCPU thread numbering')
    return int(gdb.parse_and_eval('g_cpus[%d].queue.current->tid' % cpu))

class NewTabReturn(gdb.Breakpoint):
    def __init__(self, address):
        super().__init__('*%#x' % address, gdb.BP_HARDWARE_BREAKPOINT,
                         temporary=True, internal=True)
    def stop(self):
        if current_tid() != ui_tid:
            return False
        print('NEW_TAB_RETURN diagnostic_host_ms=%.3f' %
              ((time.monotonic()-started)*1000), flush=True)
        gdb.post_event(lambda: gdb.execute('detach'))
        gdb.post_event(lambda: gdb.execute('quit'))
        return True

class NewTabEntry(gdb.Breakpoint):
    def stop(self):
        global started, ui_tid
        started = time.monotonic()
        ui_tid = current_tid()
        print('NEW_TAB_ENTER tid=%d' % ui_tid, flush=True)
        return_address = int(gdb.parse_and_eval('*(unsigned long long*)$rsp'))
        # 硬件地址最多保留四个，包括 GDB 移除临时入口断点前的那一个。
        # 不要在 stop() 内删除断点。
        NewTabReturn(return_address)
        return False

class AddTabStage(gdb.Breakpoint):
    def __init__(self, address, label):
        super().__init__('*%#x' % address, gdb.BP_HARDWARE_BREAKPOINT,
                         internal=True)
        self.label = label
    def stop(self):
        if started is not None:
            print('%s diagnostic_host_ms=%.3f' %
                  (self.label, (time.monotonic()-started)*1000), flush=True)
        return False

class RendererInitReturn(gdb.Breakpoint):
    def __init__(self, address, entered):
        super().__init__('*%#x' % address, gdb.BP_HARDWARE_BREAKPOINT,
                         temporary=True, internal=True)
        self.entered = entered
        self.done = False
    def stop(self):
        if self.done or current_tid() != ui_tid:
            return False
        self.done = True
        print('%s_EXIT diagnostic_host_duration_ms=%.3f' %
              (stage_label, (time.monotonic()-self.entered)*1000), flush=True)
        # 在 NewTab 返回前保留无效地址；绝不在 stop() 内修改断点。
        # done 标志会忽略之后无关的返回。
        return False

class RendererInitEntry(gdb.Breakpoint):
    def stop(self):
        global init_traced
        if started is not None and not init_traced and current_tid() == ui_tid:
            init_traced = True
            entered = time.monotonic()
            print('%s_ENTER diagnostic_host_ms=%.3f' %
                  (stage_label, (entered-started)*1000), flush=True)
            RendererInitReturn(int(gdb.parse_and_eval(
                '*(unsigned long long*)$rsp')), entered)
        return False

NewTabEntry('*0x520f0100', gdb.BP_HARDWARE_BREAKPOINT,
            temporary=True, internal=True)
RendererInitEntry('*%#x' % stage_address, gdb.BP_HARDWARE_BREAKPOINT, internal=True)
end
continue
