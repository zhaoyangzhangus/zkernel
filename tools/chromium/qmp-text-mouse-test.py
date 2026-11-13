"""Guest-only mouse QA; requires a fresh centered terminal in canonical QEMU."""
from pathlib import Path
import tempfile
import time

root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
qmp = QMPClient()
def command(name, arguments=None):
    return qmp.command(name, arguments)
def move(x, y):
    for dx, dy in [(-2500, -1500), (x, y)]:
        while dx or dy:
            a = max(-100, min(100, dx)); b = max(-100, min(100, dy))
            command('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': 'x', 'value': a}},
                {'type': 'rel', 'data': {'axis': 'y', 'value': b}}]})
            dx -= a; dy -= b; time.sleep(.008)
def button(down):
    command('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
    time.sleep(.03)
def click(x, y):
    move(x, y); button(True); button(False); time.sleep(.15)
def drag(x0, y0, x1, y1):
    move(x0, y0); button(True)
    # 拖动期间不重置原点：这会触发不需要的自动滚动。
    command('input-send-event', {'events': [
        {'type': 'rel', 'data': {'axis': 'x', 'value': x1-x0}},
        {'type': 'rel', 'data': {'axis': 'y', 'value': y1-y0}}]})
    time.sleep(.15); button(False); time.sleep(.15)
def key(*codes):
    for down in (True, False):
        command('input-send-event', {'events': [{'type': 'key', 'data': {
            'down': down, 'key': {'type': 'qcode', 'data': code}}}
            for code in (codes if down else reversed(codes))]})
        time.sleep(.035)
def type_text(text):
    punctuation = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus'}
    for char in text: key(punctuation.get(char, char))
def screenshot(label):
    command('screendump', {'filename': str(root / ('build/mouse-' + label + '.png')), 'format': 'png'})
assert command('query-status')['running']
with tempfile.TemporaryDirectory(prefix='system-mouse-frame-') as directory:
    path = str(Path(directory) / 'frame.ppm')
    def pixels():
        command('screendump', {'filename': path})
        with open(path, 'rb') as frame:
            assert frame.readline().strip() == b'P6'
            width, height = map(int, frame.readline().split())
            assert (width, height) == (1920, 1080) and frame.readline().strip() == b'255'
            return frame.read()
    def caret():
        frame = pixels()
        points = [(x,y) for y in range(175,920) for x in range(248,1640)
                  if frame[(y*1920+x)*3:(y*1920+x)*3+3] == bytes((26,115,232))]
        assert points, 'No visible text cursor'
        return min(x for x,y in points), min(y for x,y in points)
    def selection_count():
        frame = pixels()
        return sum(frame[(y*1920+x)*3:(y*1920+x)*3+3] == bytes((211,227,253))
                   for y in range(175,800) for x in range(250,800))
    start_x, start_y = caret()
    assert start_y < 400, 'Terminal prompt still at the bottom'
    type_text('echo abcdef'); end_x, _ = caret()
    advance = (end_x-start_x)/11
    click(start_x+round(7*advance), start_y+8)
    clicked_x, _ = caret(); assert clicked_x < end_x-20, 'Mouse did not reposition terminal cursor'
    type_text('x'); screenshot('terminal-click-insert')
    drag(start_x+round(5*advance),start_y+8,start_x+round(8*advance),start_y+8)
    assert selection_count()>200, 'No terminal command selection'
    screenshot('terminal-command-selection')
    type_text('z'); key('ret'); time.sleep(.4)
    _, next_y=caret(); assert next_y>start_y, 'Command/prompt did not advance inline'
    screenshot('terminal-selected-replacement')
    drag(250,184,470,184); assert selection_count()>1000, 'No terminal output selection'
    screenshot('terminal-output-selection')
    # 使用临时且未保存的文档，避免修改或保存用户的笔记。
    type_text('notepad /tmp/mouse-qa.txt'); key('ret'); time.sleep(1)
    note_x,note_y=caret(); assert note_x<300 and note_y==175
    type_text('abcdef'); end_x,_=caret()
    cell=(end_x-note_x)//6
    click(note_x+2*cell,note_y+8); type_text('x')
    x,_=caret(); assert x==note_x+3*cell, 'Editor click insertion failed'
    screenshot('notepad-click-insert')
    drag(note_x,note_y+8,note_x+3*cell,note_y+8)
    assert selection_count()>300, 'No editor selection'
    screenshot('notepad-selection')
    type_text('q'); x,_=caret(); assert x==note_x+cell, 'Editor selection replacement failed'
    screenshot('notepad-replacement')
    key('ctrl','z'); x,_=caret(); assert x==note_x+3*cell, 'Selection replacement undo failed'
    screenshot('notepad-undo')
    drag(note_x,note_y+8,1800,1000)
    click(1632,155);time.sleep(.4) # Capture must have released before maximize.
    frame=pixels();assert frame[(10*1920+200)*3:(10*1920+200)*3+3]==bytes((211,227,253)), 'Outside release kept pointer captured'
    screenshot('notepad-maximized-after-outside-release')
    key('ctrl','q');key('left');key('ret');time.sleep(.5) # Discard only our temporary unsaved document.
    screenshot('terminal-after-editor-test')
    drag(1677,942,1677,482);time.sleep(.4) # Shrink only the guest window.
    for _ in range(3):
        type_text('help');key('ret')
    time.sleep(.4);move(600,300);bottom_frame=pixels()
    screenshot('terminal-scrollback-bottom')
    command('input-send-event',{'events':[{'type':'btn','data':{'button':'wheel-up','down':True}},
                                        {'type':'btn','data':{'button':'wheel-up','down':False}}]})
    time.sleep(.4);scrolled=pixels();assert scrolled!=bottom_frame,'Terminal wheel did not scroll output'
    screenshot('terminal-scrollback-up')
    command('input-send-event',{'events':[{'type':'btn','data':{'button':'wheel-down','down':True}},
                                        {'type':'btn','data':{'button':'wheel-down','down':False}}]})
    time.sleep(.4);assert pixels()==bottom_frame,'Terminal wheel did not restore the bottom view'
    drag(1677,482,1677,942);time.sleep(.3)
    type_text('clear');key('ret');time.sleep(.3)
    screenshot('terminal-inline-final')
print('QEMU text mouse: inline prompt, click insertion, command/output selection, editor replacement/undo, outside release and wheel scrollback PASS')
