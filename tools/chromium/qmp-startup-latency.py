"""Launch probe: desktop visible; caller specifies motion from current pointer.

Body detection is a heuristic requiring screenshot review. It follows the
visible toolbar extent, including geometry retained by Chrome's last session.
"""
import json
import pathlib
import tempfile
import time
import argparse
import subprocess

def browser_extent(width, pixels):
    # 对工具栏字形下方的多行进行采样。地址栏聚焦时会在第 90 行绘制蓝色边框；只采样一行
    # 会把 Dock 恢复后仍显示但已聚焦的浏览器误判为空桌面。
    light = [x for x in range(width)
             if sum(min(pixels[(y*width+x)*3:(y*width+x)*3+3]) >= 200
                    for y in (84, 86, 88, 90, 92)) >= 3]
    if len(light) < 300:
        return None
    left, right = light[0], light[-1]+1
    if len(light)/(right-left) < .9:
        return None
    return left, right

def body_pixel_count(width, pixels, left, right):
    """Count dark text or saturated artwork, not white/gray empty content."""
    count = 0
    for y in range(240, min(700, len(pixels)//(width*3))):
        row = pixels[(y*width+left)*3:(y*width+right)*3]
        for i in range(0, len(row), 3):
            rgb = row[i:i+3]
            high, low = max(rgb), min(rgb)
            count += high < 100 or (high-low > 80 and low < 180)
    return count


root = pathlib.Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
parser=argparse.ArgumentParser()
parser.add_argument('--move-x',type=int,default=265)
parser.add_argument('--move-y',type=int,default=990)
parser.add_argument('--label',choices=('warm','cold'),default='warm')
parser.add_argument('--reset-pointer', action='store_true',
                    help='Home the pointer first; move-x/y then specify desktop coordinates')
parser.add_argument('--close-existing',action='store_true',
                    help='Click the real root X on an inspected normal Chrome, then relaunch')
parser.add_argument('--capture-network-after',type=float,default=0,
                    help='Capture one guest network snapshot after this elapsed time')
args=parser.parse_args()
if args.capture_network_after and not (root/'build/qemu-running-kernel.elf').is_file():
    parser.error('Missing symbols for the running guest kernel')
qmp = QMPClient()
def command(name, arguments=None):
    return qmp.command(name, arguments)

if args.reset_pointer:
    # 新启动的 QEMU 不保证客户机指针位于 (0,0)。只发送相对移动可能会让指针移出 Dock，
    # 看起来像是启动失败。
    for _ in range(25):
        command('input-send-event', {'events':[
            {'type':'rel','data':{'axis':'x','value':-100}},
            {'type':'rel','data':{'axis':'y','value':-100}}]})
        time.sleep(.03)
with tempfile.TemporaryDirectory(prefix='system-startup-') as directory:
    filename=str(pathlib.Path(directory)/'frame.ppm')
    def frame():
        command('screendump',{'filename':filename})
        with open(filename,'rb') as image:
            assert image.readline().strip()==b'P6'
            width,height=map(int,image.readline().split())
            assert image.readline().strip()==b'255'
            return width,image.read()
    width,baseline=frame()
    if args.close_existing:
        extent = browser_extent(width, baseline)
        if extent is None or not (8 <= extent[0] <= 14 and 950 <= extent[1] <= 958):
            raise RuntimeError('Root-close test requires normal 945x1060 Chrome; do not click blindly')
        for dx, dy in [(-2000, -1200), (935, 30)]:
            while dx or dy:
                x, y = max(-100,min(100,dx)), max(-100,min(100,dy))
                command('input-send-event',{'events':[
                    {'type':'rel','data':{'axis':'x','value':x}},
                    {'type':'rel','data':{'axis':'y','value':y}}]})
                dx, dy = dx-x, dy-y
                time.sleep(.05)
        for down in (True,False):
            command('input-send-event',{'events':[
                {'type':'btn','data':{'button':'left','down':down}}]})
            time.sleep(.05)
        deadline = time.monotonic()+10
        while True:
            width, baseline = frame()
            if browser_extent(width, baseline) is None:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('Root X did not remove Chrome from the desktop')
            time.sleep(.1)
        command('screendump',{'filename':str(root/'build/chrome-root-closed.png'),
                             'format':'png'})
        # 当前已校准指针位置是根 X；规范 Dock 图标位于 (1200,1020)。忽略调用方的相对启动默认值。
        args.move_x, args.move_y = 265, 990
    offset=(20*width+20)*3
    desktop=baseline[offset:offset+3]
    dx, dy = args.move_x, args.move_y
    while dx or dy:
        x, y = max(-100,min(100,dx)), max(-100,min(100,dy))
        command('input-send-event',{'events':[
            {'type':'rel','data':{'axis':'x','value':x}},
            {'type':'rel','data':{'axis':'y','value':y}}]})
        dx, dy = dx-x, dy-y
        time.sleep(.1)
    start=time.monotonic()
    for down in (True,False):
        command('input-send-event',{'events':[
            {'type':'btn','data':{'button':'left','down':down}}]})
        if down: time.sleep(.05)
    window_seconds=None
    debug_seconds=0
    debug_captured=False
    while time.monotonic()-start<120:
        width,pixels=frame()
        if pixels[offset:offset+3]!=desktop and window_seconds is None:
            window_seconds=time.monotonic()-start
            print(f'Window visible: {window_seconds:.2f}s',flush=True)
        if window_seconds is not None:
            extent = browser_extent(width, pixels)
            if extent is None:
                time.sleep(1)
                continue
            center = (extent[0]+extent[1])//2
            left, right = max(extent[0], center-275), min(extent[1], center+275)
            # 同时包含在线 Google 标志/Doodle，而不只是大多为白色的搜索/页脚区域。
            # 空白导航在这里仍没有正文像素；截图仍是权威门禁依据。
            if body_pixel_count(width, pixels, left, right)>1000:
                result={'window_seconds':window_seconds,
                        'body_seconds':time.monotonic()-start,
                        'sampling_interval_seconds':1,
                        'verification':'Heuristic; inspect final screenshot to confirm online body'}
                result['label']=args.label
                result['body_sample_x_range']=[left, right]
                result['observed_browser_x_extent']=list(extent)
                result['debug_pause_seconds']=debug_seconds
                result['debug_capture_attempted']=debug_captured
                (root/f'build/chrome-{args.label}-startup.json').write_text(json.dumps(result,indent=2)+'\n')
                command('screendump',{'filename':str(root/f'build/chrome-{args.label}-startup.png'),'format':'png'})
                print(json.dumps(result),flush=True)
                break
            if (args.capture_network_after > 0 and not debug_captured and
                    time.monotonic()-start >= args.capture_network_after):
                debug_captured=True
                debug_start=time.monotonic()
                print('Capturing slow-start network state; measurement will be perturbed',flush=True)
                try:
                    command('human-monitor-command',{'command-line':
                        'gdbserver unix:/tmp/system-network.sock,server=on,wait=off'})
                    with (root/'build/chrome-startup-network-stall.log').open('w') as log:
                        subprocess.run(['gdb','-q','-nx','-batch',
                            str(root/'build/qemu-running-kernel.elf'),'-x',
                            str(root/'tools/chromium/inspect-network-worker.gdb')],
                            cwd=root,stdout=log,stderr=subprocess.STDOUT,timeout=8,check=True)
                finally:
                    command('human-monitor-command',{'command-line':'gdbserver none'})
                    command('cont')
                    assert command('query-status')['running']
                    debug_seconds=time.monotonic()-debug_start
        time.sleep(1)
    else:
        raise RuntimeError('No rendered body within 120 seconds; do not relaunch automatically')
