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
# Four entry addresses, read-only metadata. Offsets/vector size layout were
# checked against this guarded ELF's DirectRenderer::DrawFrame disassembly.
# The current libc++ vector stores begin + element COUNT, not begin + end.
# Times include debugger stops and are stage diagnostics, not CPU benchmarks.
started=None
browser=None
calls=0
samples=0
pending={}
memory=gdb.selected_inferior()
mode=os.environ.get('OS_RENDER_PASS_STAGE','passes')
if mode not in ('passes','capture'):raise gdb.GdbError('Unknown render-pass stage')
def current():
    cpu=gdb.selected_thread().num-1
    if not 0<=cpu<int(gdb.parse_and_eval('g_cpu_count')):raise gdb.GdbError('Unexpected vCPU')
    return gdb.parse_and_eval('g_cpus[%d].queue.current'%cpu).dereference()
def register(name):return int(gdb.parse_and_eval('$'+name))
def read_user(address,size):
    if not 0<address<0x800000000000 or not 1<=size<=64 or address+size>0x800000000000:
        raise gdb.GdbError('Out-of-bounds render metadata read')
    # QEMU debugger memory reads can use a different vCPU's user CR3. Resolve
    # the stopped thread's process mapping instead; never fault in a guest page.
    process=current()['process'].dereference()
    table=int(process['vm'].dereference()['root_table']['value'])
    result=bytearray()
    while size:
        physical=table
        for shift in (39,30,21,12):
            entry=struct.unpack('<Q',bytes(memory.read_memory(
                0xffff800000000000+physical+((address>>shift)&511)*8,8)))[0]
            if entry&5!=5:raise gdb.GdbError('Unmapped/non-user render metadata')
            physical=entry&0x000ffffffffff000
            if shift in (30,21) and entry&128:
                physical=(physical&~((1<<shift)-1))+(address&((1<<shift)-1))
                break
        else:physical+=address&4095
        amount=min(size,4096-(address&4095))
        result.extend(memory.read_memory(0xffff800000000000+physical,amount))
        address+=amount;size-=amount
    return bytes(result)
def read(address,format):
    return struct.unpack(format,read_user(address,struct.calcsize(format)))
def emit(label,fields):
    fields['diagnostic_host_ms']=(time.monotonic()-started)*1000
    print(label+' '+json.dumps(fields),flush=True)
def owned(thread):
    return started is not None and int(thread['process']) and int(thread['process'].dereference()['pid'])==browser
def paired(thread):
    frame=pending.get(int(thread['tid']))
    return frame if frame and register('rsp')<frame['sp'] else None
class Input(gdb.Breakpoint):
    def stop(self):
        global started,browser
        thread=current()
        if started is not None or not int(thread['process']):return False
        browser=int(thread['process'].dereference()['pid']);started=time.monotonic()
        return False
class Frame(gdb.Breakpoint):
    def stop(self):
        global calls
        thread=current()
        if not owned(thread) or calls>=16:return False
        calls+=1
        begin,count=read(register('rsi'),'<QQ')
        if not begin or not 1<=count<=1024:
            emit('INVALID_RENDER_PASS_ABI',{'begin':hex(begin),'count':count})
            gdb.post_event(lambda:gdb.execute('detach'))
            gdb.post_event(lambda:gdb.execute('quit'))
            return True
        root=read(begin+(count-1)*8,'<Q')[0]
        emit('DIRECT_FRAME_ABI',{'call':calls,'vector':hex(register('rsi')),
             'begin':hex(begin),'count':count,'root':hex(root),
             'viewport_address':hex(register('rdx'))})
        frame={'call':calls,'renderer':register('rdi'),'root':root,'sp':register('rsp')}
        pending[int(thread['tid'])]=frame
        emit('DIRECT_FRAME',{'call':calls,'tid':int(thread['tid']),
             'renderer':hex(frame['renderer']),'passes':count,
             'root_output':read(root,'<iiii'),'root_damage':read(root+16,'<iiii'),
             'device_viewport':read(register('rdx'),'<ii')})
        return False
class RenderPass(gdb.Breakpoint):
    def stop(self):
        global samples
        thread=current()
        if not owned(thread) or samples>=32:return False
        frame=paired(thread)
        if not frame or register('rdi')!=frame['renderer']:return False
        samples+=1
        render_pass=register('rsi')
        emit('SOFTWARE_RENDER_PASS',{'call':frame['call'],
             'is_root':render_pass==frame['root'],
             'pass_output':read(render_pass,'<iiii'),
             'pass_damage':read(render_pass+16,'<iiii'),
             'update_rect':read(register('rcx'),'<iiii'),
             'viewport':read(register('r8'),'<ii')})
        return False
class Paint(gdb.Breakpoint):
    def stop(self):
        thread=current()
        if not owned(thread):return False
        frame=paired(thread)
        emit('ROOT_OUTPUT_PAINT',{'call':frame['call'] if frame else None,
             'damage':read(register('rsi'),'<iiii')})
        gdb.post_event(lambda:gdb.execute('detach'))
        gdb.post_event(lambda:gdb.execute('quit'))
        return True
class Capture(gdb.Breakpoint):
    def stop(self):
        global samples
        thread=current()
        if not owned(thread) or samples>=4:return False
        samples+=1
        emit('TAB_THUMBNAIL_REQUEST',{'tid':int(thread['tid'])})
        return False
Input('*0x520f0100',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
Frame('*0x4ed1d180',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
if mode=='passes':RenderPass('*0x4ed332a0',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
else:Capture('*0x49f1d7e0',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
Paint('*0x4edaf0c0',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
end
continue
