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
python
# One click only, four hardware addresses total. Host timings are diagnostic:
# hardware stops perturb execution. No input strings, URLs or buffers are read.
started = None
browser_pid = None
present_started = None
def current():
    cpu = gdb.selected_thread().num-1
    if not 0 <= cpu < int(gdb.parse_and_eval('g_cpu_count')):
        raise gdb.GdbError('Unexpected vCPU')
    return gdb.parse_and_eval('g_cpus[%d].queue.current' % cpu).dereference()
class Input(gdb.Breakpoint):
    def stop(self):
        global started
        if started is not None or int(gdb.parse_and_eval('$rdi')) == 0:
            return False
        event = gdb.parse_and_eval('(input_event_t*)$rdi').dereference()
        kind,code,value = (int(event[name]) for name in ('type','code','value'))
        if not ((kind == 5 and code & 1 and value & 1) or
                (kind == 2 and code == 272 and value == 1)):
            return False
        started = time.monotonic()
        print('INPUT_CORE_PRESS', flush=True)
        return False
class Press(gdb.Breakpoint):
    def stop(self):
        global browser_pid
        if started is None or browser_pid is not None:
            return False
        browser_pid = int(current()['process'].dereference()['pid'])
        print('OMNIBOX_PRESS diagnostic_host_ms=%.3f pid=%d' %
              ((time.monotonic()-started)*1000,browser_pid), flush=True)
        return False
class Returned(gdb.Breakpoint):
    def __init__(self, tid):
        super().__init__('*%#x' % int(gdb.parse_and_eval('*(unsigned long long*)$rsp')),
                         gdb.BP_HARDWARE_BREAKPOINT, internal=True)
        self.tid,self.done = tid,False
    def stop(self):
        if self.done or int(current()['tid']) != self.tid:
            return False
        self.done = True
        now = time.monotonic()
        print('PRESENT_RETURN diagnostic_host_ms=%.3f wait_and_compose_ms=%.3f status=%d' %
              ((now-started)*1000,(now-present_started)*1000,
               int(gdb.parse_and_eval('(long long)$rax'))), flush=True)
        gdb.post_event(lambda:gdb.execute('detach'))
        gdb.post_event(lambda:gdb.execute('quit'))
        return True
class Present(gdb.Breakpoint):
    def stop(self):
        global present_started
        if browser_pid is None or present_started is not None:
            return False
        if int(gdb.parse_and_eval('(process_t*)$rdi').dereference()['pid']) != browser_pid:
            return False
        present_started = time.monotonic()
        print('PRESENT_ENTER diagnostic_host_ms=%.3f' %
              ((present_started-started)*1000), flush=True)
        Returned(int(current()['tid']))
        return False
Input('input_core_push',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
Press('*0x4ad73980',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
Present('window_server_present',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
end
continue
