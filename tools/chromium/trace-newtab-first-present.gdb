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
# Localization only: hardware stops perturb elapsed time. At most four
# addresses (two entries and their returns); never delete BPs in stop().
started = None
input_kind = os.environ.get('OS_INPUT_TRACE', 'newtab')
if input_kind not in ('newtab','address','addressrelease'):
    raise gdb.GdbError('Unknown input trace kind')
input_label = {'newtab':'NEW_TAB','address':'OMNIBOX_PRESS',
               'addressrelease':'OMNIBOX_RELEASE'}[input_kind]
ui_tid = None
browser_pid = None
tab_done = False
present_done = False
present_started = None

def current():
    cpu = gdb.selected_thread().num-1
    if not 0 <= cpu < int(gdb.parse_and_eval('g_cpu_count')):
        raise gdb.GdbError('Unexpected vCPU number')
    return gdb.parse_and_eval('g_cpus[%d].queue.current' % cpu).dereference()

def finish_if_done():
    if tab_done and present_done:
        gdb.post_event(lambda: gdb.execute('detach'))
        gdb.post_event(lambda: gdb.execute('quit'))
        return True
    return False

class Return(gdb.Breakpoint):
    def __init__(self, kind, tid):
        super().__init__('*%#x' % int(gdb.parse_and_eval('*(unsigned long long*)$rsp')),
                         gdb.BP_HARDWARE_BREAKPOINT, internal=True)
        self.kind, self.tid, self.done = kind, tid, False
    def stop(self):
        global tab_done, present_done
        if self.done or int(current()['tid']) != self.tid:
            return False
        self.done = True
        now = time.monotonic()
        if self.kind == 'TAB':
            tab_done = True
            print('%s_RETURN diagnostic_host_ms=%.3f' % (input_label,(now-started)*1000), flush=True)
        else:
            present_done = True
            print('FIRST_PRESENT_RETURN diagnostic_host_ms=%.3f wait_and_compose_ms=%.3f status=%d' %
                  ((now-started)*1000, (now-present_started)*1000,
                   int(gdb.parse_and_eval('(long long)$rax'))), flush=True)
        return finish_if_done()

class TabEntry(gdb.Breakpoint):
    def stop(self):
        global started, ui_tid, browser_pid
        if started is not None:
            return False
        thread = current()
        ui_tid = int(thread['tid'])
        browser_pid = int(thread['process'].dereference()['pid'])
        started = time.monotonic()
        print('%s_ENTER tid=%d pid=%d' % (input_label,ui_tid,browser_pid), flush=True)
        Return('TAB', ui_tid)
        return False

class PresentEntry(gdb.Breakpoint):
    def stop(self):
        global present_started
        if started is None or present_started is not None:
            return False
        owner = gdb.parse_and_eval('(process_t*)$rdi').dereference()
        if int(owner['pid']) != browser_pid:
            return False
        present_started = time.monotonic()
        print('FIRST_PRESENT_ENTER diagnostic_host_ms=%.3f damage=%d,%d %ux%u' %
              ((present_started-started)*1000,
               int(gdb.parse_and_eval('damage_x')), int(gdb.parse_and_eval('damage_y')),
               int(gdb.parse_and_eval('damage_width')), int(gdb.parse_and_eval('damage_height'))), flush=True)
        Return('PRESENT', int(current()['tid']))
        return False

TabEntry('*%#x' % {'newtab':0x520f0100,'address':0x4ad73980,
                   'addressrelease':0x4ad73f80}[input_kind],
         gdb.BP_HARDWARE_BREAKPOINT, internal=True)
PresentEntry('window_server_present', gdb.BP_HARDWARE_BREAKPOINT, internal=True)
end
continue
