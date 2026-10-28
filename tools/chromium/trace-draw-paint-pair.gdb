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
# Four addresses. Pair by owner and return stack, not by first stage timestamps:
# DrawAndSwap may skip painting, then another invocation paints a later frame.
started=None
browser=None
calls=0
return_pc=None
pending=None
mode=os.environ.get('OS_DRAW_TRACE','return')
if mode not in ('return','decision'):raise gdb.GdbError('Unknown draw trace mode')
def current():
    cpu=gdb.selected_thread().num-1
    if not 0<=cpu<int(gdb.parse_and_eval('g_cpu_count')):raise gdb.GdbError('Unexpected vCPU')
    return gdb.parse_and_eval('g_cpus[%d].queue.current'%cpu).dereference()
def emit(label,fields):
    print(label+' '+json.dumps(fields),flush=True)
class Input(gdb.Breakpoint):
    def stop(self):
        global started,browser
        if started is not None:return False
        browser=int(current()['process'].dereference()['pid']);started=time.monotonic()
        return False
class Returned(gdb.Breakpoint):
    def stop(self):
        global pending
        if pending is None or int(current()['tid'])!=pending['tid'] or int(gdb.parse_and_eval('$rsp'))!=pending['sp']+8:return False
        emit('DRAW_RETURN',{'call':pending['call'],'diagnostic_host_duration_ms':(time.monotonic()-pending['begin'])*1000,
             'return_success':bool(int(gdb.parse_and_eval('$rax'))&255)})
        pending=None
        return False
class Draw(gdb.Breakpoint):
    def stop(self):
        global calls,return_pc,pending
        thread=current()
        if started is None or int(thread['process'].dereference()['pid'])!=browser or calls>=16:return False
        calls+=1
        sp=int(gdb.parse_and_eval('$rsp'));pc=int(gdb.parse_and_eval('*(unsigned long long*)$rsp'))
        if mode=='return' and return_pc is None:
            return_pc=pc
            Returned('*%#x'%pc,gdb.BP_HARDWARE_BREAKPOINT,internal=True)
        tracked=mode=='decision' or (pc==return_pc and pending is None)
        display=hex(int(gdb.parse_and_eval('$rdi')))
        emit('DRAW_ENTER',{'call':calls,'display':display,'diagnostic_host_ms':(time.monotonic()-started)*1000,'return_tracked':tracked})
        if tracked:pending={'call':calls,'display':display,'tid':int(thread['tid']),'sp':sp,'begin':time.monotonic()}
        return False
class Paint(gdb.Breakpoint):
    def stop(self):
        thread=current()
        if started is None or int(thread['process'].dereference()['pid'])!=browser:return False
        tracked=pending is not None and int(thread['tid'])==pending['tid'] and int(gdb.parse_and_eval('$rsp'))<pending['sp']
        emit('BEGIN_PAINT',{'diagnostic_host_ms':(time.monotonic()-started)*1000,
             'draw_call':pending['call'] if tracked else None,
             'display':pending['display'] if tracked else None,
             'draw_to_paint_ms':(time.monotonic()-pending['begin'])*1000 if tracked else None})
        gdb.post_event(lambda:gdb.execute('detach'))
        gdb.post_event(lambda:gdb.execute('quit'))
        return True
class Decision(gdb.Breakpoint):
    def stop(self):
        thread=current()
        if started is None or pending is None or int(thread['tid'])!=pending['tid']:
            return False
        if not int(thread['process']) or int(thread['process'].dereference()['pid'])!=browser:
            return False
        if int(gdb.parse_and_eval('$rsp'))>=pending['sp']:return False
        emit('DRAW_DECISION',{'call':pending['call'],'display':pending['display'],
             'will_draw':bool(int(gdb.parse_and_eval('$rsi'))&255),
             'diagnostic_host_ms':(time.monotonic()-started)*1000,
             'draw_to_decision_ms':(time.monotonic()-pending['begin'])*1000})
        return False
Input('*0x520f0100',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
Draw('*0x4ecf4de0',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
Paint('*0x4edaf0c0',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
if mode=='decision':Decision('*0x4ecd2cd0',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
end
continue
