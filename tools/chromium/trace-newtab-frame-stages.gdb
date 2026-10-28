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
# Four hardware addresses. Stage localization, not a native latency benchmark:
# hardware stops perturb time and the first frame can contain prior damage.
started = None
browser_pid = None
all_processes = os.environ.get('OS_FRAME_SCOPE') == 'all'
def pid():
    cpu = gdb.selected_thread().num-1
    if not 0 <= cpu < int(gdb.parse_and_eval('g_cpu_count')):
        raise gdb.GdbError('Unexpected vCPU number')
    return int(gdb.parse_and_eval('g_cpus[%d].queue.current->process->pid' % cpu))

class TabEntry(gdb.Breakpoint):
    def stop(self):
        global started,browser_pid
        if started is None:
            browser_pid = pid()
            started = time.monotonic()
            print('NEW_TAB_ENTER pid=%d' % browser_pid,flush=True)
        return False

class Stage(gdb.Breakpoint):
    def __init__(self,address,label):
        super().__init__(address,gdb.BP_HARDWARE_BREAKPOINT,internal=True)
        self.label,self.done,self.samples = label,False,0
    def stop(self):
        if started is None or self.done:
            return False
        stage_pid = pid()
        if self.label == 'PRESENT':
            if int(gdb.parse_and_eval('(process_t*)$rdi').dereference()['pid']) != browser_pid:
                return False
        elif not all_processes and stage_pid != browser_pid:
            return False
        self.samples += 1
        self.done = self.label not in ('DISPLAY_BEGIN_FRAME','DRAW_AND_SWAP') or self.samples >= 8
        print('%s diagnostic_host_ms=%.3f stage_pid=%d' %
              (self.label,(time.monotonic()-started)*1000,stage_pid),flush=True)
        if self.label == 'PRESENT':
            gdb.post_event(lambda: gdb.execute('detach'))
            gdb.post_event(lambda: gdb.execute('quit'))
            return True
        return False

TabEntry('*0x520f0100',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
frame_stage = os.environ.get('OS_FRAME_STAGE', 'schedule')
if frame_stage not in ('paint','pipeline','schedule'):
    raise gdb.GdbError('Unknown frame stage')
if frame_stage == 'paint':
    Stage('*0x4edaf0c0','BEGIN_PAINT')
    Stage('*0x4edaf1a0','END_PAINT')
elif frame_stage == 'pipeline':
    Stage('*0x4ecf4de0','DRAW_AND_SWAP')
    Stage('*0x4edaf0c0','BEGIN_PAINT')
else:
    # LiteOS deliberately disables its external platform ACK-loop adapter in
    # DesktopWindowTreeHostPlatform. Trace the actual Viz scheduler, not the
    # registered-but-unused NativeGuestBeginFrameSource::MaybeIssueFrame method.
    Stage('*0x4ecd4510','DISPLAY_BEGIN_FRAME')
    Stage('*0x4ecf4de0','DRAW_AND_SWAP')
Stage('window_server_present','PRESENT')
end
continue
