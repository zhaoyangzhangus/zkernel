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
# One hardware address. InputEvent ABI is the 24-byte os_input_event_t prefix;
# read metadata only, never surrounding memory or text. No deletion in stop().
samples=0
class Key(gdb.Breakpoint):
    def stop(self):
        global samples
        if samples>=4:return False
        pointer=int(gdb.parse_and_eval('$rsi'))
        if pointer==0:return False
        timestamp,device,kind,flags,code,value=struct.unpack('<QIHHIi',
            bytes(gdb.selected_inferior().read_memory(pointer,24)))
        if kind!=1:return False
        samples+=1
        print('KEY timestamp=%d device=%d code=%d value=%d'%(timestamp,device,code,value),flush=True)
        if samples<4:return False
        gdb.post_event(lambda:gdb.execute('detach'))
        gdb.post_event(lambda:gdb.execute('quit'))
        return True
Key('*0x455a0a20',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
end
continue
