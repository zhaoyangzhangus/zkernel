# Bounded, read-only return probes for the pinned native video bridge.
set pagination off
set confirm off
python
import gdb
import json
import os
import sys
from pathlib import Path
sys.path.insert(0,str(Path('tools/chromium').resolve()))
from trace_build import validate_chrome_build,_chrome_build_id
from guest_memory import read_user
validate_chrome_build('Build ID: bade6abd6c51df18')
expected='Build ID: 92ef122fa8ec44beedd1866b47c1bd2180669d26'
for path in ('build/esp/usr/apps/chrome/lib/libos-vulkan-video.so.1',
             'build/nvidia/vulkan/libos-vulkan-video.so.1'):
    if _chrome_build_id(Path(path))!=expected:
        raise gdb.GdbError('Refuse stale video-library probe offsets')
gdb.execute('target remote /tmp/system-video-fences.sock')
query_mode=os.environ.get('OS_VIDEO_TRACE_QUERY')=='1'
transfer_mode=os.environ.get('OS_VIDEO_TRACE_TRANSFER')=='1'
if query_mode and transfer_mode:
    raise gdb.GdbError('Select one diagnostic mode')
counts={'download':0,'start':0,'reset':0,'frames':0,'queries':0,'transfers':0}
query_results=[]
failures=[]
trace_root=None

class TransferPath(gdb.Breakpoint):
    def stop(self):
        if int(gdb.parse_and_eval('$cr3')) & ~4095 != trace_root:
            return False
        memory=gdb.selected_inferior()
        def u64(address):return int.from_bytes(read_user(memory,trace_root,address,8),'little')
        hwfc=int(gdb.parse_and_eval('$rdi'))
        device_private=u64(u64(hwfc+0x10)+0x10)
        extensions=u64(device_private+0x980)
        counts['transfers']+=1
        if counts['transfers']==1:
            print('VIDEO_TRANSFER_PATH '+json.dumps(dict(
                external_host_memory_enabled=bool(extensions & 0x10),
                upload=int(gdb.parse_and_eval('$rcx')) & 0xffffffff)),flush=True)
        return counts['transfers']>=32

class DecodeQuery(gdb.Breakpoint):
    def __init__(self,address,frame_probe):
        super().__init__('*%#x'%address,type=gdb.BP_HARDWARE_BREAKPOINT)
        self.frame_probe=frame_probe

    def stop(self):
        if int(gdb.parse_and_eval('$cr3')) & ~4095 != trace_root:
            return False
        if self.frame_probe:
            counts['frames']+=1
            enabled=int(gdb.parse_and_eval('$r8')) & 0xffffffff
            if counts['frames']==1:
                print('VIDEO_QUERY_POOL_COUNT %d'%enabled,flush=True)
            return counts['frames']>=128
        result=int(gdb.parse_and_eval('$rax')) & 0xffffffff
        if result & 0x80000000:result-=1<<32
        status=None
        if result==0:
            stack=int(gdb.parse_and_eval('$rsp'))
            memory=gdb.selected_inferior()
            pointer=int.from_bytes(read_user(memory,trace_root,stack+0xf0,8),'little')
            status=int.from_bytes(read_user(memory,trace_root,pointer,4),'little',signed=True)
        counts['queries']+=1
        query_results.append(dict(result=result,status=status))
        return False

class FenceResult(gdb.Breakpoint):
    def __init__(self,address,label):
        super().__init__('*%#x'%address,type=gdb.BP_HARDWARE_BREAKPOINT)
        self.label=label

    def stop(self):
        if int(gdb.parse_and_eval('$cr3')) & ~4095 != trace_root:
            return False
        value=int(gdb.parse_and_eval('$rax')) & 0xffffffff
        if value & 0x80000000:
            value-=1<<32
        counts[self.label]+=1
        if value:
            failures.append(dict(label=self.label,result=value))
            print('VIDEO_FENCE_FAILURE label=%s result=%d'%(self.label,value),flush=True)
        return bool(failures) or counts['download']>=64

class LocateLibrary(gdb.Breakpoint):
    def __init__(self):
        super().__init__('*0x45948a7b',type=gdb.BP_HARDWARE_BREAKPOINT)

    def stop(self):
        global trace_root
        trace_root=int(gdb.parse_and_eval('$cr3')) & ~4095
        native=int(gdb.parse_and_eval('$rbx'))
        send=int.from_bytes(read_user(gdb.selected_inferior(),trace_root,
                                     native+0x190,8),'little')
        base=send-0x1b940
        if not (0x10000<=base<0x800000000000 and base%4096==0):
            raise gdb.GdbError('Invalid video bridge load base')
        self.enabled=False
        if transfer_mode:
            TransferPath('*%#x'%(base+0x13dd50),type=gdb.BP_HARDWARE_BREAKPOINT)
        elif query_mode:
            DecodeQuery(base+0xff041,True)
            DecodeQuery(base+0xff42d,False)
        else:
            FenceResult(base+0x11b9b2,'download')
            FenceResult(base+0x11ba40,'start')
            FenceResult(base+0x11ba63,'reset')
        print('VIDEO_FENCE_PROBES_READY base=%#x root=%#x'%(base,trace_root),flush=True)
        return False

LocateLibrary()
print('VIDEO_FENCE_LOCATOR_READY',flush=True)
end
continue
python
print('VIDEO_FENCE_RESULT '+json.dumps(dict(counts=counts,failures=failures,
    query_results=query_results)),flush=True)
if query_mode and counts['frames']<128:
    raise gdb.GdbError('Incomplete decode query trace')
if transfer_mode and counts['transfers']<32:
    raise gdb.GdbError('Incomplete transfer path trace')
if not query_mode and not transfer_mode and not failures and counts['download']<64:
    raise gdb.GdbError('Incomplete fence trace')
end
delete breakpoints
detach
quit
