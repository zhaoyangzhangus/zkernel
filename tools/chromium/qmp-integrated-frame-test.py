"""Real client-caption drag/resize in the sole QEMU, with kernel-state evidence."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools'))
from qmp_client import QMPClient

parser=argparse.ArgumentParser()
parser.add_argument('--evidence-dir',type=Path,default=root/'build')
parser.add_argument('--close-relaunch',action='store_true',
                    help='Also click the real Chrome X, verify removal and relaunch from the dock')
parser.add_argument('--tab-drag-only',action='store_true',
                    help='Check a second tab drag/real-X close without changing window geometry')
args=parser.parse_args()
if args.tab_drag_only and args.close_relaunch:
    parser.error('--tab-drag-only and --close-relaunch are separate trials')
args.evidence_dir.mkdir(parents=True,exist_ok=True)
subprocess.run(['cc','-g3','-fno-eliminate-unused-debug-types','-ffreestanding',
                '-DOS_KERNEL_BUILD','-Iinclude','-c','tools/chromium/inspection-types.c',
                '-o','build/current-inspection-types.o'],cwd=root,check=True)
qmp=QMPClient()
assert qmp.command('query-status')['running'], 'Do not resume a manually paused guest'
socket=root/'build/chrome-frame-gdb.sock'

def snapshot():
    code=r'''
import gdb,json
s=gdb.parse_and_eval('g_window_server')
pid=int(s['desktop_chrome_launch_pid'])
windows=[]
for i in range(int(s['count'])):
    w=s['windows'][i].dereference()
    if int(w['owner'].dereference()['pid'])!=pid:continue
    windows.append({key:int(w[key]) for key in
        ('identifier','flags','x','y','width','height','maximized','minimized',
         'client_pointer_capture','client_button_capture')})
print('FRAME_STATE '+json.dumps({'windows':windows,'drag':int(s['dragging_identifier']),
    'edges':int(s['resize_edges']),'pointer':[int(s['pointer_x']),int(s['pointer_y'])]}))
'''
    assert qmp.command('query-status')['running']
    qmp.command('human-monitor-command',{'command-line':'gdbserver none'})
    qmp.command('human-monitor-command',{'command-line':f'gdbserver unix:{socket},server=on,wait=off'})
    try:
        result=subprocess.run(['gdb','-q','-nx','-batch',str(root/'build/qemu-running-kernel.elf'),
            '-ex','add-symbol-file build/current-inspection-types.o 0',
            '-ex',f'target remote {socket}','-ex','python exec('+repr(code)+')',
            '-ex','detach'],cwd=root,text=True,capture_output=True,check=True,timeout=15)
        rows=[json.loads(line[len('FRAME_STATE '):]) for line in result.stdout.splitlines()
              if line.startswith('FRAME_STATE ')]
        if len(rows)!=1:raise RuntimeError(result.stdout+result.stderr)
        return rows[0]
    finally:
        qmp.command('human-monitor-command',{'command-line':'gdbserver none'})
        qmp.command('cont')

def browser(state):
    windows=state['windows']
    assert len(windows)==1, 'Close guest menus before this bounded test'
    window=windows[0]
    assert window['flags']&64 and window['flags']&4 and not window['flags']&16
    assert not window['maximized'] and not window['minimized']
    return window

def button(down):
    qmp.command('input-send-event',{'events':[
        {'type':'btn','data':{'button':'left','down':down}}]})
    time.sleep(.08)

def motion(dx,dy):
    while dx or dy:
        x=max(-10,min(10,dx));y=max(-10,min(10,dy))
        qmp.command('input-send-event',{'events':[
            {'type':'rel','data':{'axis':'x','value':x}},
            {'type':'rel','data':{'axis':'y','value':y}}]})
        dx-=x;dy-=y
        time.sleep(.03)

states={}
try:
    states['before']=snapshot()
    before=browser(states['before'])
    assert before['height']>500 and before['width']>700
    if args.tab_drag_only:
        # 要求已有一个标签页。第二个标签页仍留在标签栏中；此测试不验证拆分为新窗口或
        # 移动整个窗口。
        qmp.key('ctrl','t')
        time.sleep(1.5)
        qmp.screenshot(args.evidence_dir/'chrome-integrated-two-tabs.png')
        qmp.move(before['x']+380,before['y']+20)
        button(True)
        motion(-60,0)
        states['tab-held']=snapshot()
        held=browser(states['tab-held'])
        assert not states['tab-held']['drag'] and not states['tab-held']['edges']
        for field in ('identifier','x','y','width','height'):
            assert held[field]==before[field], 'Tab drag moved the native window'
        qmp.screenshot(args.evidence_dir/'chrome-integrated-tab-held.png')
        button(False)
        time.sleep(.3)
        states['tab-released']=snapshot()
        released=browser(states['tab-released'])
        assert not released['client_pointer_capture'] and not released['client_button_capture']
        qmp.click(before['x']+499,before['y']+20)
        time.sleep(.5)
        states['tab-closed']=snapshot()
        closed=browser(states['tab-closed'])
        for field in ('identifier','x','y','width','height'):
            assert closed[field]==before[field]
        assert not closed['client_pointer_capture'] and not closed['client_button_capture']
        qmp.screenshot(args.evidence_dir/'chrome-integrated-tab-closed.png')
        print(json.dumps(states,indent=2),flush=True)
        sys.exit(0)
    # 缩短窗口，为标题栏双轴拖动留出空间。
    qmp.move(before['x']+before['width']//2,before['y']+before['height']-2)
    button(True)
    motion(0,-180)
    states['resize-held']=snapshot()
    assert states['resize-held']['drag']==before['identifier']
    assert states['resize-held']['edges']&8
    button(False)
    time.sleep(.3)
    states['resized']=snapshot()
    resized=browser(states['resized'])
    assert before['height']-resized['height']>=150
    assert resized['width']==before['width'] and not states['resized']['drag']
    qmp.screenshot(args.evidence_dir/'chrome-integrated-resized.png')

    # 标签栏中的空白区域属于 HTCAPTION。内核只能在 Chromium 阈值触发后开始移动，不能
    # 截获标签本身的按下事件。
    qmp.move(resized['x']+600,resized['y']+20)
    button(True)
    motion(140,70)
    states['drag-held']=snapshot()
    held=browser(states['drag-held'])
    assert states['drag-held']['drag']==held['identifier'] and not states['drag-held']['edges']
    assert not held['client_pointer_capture'] and not held['client_button_capture']
    button(False)
    time.sleep(.3)
    states['moved']=snapshot()
    moved=browser(states['moved'])
    assert 100<=moved['x']-resized['x']<=150
    assert 35<=moved['y']-resized['y']<=80
    assert (moved['width'],moved['height'])==(resized['width'],resized['height'])
    assert not states['moved']['drag'] and not states['moved']['edges']
    qmp.screenshot(args.evidence_dir/'chrome-integrated-moved.png')

    # 交接后普通客户区点击仍必须能获取并释放隐式按压捕获，以证明交互式拖动已正常结束。
    qmp.move(moved['x']+400,moved['y']+500)
    button(True)
    states['client-held']=snapshot()
    assert browser(states['client-held'])['client_button_capture']
    button(False)
    states['client-released']=snapshot()
    assert not browser(states['client-released'])['client_button_capture']
    assert not states['client-released']['drag']
    if args.close_relaunch:
        qmp.click(moved['x']+moved['width']-20,moved['y']+20)
        time.sleep(1)
        states['closed']=snapshot()
        assert not states['closed']['windows'] and not states['closed']['drag']
        qmp.screenshot(args.evidence_dir/'chrome-integrated-closed.png')
        qmp.click(1200,1020)
        time.sleep(5)
        states['relaunched']=snapshot()
        relaunched=browser(states['relaunched'])
        assert relaunched['identifier']!=moved['identifier']
        qmp.screenshot(args.evidence_dir/'chrome-integrated-relaunched.png')
    print(json.dumps(states,indent=2),flush=True)
finally:
    button(False)
    qmp.close()
