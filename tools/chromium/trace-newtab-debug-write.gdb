set pagination off
set confirm off
python
import gdb, re, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/chromium').resolve()))
from trace_build import validate_chrome_build
expected = 'Build ID: 5c734622f8e4d5fe'
try:
    validate_chrome_build(expected)
except RuntimeError as error:
    raise gdb.GdbError('Refusing stale Chrome diagnostic: ' + str(error))
end
target remote /tmp/system-keys.sock
python
# 仅对有界的诊断前缀进行分类；不要记录用户字符串或 URL。
owner = None
samples = 0
def tid():
    cpu = gdb.selected_thread().num-1
    if not 0 <= cpu < int(gdb.parse_and_eval('g_cpu_count')):
        raise gdb.GdbError('Unexpected vCPU')
    return int(gdb.parse_and_eval('g_cpus[%d].queue.current->tid' % cpu))
def finish():
    gdb.post_event(lambda: gdb.execute('detach'))
    gdb.post_event(lambda: gdb.execute('quit'))
    return True
class Returned(gdb.Breakpoint):
    def stop(self):
        if tid() != owner: return False
        print('DEBUG_WRITE_SUMMARY samples=%d' % samples, flush=True)
        return finish()
class Entry(gdb.Breakpoint):
    def stop(self):
        global owner
        if owner is None:
            owner=tid()
            Returned('*%#x' % int(gdb.parse_and_eval('*(unsigned long long*)$rsp')),
                     gdb.BP_HARDWARE_BREAKPOINT, internal=True)
        return False
class DebugWrite(gdb.Breakpoint):
    def stop(self):
        global samples
        if owner is None or tid()!=owner: return False
        length=int(gdb.parse_and_eval('$rsi'))
        marker='non-marker'
        try:
            raw=bytes(gdb.selected_inferior().read_memory(int(gdb.parse_and_eval('$rdi')),min(length,64)))
            match=re.match(rb'(?:OS|LITEOS)_[A-Z0-9_]+',raw)
            if match:marker=match.group().decode('ascii')
        except gdb.MemoryError:
            marker='unreadable'
        samples+=1
        print('DEBUG_WRITE sample=%d length=%d marker=%s' % (samples,length,marker), flush=True)
        return finish() if samples>=12 else False
Entry('*0x51e88fb0',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
DebugWrite('syscall_debug_write',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
end
continue
