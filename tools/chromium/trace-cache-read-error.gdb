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
# One hardware address, eight bounded error records, no code/data writes.
# Address = current llvm-nm OnCacheReadError 0xbad0310 + ELF_PIE_BIAS.
samples=0
class CacheError(gdb.Breakpoint):
    def stop(self):
        global samples
        if samples>=8:return False
        cpu=gdb.selected_thread().num-1
        if not 0<=cpu<int(gdb.parse_and_eval('g_cpu_count')):
            raise gdb.GdbError('Unexpected vCPU')
        thread=gdb.parse_and_eval('g_cpus[%d].queue.current'%cpu).dereference()
        if not int(thread['process']):return False
        process=thread['process'].dereference()
        if process['name'].string().rsplit('/',1)[-1]!='chrome':return False
        samples+=1
        result=int(gdb.parse_and_eval('$esi')) & 0xffffffff
        if result & 0x80000000:result-=0x100000000
        rsp=int(gdb.parse_and_eval('$rsp'))
        return_pc=struct.unpack('<Q',bytes(gdb.selected_inferior().read_memory(rsp,8)))[0]
        record={'pid':int(process['pid']),'tid':int(thread['tid']),
                'result':result,'restart':bool(int(gdb.parse_and_eval('$dl'))),
                'return_pc':hex(return_pc),'binary_return_offset':hex(return_pc-0x40000000)}
        print('CACHE_READ_ERROR '+json.dumps(record),flush=True)
        if samples<8:return False
        gdb.post_event(lambda:gdb.execute('detach'))
        gdb.post_event(lambda:gdb.execute('quit'))
        return True
CacheError('*0x4bba65c0',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
print('CACHE_ERROR_TRACE_READY',flush=True)
end
continue
