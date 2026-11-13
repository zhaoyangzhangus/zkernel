# 在 COMMIT 入口、合成器发布之前读取稳定的客户区像素。
set pagination off
set confirm off
target remote /tmp/system-surface-capture.sock
set $submit_entry = &window_server_surface_end
add-symbol-file build/current-inspection-types.o 0
python
import gdb
import os
import struct
import sys
from pathlib import Path
sys.path.insert(0,str(Path('tools/chromium').resolve()))
from guest_memory import read_user
destination=Path(os.environ['OS_VIDEO_CAPTURE_DIR']).resolve()
if not destination.is_dir():
    raise gdb.GdbError('Capture directory must already exist')

class SurfaceSubmit(gdb.Breakpoint):
    def __init__(self):
        super().__init__('*%#x'%int(gdb.parse_and_eval('$submit_entry')),
                         type=gdb.BP_HARDWARE_BREAKPOINT)
        self.count=0

    def stop(self):
        try:
            return self.capture()
        except Exception:
            import traceback
            traceback.print_exc()
            raise

    def capture(self):
        if not int(gdb.parse_and_eval('$rcx')) & 255:
            return False
        memory=gdb.selected_inferior()
        frame=int(gdb.parse_and_eval('$rdx')) & ((1<<64)-1)
        token,address,capacity,width,height,stride,reserved=struct.unpack(
            '<QQQIIII',bytes(memory.read_memory(frame,40)))
        if not (1800<=width<=1920 and 900<=height<=1080):
            return False
        if not (token and stride==width*4 and capacity>=height*stride and
                not reserved):
            raise gdb.GdbError('Unexpected surface frame layout')
        owner=gdb.Value(int(gdb.parse_and_eval('$rdi')) & ((1<<64)-1)).cast(
            gdb.lookup_type('process_t').pointer()).dereference()
        if owner['name'].string()!='chrome':
            return False
        root=int(owner['vm'].dereference()['root_table']['value'])
        x,y,w,h=700,280,600,160
        rgb=bytearray()
        for row in range(y,y+h):
            bgra=read_user(memory,root,address+row*stride+x*4,w*4)
            for offset in range(0,len(bgra),4):
                rgb.extend((bgra[offset+2],bgra[offset+1],bgra[offset]))
        path=destination/('surface-submit-%02d.ppm'%self.count)
        path.write_bytes(('P6\n%d %d\n255\n'%(w,h)).encode()+rgb)
        print('SURFACE_SUBMIT index=%d pid=%d token=%d crop=%s path=%s'%
              (self.count,int(owner['pid']),token,(x,y,w,h),path),flush=True)
        self.count+=1
        return self.count>=8

capture=SurfaceSubmit()
print('SURFACE_CAPTURE_READY',flush=True)
end
continue
python
if capture.count != 8:
    raise gdb.GdbError('Incomplete surface capture; do not treat debugger exit as success')
end
delete breakpoints
detach
quit
