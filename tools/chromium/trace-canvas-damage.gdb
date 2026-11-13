# 在原生提交前，对成功的 GPU 读回矩形进行有界采样。
set pagination off
set confirm off
python
import gdb,json,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path('tools/chromium').resolve()))
from guest_memory import read_user
from trace_build import validate_chrome_build
validate_chrome_build('Build ID: 86b95e0215c7b1eb')
gdb.execute('target remote build/chrome-damage-gdb.sock')
records=[]
class Damage(gdb.Breakpoint):
    def stop(self):
        root=int(gdb.parse_and_eval('$cr3')) & ~4095
        address=int(gdb.parse_and_eval('$rsi'))
        # gfx::Rect：先是 Point（两个 int32），再是 Size（两个 int32）。
        rect=struct.unpack('<iiii',read_user(gdb.selected_inferior(),root,address,16))
        x,y,width,height=rect
        if not(0<=x<=1920 and 0<=y<=1080 and 0<width<=1920 and 0<height<=1080):
            raise gdb.GdbError('Invalid canvas rectangle; refuse stale layout')
        records.append(rect)
        return len(records)>=32
probe=Damage('*0x4559e650',type=gdb.BP_HARDWARE_BREAKPOINT)
gdb.execute('continue')
probe.delete()
print('CANVAS_DAMAGE '+json.dumps({'samples':len(records),'rectangles':records,
    'readback_pixels':sum(w*h for x,y,w,h in records),
    'full_945x1060_pixels':len(records)*945*1060}),flush=True)
gdb.execute('detach')
end
