"""Real outside click: menu disappears and anchor button loses stale highlight."""
import argparse
import ast
import json
from pathlib import Path
import tempfile
import time

root=Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
parser=argparse.ArgumentParser()
parser.add_argument('--target',choices=('body','toolbar','desktop'),default='body')
parser.add_argument('--cycles',type=int,default=3)
args=parser.parse_args()
assert 1<=args.cycles<=10
tree=ast.parse((root/'tools/chromium/qmp-app-menu-test.py').read_text())
functions=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='menu_is_painted']
namespace={};exec(compile(ast.Module(body=functions,type_ignores=[]),'menu-gate','exec'),namespace)
qmp = QMPClient()
def command(name,arguments=None):
    return qmp.command(name, arguments)
def move(x,y):
    for dx,dy in [(-2000,-1200),(x,y)]:
        while dx or dy:
            a=max(-100,min(100,dx));b=max(-100,min(100,dy))
            command('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':a}},{'type':'rel','data':{'axis':'y','value':b}}]})
            dx-=a;dy-=b;time.sleep(.015)
def click():
    for down in (True,False):
        command('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
        if down:time.sleep(.02)
def png(label):
    command('screendump',{'filename':str(root/f'build/chrome-menu-outside-{args.target}-{label}.png'),'format':'png'})
assert command('query-status')['running']
command('send-key',{'keys':[{'type':'qcode','data':'esc'}],'hold-time':40});time.sleep(.3)
point={'body':(250,600),'toolbar':(400,72),'desktop':(1200,600)}[args.target]
with tempfile.TemporaryDirectory(prefix='system-menu-outside-') as directory:
    frame_path=str(Path(directory)/'frame.ppm')
    def frame():
        command('screendump',{'filename':frame_path})
        with open(frame_path,'rb') as f:
            assert f.readline().strip()==b'P6';w,h=map(int,f.readline().split());assert f.readline().strip()==b'255'
            assert (w,h)==(1920,1080);pixels=f.read()
        button=b''.join(pixels[(y*w+910)*3:(y*w+946)*3] for y in range(55,91))
        return button,namespace['menu_is_painted'](w,pixels)
    move(*point);time.sleep(.3);baseline,painted=frame();assert not painted
    png('baseline')
    # 验证基线状态确实未悬停。先前卡住的高亮不能成为参考状态并导致测试误通过。
    move(928,72);time.sleep(.35);hovered,_=frame()
    hover_difference=sum(abs(a-b)>8 for a,b in zip(baseline,hovered))/len(baseline)
    assert hover_difference>.05, f'Baseline already highlighted or hover broken: {hover_difference:.3f}'
    move(*point);time.sleep(.35);unhovered,_=frame()
    assert sum(abs(a-b)>8 for a,b in zip(baseline,unhovered))/len(baseline)<.01, 'Hover did not reset before opening menu'
    try:
        for cycle in range(args.cycles):
            move(928,72);click();time.sleep(.4)
            assert frame()[1],'Menu did not open'
            move(*point);click();started=time.monotonic()
            while time.monotonic()-started<2:
                current,painted=frame()
                difference=sum(abs(a-b)>8 for a,b in zip(baseline,current))/len(baseline)
                if not painted and difference<.01:break
                time.sleep(.03)
            else:
                png('failure')
                raise RuntimeError(f'Outside click: menu_visible={painted}, button_difference={difference:.3f}')
            png('closed');print(json.dumps({'target':args.target,'cycle':cycle+1,'button_difference':difference,'menu_closed':True}),flush=True)
    finally:
        # 不要让失败的原生点击或仍打开的菜单影响下一个测试。
        command('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':False}}]})
        command('send-key',{'keys':[{'type':'qcode','data':'esc'}],'hold-time':40})
