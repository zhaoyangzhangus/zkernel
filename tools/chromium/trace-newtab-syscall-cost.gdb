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
# Two hardware addresses; selected UI thread only, for the synchronous NewTab
# call. Outer runner MUST disable the selector after terminal debugger cleanup.
owner = None
started = None
memory = gdb.selected_inferior()
def address(name):
    return int(gdb.parse_and_eval('&'+name))
def write(name,value):
    memory.write_memory(address(name),struct.pack('<Q',value))
def current():
    cpu=gdb.selected_thread().num-1
    if not 0<=cpu<int(gdb.parse_and_eval('g_cpu_count')):
        raise gdb.GdbError('Unexpected vCPU')
    return gdb.parse_and_eval('g_cpus[%d].queue.current' % cpu).dereference()
class Returned(gdb.Breakpoint):
    def stop(self):
        if int(current()['tid']) != owner:
            return False
        write('g_syscall_cost_tid',0)
        values=struct.unpack('<'+'Q'*512,bytes(memory.read_memory(address('g_syscall_costs'),4096)))
        rows=[list(values[i:i+4]) for i in range(0,512,4) if values[i+1]]
        dropped=int(gdb.parse_and_eval('g_syscall_cost_dropped'))
        print('NEW_TAB_SYSCALL_COST '+json.dumps({'tid':owner,
              'diagnostic_host_ms':(time.monotonic()-started)*1000,
              'syscall_wall_costs':rows,'dropped':dropped,
              'selector_disabled':True}),flush=True)
        gdb.post_event(lambda:gdb.execute('detach'))
        gdb.post_event(lambda:gdb.execute('quit'))
        return True
class Entry(gdb.Breakpoint):
    def stop(self):
        global owner,started
        if owner is not None:
            return False
        if int(gdb.parse_and_eval('g_syscall_cost_tid')) != 0:
            raise gdb.GdbError('Another cost profile is active')
        if bytes(memory.read_memory(address('g_syscall_cost_guard'),1)) != b'\x00':
            raise gdb.GdbError('Cost publisher busy; do not reset')
        if int(gdb.parse_and_eval('sizeof(g_syscall_costs)')) != 4096:
            raise gdb.GdbError('Unexpected cost record layout')
        owner=int(current()['tid'])
        Returned('*%#x' % int(gdb.parse_and_eval('*(unsigned long long*)$rsp')),
                 gdb.BP_HARDWARE_BREAKPOINT,internal=True)
        memory.write_memory(address('g_syscall_costs'),bytes(4096))
        write('g_syscall_cost_dropped',0)
        write('g_syscall_cost_tid',owner)
        started=time.monotonic()
        return False
Entry('*0x520f0100',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
end
continue
