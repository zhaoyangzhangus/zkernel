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
# Four hardware addresses; first browser present can include earlier damage.
# This localizes stages, not exact UI latency: hardware stops perturb timing.
started=None
owner=None
browser_pid=None
tab_done=False
present_done=False
def current():
    cpu=gdb.selected_thread().num-1
    if not 0<=cpu<int(gdb.parse_and_eval('g_cpu_count')):
        raise gdb.GdbError('Unexpected vCPU')
    return gdb.parse_and_eval('g_cpus[%d].queue.current'%cpu).dereference()
def finish():
    if not(tab_done and present_done):return False
    gdb.post_event(lambda:gdb.execute('detach'))
    gdb.post_event(lambda:gdb.execute('quit'))
    return True
class Input(gdb.Breakpoint):
    def stop(self):
        global started
        if started is not None or int(gdb.parse_and_eval('$rdi'))==0:return False
        event=gdb.parse_and_eval('(input_event_t*)$rdi').dereference()
        # USB HID keyboard T usage, not an evdev code. No text is inspected.
        if int(event['type'])!=1 or int(event['code'])!=0x17 or int(event['value'])!=1:return False
        started=time.monotonic()
        print('INPUT_CORE_T_PRESS',flush=True)
        return False
class Returned(gdb.Breakpoint):
    def stop(self):
        global tab_done
        if tab_done or int(current()['tid'])!=owner:return False
        tab_done=True
        print('NEW_TAB_RETURN diagnostic_host_ms=%.3f'%((time.monotonic()-started)*1000),flush=True)
        return finish()
class NewTab(gdb.Breakpoint):
    def stop(self):
        global owner,browser_pid
        if started is None or owner is not None:return False
        thread=current();owner=int(thread['tid']);browser_pid=int(thread['process'].dereference()['pid'])
        print('NEW_TAB_ENTER diagnostic_host_ms=%.3f tid=%d'%((time.monotonic()-started)*1000,owner),flush=True)
        Returned('*%#x'%int(gdb.parse_and_eval('*(unsigned long long*)$rsp')),
                 gdb.BP_HARDWARE_BREAKPOINT,internal=True)
        return False
class Present(gdb.Breakpoint):
    def stop(self):
        global present_done
        if browser_pid is None or present_done:return False
        if int(gdb.parse_and_eval('(process_t*)$rdi').dereference()['pid'])!=browser_pid:return False
        present_done=True
        print('FIRST_BROWSER_PRESENT diagnostic_host_ms=%.3f damage=%d,%d %ux%u'%
             ((time.monotonic()-started)*1000,int(gdb.parse_and_eval('damage_x')),
              int(gdb.parse_and_eval('damage_y')),int(gdb.parse_and_eval('damage_width')),
              int(gdb.parse_and_eval('damage_height'))),flush=True)
        return finish()
Input('input_core_push',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
NewTab('*0x520f0100',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
Present('window_server_present',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
end
continue
