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
# Four hardware addresses, no temporary return breakpoint. These are entry
# timelines, not function durations or CPU benchmarks. Only the browser that
# received NewTab is attributed; child/background frames cannot satisfy paint.
started=None
browser=None
samples=0
# Browser UI uses LayerTreeHost::CreateSingleThreaded, unlike renderer's
# ProxyMain. Choose adjacent stages without exceeding four hardware slots.
mode=os.environ.get('OS_COMMIT_STAGE','activation')
stages={
    'activation':((0x4d35aea0,'BEGIN_MAIN_FRAME'),(0x4d2ba3c0,'ACTIVATE_SYNC_TREE')),
    'commit':((0x4d35aea0,'BEGIN_MAIN_FRAME'),(0x4d358280,'DO_COMMIT')),
    'dispatch':((0x4d35ad00,'SEND_BEGIN_MAIN_FRAME'),(0x4d35aea0,'BEGIN_MAIN_FRAME')),
}
if mode not in stages:raise gdb.GdbError('Unknown OS_COMMIT_STAGE')
def current():
    cpu=gdb.selected_thread().num-1
    if not 0<=cpu<int(gdb.parse_and_eval('g_cpu_count')):
        raise gdb.GdbError('Unexpected vCPU')
    return gdb.parse_and_eval('g_cpus[%d].queue.current'%cpu).dereference()
class NewTab(gdb.Breakpoint):
    def stop(self):
        global started,browser
        if started is not None:return False
        thread=current()
        if not int(thread['process']):return False
        process=thread['process'].dereference()
        if process['name'].string().rsplit('/',1)[-1]!='chrome':return False
        browser=int(process['pid']);started=time.monotonic()
        print('COMMIT_TRACE_START '+json.dumps({'pid':browser,'tid':int(thread['tid'])}),flush=True)
        return False
class Stage(gdb.Breakpoint):
    def __init__(self,address,label):
        super().__init__(address,gdb.BP_HARDWARE_BREAKPOINT,internal=True)
        self.label=label
    def stop(self):
        global samples
        if started is None:return False
        thread=current()
        if not int(thread['process']) or int(thread['process'].dereference()['pid'])!=browser:
            return False
        if samples>=40:return False
        samples+=1
        print('COMMIT_STAGE '+json.dumps({'label':self.label,'pid':browser,
             'tid':int(thread['tid']),'object':hex(int(gdb.parse_and_eval('$rdi'))),
             'diagnostic_host_ms':(time.monotonic()-started)*1000}),flush=True)
        if self.label!='BEGIN_PAINT':return False
        gdb.post_event(lambda:gdb.execute('detach'))
        gdb.post_event(lambda:gdb.execute('quit'))
        return True
NewTab('*0x520f0100',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
for address,label in stages[mode]:Stage('*%#x'%address,label)
Stage('*0x4edaf0c0','BEGIN_PAINT')
end
continue
