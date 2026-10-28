# Inspect submitted H.264 references without changing decoder state.
set pagination off
set confirm off
python
import gdb
import json
import struct
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
        raise gdb.GdbError('Refuse stale video-library reference probe offsets')
layout=json.loads(Path('build/nvidia/vulkan/video-decode-layout.json').read_text())
if layout['layout_version']!=1 or any(not 0<=v<16*1024*1024 for v in layout.values()):
    raise gdb.GdbError('Invalid video layout metadata')
gdb.execute('target remote /tmp/system-video-references.sock')
capture=None

class References(gdb.Breakpoint):
    def __init__(self,address,root):
        super().__init__('*%#x'%address,type=gdb.BP_HARDWARE_BREAKPOINT)
        self.root=root
        self.count=0
        self.history={}
        self.mismatches=[]

    def stop(self):
        if int(gdb.parse_and_eval('$cr3')) & ~4095 != self.root:
            return False
        memory=gdb.selected_inferior()
        def read(address,size):return read_user(memory,self.root,address,size)
        def number(address,size=8,signed=False):return int.from_bytes(read(address,size),'little',signed=signed)
        def field(address,name,size=8,signed=False):return number(address+layout[name],size,signed)
        def poc(address,name):return struct.unpack('<ii',read(address+layout[name],8))
        def slot(address):
            h264=field(field(address,'slot_next'),'ref_std')
            resource=field(address,'slot_resource')
            return dict(slot=field(address,'slot_index',4,True),
                        frame_num=field(h264,'ref_num',2),poc=poc(h264,'ref_poc'),
                        view=field(resource,'resource_view'))
        codec=int(gdb.parse_and_eval('$rdi'))
        frame=int(gdb.parse_and_eval('$rsi'))
        picture=int(gdb.parse_and_eval('$rdx'))
        info=picture+layout['picture_info']
        h264=field(field(info,'info_next'),'h264_std')
        count=field(info,'info_count',4)
        if count>36:
            raise gdb.GdbError('Invalid H264 reference count')
        pointer=field(info,'info_refs')
        references=[slot(pointer+i*layout['slot_size']) for i in range(count)]
        setup=slot(field(info,'info_setup'))
        context=field(codec,'codec_private')
        short_count=field(context,'h264_short_count',4)
        if short_count>32:
            raise gdb.GdbError('Invalid CPU short reference count')
        cpu=[]
        for i in range(short_count):
            source=number(context+layout['h264_short_refs']+i*8)
            cpu.append(dict(frame_num=field(source,'cpu_picture_num',4,True),
                            poc=poc(source,'cpu_picture_poc'),
                            gap=field(source,'cpu_picture_gap',4)))
        if not self.count:
            decoder=field(field(codec,'codec_internal'),'internal_hwaccel')
            shared=field(decoder,'decode_shared')
            print('VIDEO_DPB_MODE '+json.dumps(dict(
                dedicated=field(decoder,'decode_dedicated',4),
                caps=field(shared+layout['shared_caps'],'decode_caps_flags',4))),flush=True)
        for reference in references:
            key=(reference['frame_num'],tuple(reference['poc']))
            previous=self.history.get(key)
            if previous and (previous['slot']!=reference['slot'] or previous['view']!=reference['view']):
                self.mismatches.append(dict(index=self.count,reference=reference,previous=previous))
        record=dict(index=self.count,pts_us=field(frame,'frame_pts',8,True),
                    frame_num=field(h264,'picture_num',2),poc=poc(h264,'picture_poc'),
                    setup=setup,refs=references,cpu_short=cpu)
        print('VIDEO_REFERENCES '+json.dumps(record),flush=True)
        self.history[(setup['frame_num'],tuple(setup['poc']))]=setup
        self.count+=1
        return self.count>=96

class LocateLibrary(gdb.Breakpoint):
    def __init__(self):
        super().__init__('*0x45948a7b',type=gdb.BP_HARDWARE_BREAKPOINT)

    def stop(self):
        global capture
        root=int(gdb.parse_and_eval('$cr3')) & ~4095
        native=int(gdb.parse_and_eval('$rbx'))
        send=int.from_bytes(read_user(gdb.selected_inferior(),root,native+0x190,8),'little')
        base=send-0x1b940
        if not (0x10000<=base<0x800000000000 and base%4096==0):
            raise gdb.GdbError('Invalid video bridge load base')
        self.enabled=False
        capture=References(base+0xfef10,root)
        print('VIDEO_REFERENCE_PROBES_READY',flush=True)
        return False

LocateLibrary()
end
continue
python
if capture is None or capture.count!=96:
    raise gdb.GdbError('Incomplete reference trace')
print('VIDEO_REFERENCE_RESULT '+json.dumps(dict(count=capture.count,
    mismatches=capture.mismatches)),flush=True)
end
delete breakpoints
detach
quit
