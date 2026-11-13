"""Bounded real wheel-input probe in the single inspected 1080p guest.

Reports visible body changes, not renderer FPS or event dispatch latency.
Use on a loaded, scrollable page; review the retained down/up screenshots.
"""
import json
from pathlib import Path
import tempfile
import time

root=Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
qmp = QMPClient()
def command(name,args=None):
    return qmp.command(name, args)
def move(dx,dy):
    while dx or dy:
        x=max(-100,min(100,dx));y=max(-100,min(100,dy))
        command('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':x}},{'type':'rel','data':{'axis':'y','value':y}}]})
        dx-=x;dy-=y;time.sleep(.02)
assert command('query-status')['running']
command('send-key',{'keys':[{'type':'qcode','data':'esc'}],'hold-time':40})
move(-2000,-1200);move(400,600);time.sleep(.3)
with tempfile.TemporaryDirectory(prefix='system-scroll-') as directory:
    path=str(Path(directory)/'frame.ppm')
    def frame():
        command('screendump',{'filename':path})
        with open(path,'rb') as f:
            assert f.readline().strip()==b'P6'
            w,h=map(int,f.readline().split());assert (w,h)==(1920,1080)
            assert f.readline().strip()==b'255';pixels=f.read()
        # 排除指针、工具栏、滚动条和桌面。稀疏采样可避免宿主侧像素比较进入客户机输入路径。
        return b''.join(pixels[(y*w+x)*3:(y*w+x)*3+3]
                        for y in range(200,900,12) for x in range(40,890,24))
    for direction in ('wheel-down','wheel-up'):
        for sample in range(3):
            before=frame();started=time.monotonic();down=False
            try:
                down=True
                command('input-send-event',{'events':[{'type':'btn','data':{'button':direction,'down':True}}]})
                time.sleep(.02)
                command('input-send-event',{'events':[{'type':'btn','data':{'button':direction,'down':False}}]});down=False
                while time.monotonic()-started<2:
                    after=frame()
                    change=sum(abs(a-b)>12 for a,b in zip(before,after))/len(before)
                    if change>.02:break
                    time.sleep(.01)
                else:raise RuntimeError('No wheel body movement: inspect page boundary/input route')
                print(json.dumps({'direction':direction,'sample':sample+1,
                    'first_body_change_ms':(time.monotonic()-started)*1000,
                    'changed_sample_fraction':change,'scope':'Includes wheel hold, QMP, screenshot and sparse comparison; not FPS'}),flush=True)
                time.sleep(.3)
            finally:
                if down:command('input-send-event',{'events':[{'type':'btn','data':{'button':direction,'down':False}}]})
        command('screendump',{'filename':str(root/f'build/chrome-scroll-{direction}.png'),'format':'png'})
