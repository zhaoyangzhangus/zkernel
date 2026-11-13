"""Guest-only QA, fresh canonical 1080p desktop; leave Task Manager open."""
from pathlib import Path
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame
qmp = QMPClient()
command, move, click, key = qmp.command, qmp.move, qmp.click, qmp.key
def screenshot(label):
    qmp.screenshot(root / ('build/taskmgr-' + label + '.png'))
assert command('query-status')['running']
with tempfile.TemporaryDirectory(prefix='system-taskmgr-pixels-') as directory:
    filename=str(Path(directory)/'frame.ppm')
    def frame():
        command('screendump',{'filename':filename})
        return read_qemu_frame(filename)
    def roi(pixels,x,y,w,h):
        return b''.join(pixels[((y+r)*1920+x)*3:((y+r)*1920+x+w)*3] for r in range(h))
    click(1104,1020);time.sleep(1.3)
    before=frame()
    screenshot('before-auto')
    click(912,1020);time.sleep(.8) # create our own blank editor, never kill a user document
    click(1600,155);time.sleep(1.3) # minimize our editor; reveal manager, no second launch or refresh command
    after=frame()
    screenshot('after-auto')
    assert roi(before,484,517,200,24)!=roi(after,484,517,200,24), 'New process did not appear automatically'
    assert roi(before,448,284,4,4)==roi(after,448,284,4,4), 'Automatic refresh lost selection'
    screenshot('auto-process')
    click(500,527,'right');screenshot('context-process')
    click(530,543);time.sleep(1.3)
    screenshot('confirm')
    click(530,543);time.sleep(1.3)
    ended=frame()
    assert roi(before,484,517,200,24)==roi(ended,484,517,200,24), 'Ended process is still listed'
    screenshot('ended')
    click(460,489);time.sleep(1.3)
    expanded=frame()
    assert roi(before,484,517,200,24)!=roi(expanded,484,517,200,24), 'Thread tree did not expand'
    time.sleep(1.3)
    assert roi(expanded,484,517,200,24)==roi(frame(),484,517,200,24), 'Refresh collapsed thread tree'
    screenshot('threads')
    click(550,527,'right');screenshot('context-thread')
    click(1000,700);time.sleep(.2)
    click(912,1020);time.sleep(.8)
    click(1600,155);time.sleep(1.3)
    click(460,563);time.sleep(.3) # expand our second blank editor
    click(550,597,'right');screenshot('context-thread-action')
    click(580,613);time.sleep(1.3)
    click(580,613);time.sleep(1.3)
    assert roi(expanded,484,551,200,24)==roi(frame(),484,551,200,24), 'Terminated thread/process is still listed'
    screenshot('ended-thread')
    for _ in range(4): key('ctrl','equal')
    time.sleep(.4)
    large=frame()
    assert roi(large,800,244,4,4)==bytes.fromhex('d3e3fd')*16, 'Caption did not grow with font'
    screenshot('large-font')
    # 扩大的最大化目标下半部，位于旧的 40px 命中框之外。
    click(1432,252);time.sleep(.5)
    maximized=frame()
    assert roi(maximized,20,10,4,4)==bytes.fromhex('d3e3fd')*16, 'Enlarged caption hit target failed'
    click(1872,24);time.sleep(.5)
    for _ in range(4): key('ctrl','minus')
    time.sleep(.3)
    screenshot('verified')
qmp.close()
print('QEMU: automatic process appearance, stable selection, context-menu process/thread termination, persistent thread expansion, font layout and enlarged maximize/restore passed')
