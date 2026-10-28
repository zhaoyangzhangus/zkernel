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
# Four hardware addresses; no breakpoint deletion in stop(). This locates
# callers, not latency: hardware stops perturb execution. Never read strings.
owner = None
samples = 0
wait_return_pc = None
pending = None
def current():
    cpu=gdb.selected_thread().num-1
    if not 0<=cpu<int(gdb.parse_and_eval('g_cpu_count')):
        raise gdb.GdbError('Unexpected vCPU')
    return gdb.parse_and_eval('g_cpus[%d].queue.current' % cpu).dereference()
class Returned(gdb.Breakpoint):
    def stop(self):
        if int(current()['tid'])!=owner:return False
        print('FUTEX_SUMMARY samples=%d' % samples,flush=True)
        gdb.post_event(lambda:gdb.execute('detach'))
        gdb.post_event(lambda:gdb.execute('quit'))
        return True
class Entry(gdb.Breakpoint):
    def stop(self):
        global owner
        if owner is None:
            owner=int(current()['tid'])
            Returned('*%#x' % int(gdb.parse_and_eval('*(unsigned long long*)$rsp')),
                     gdb.BP_HARDWARE_BREAKPOINT,internal=True)
        return False
class WaitReturned(gdb.Breakpoint):
    def stop(self):
        global pending
        if pending is None or int(current()['tid']) != owner:
            return False
        sample,begin = pending
        pending = None
        print('FUTEX_RETURN '+json.dumps({'sample':sample,
              'diagnostic_host_ms':(time.monotonic()-begin)*1000,
              'status':int(gdb.parse_and_eval('(long long)$rax'))}),flush=True)
        return False
class Wait(gdb.Breakpoint):
    def stop(self):
        global samples, wait_return_pc, pending
        thread=current()
        if owner is None or int(thread['tid'])!=owner or samples>=8:return False
        return_pc=int(gdb.parse_and_eval('*(unsigned long long*)$rsp'))
        if wait_return_pc is None:
            wait_return_pc=return_pc
            WaitReturned('*%#x' % return_pc,gdb.BP_HARDWARE_BREAKPOINT,internal=True)
        top=int(thread['kernel_stack_top'])
        frame=gdb.Value(top-gdb.lookup_type('arch_trap_frame_t').sizeof).cast(
            gdb.lookup_type('arch_trap_frame_t').pointer()).dereference()
        stack=int(frame['rsp']);fp=int(frame['rbp']);returns=[]
        memory=gdb.selected_inferior()
        for depth in range(8):
            if fp<stack or fp>stack+1024*1024 or fp&7:break
            try:
                words=bytes(memory.read_memory(fp,16))
            except gdb.MemoryError:break
            parent=int.from_bytes(words[:8],'little')
            pc=int.from_bytes(words[8:],'little')
            if not 0x10000<=pc<0x800000000000:break
            returns.append(hex(pc))
            if parent<=fp:break
            fp=parent
        samples+=1
        print('FUTEX_WAIT '+json.dumps({'sample':samples,'tid':owner,
             'address':hex(int(gdb.parse_and_eval('$rsi'))),
             'expected':int(gdb.parse_and_eval('(unsigned int)$rdx')),
             'timeout_ns':int(gdb.parse_and_eval('(unsigned long long)$rcx')),
             'return_tracked':return_pc==wait_return_pc,
             'user_rip':hex(int(frame['rip'])),'user_returns':returns}),flush=True)
        pending=(samples,time.monotonic()) if return_pc==wait_return_pc else None
        return False
Entry('*0x520f0100',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
Wait('futex_wait_keyed',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
end
continue
