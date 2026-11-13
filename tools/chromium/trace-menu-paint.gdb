set pagination off
set confirm off
python
import gdb, struct, time, json, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/chromium').resolve()))
from trace_build import validate_chrome_build
expected='Build ID: 473dab3074c99813'
try:
    validate_chrome_build(expected)
except RuntimeError as error:
    raise gdb.GdbError('Refusing stale Chrome diagnostic: ' + str(error))
end
target remote /tmp/system-keys.sock
python
# 仅使用四个入口地址。硬件断点会影响这些墙钟时间线；
# 它们不是 CPU 耗时，也不能证明菜单已经可见。
started=None
owner=None
samples=0
def current():
    cpu=gdb.selected_thread().num-1
    if not 0<=cpu<int(gdb.parse_and_eval('g_cpu_count')):
        raise gdb.GdbError('Unexpected vCPU')
    return gdb.parse_and_eval('g_cpus[%d].queue.current'%cpu).dereference()
class Stage(gdb.Breakpoint):
    def __init__(self,address,label):
        super().__init__(address,gdb.BP_HARDWARE_BREAKPOINT,internal=True)
        self.label=label
    def stop(self):
        global started,owner,samples
        thread=current()
        if not int(thread['process']):return False
        process=thread['process'].dereference()
        if process['name'].string().rsplit('/',1)[-1]!='chrome':return False
        pid=int(process['pid'])
        if self.label=='INPUT':
            p=int(gdb.parse_and_eval('$rsi'))
            if not p:return False
            _,_,kind,_,code,value=struct.unpack('<QIHHIi',bytes(gdb.selected_inferior().read_memory(p,24)))
            if kind!=2 or code!=0x110 or value!=1 or started is not None:return False
            owner=pid;started=time.monotonic()
        if started is None or pid!=owner or samples>=20:return False
        samples+=1
        record={'stage':self.label,'pid':pid,'tid':int(thread['tid']),
                'object':hex(int(gdb.parse_and_eval('$rdi'))),
                'diagnostic_host_ms':(time.monotonic()-started)*1000}
        if self.label=='BEGIN_PAINT':
            p=int(gdb.parse_and_eval('$rsi'))
            record['damage_xywh']=struct.unpack('<iiii',bytes(gdb.selected_inferior().read_memory(p,16)))
        print('MENU_STAGE '+json.dumps(record),flush=True)
        if samples<20:return False
        gdb.post_event(lambda:gdb.execute('detach'))
        gdb.post_event(lambda:gdb.execute('quit'))
        return True
Stage('*0x45515040','INPUT')
Stage('*0x4e90d780','MENU_HOST_INIT')
Stage('*0x4eb67180','BEGIN_PAINT')
Stage('*0x4eb67260','END_PAINT')
end
continue
