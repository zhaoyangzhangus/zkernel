"""Single canonical guest: real modal mouse/keyboard actions on QA-only files."""
from pathlib import Path
import os
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
qa_path = os.environ.get('OS_EDITOR_QA_PATH', '/tmp/message-box-qa.txt')
if qa_path not in ('/tmp/message-box-qa.txt', '/editor-save-qa.txt'):
    raise SystemExit('Only the two dedicated QA files are permitted')
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame

with QMPClient() as qmp, tempfile.TemporaryDirectory(prefix='editor-message-qa-') as directory:
    filename = str(Path(directory) / 'frame.ppm')
    def frame():
        qmp.move(1800, 960)
        time.sleep(.2)
        qmp.command('screendump', {'filename': filename})
        return read_qemu_frame(filename)
    def region(pixels, x, y, w, h):
        return b''.join(pixels[((y + row) * 1920 + x) * 3:
                               ((y + row) * 1920 + x + w) * 3] for row in range(h))
    def body(pixels):
        return region(pixels, 250, 175, 1000, 650)
    def type_text(text):
        codes = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus'}
        for character in text:
            qmp.key(codes.get(character, character))
    def launch(path):
        type_text('notepad ' + path)
        qmp.key('ret')
        time.sleep(.8)
    def focused_button(pixels):
        # 不假定字体高度：查找实际的强调按钮光栅图形。
        runs = []
        accent = bytes.fromhex('1a73e8')
        for y in range(350, 800):
            row = pixels[(y * 1920 + 650) * 3:(y * 1920 + 1270) * 3]
            start = None
            for x in range(620):
                blue = row[x * 3:x * 3 + 3] == accent
                if blue and start is None:
                    start = x
                if start is not None and (not blue or x == 619):
                    end = x if not blue else x + 1
                    if end - start >= 80:
                        runs.append((start + 650, end + 650, y))
                    start = None
        assert runs, 'Actual dialog button is missing'
        longest = max(b - a for a, b, _ in runs)
        rows = [(a, b, y) for a, b, y in runs if b - a == longest]
        return ((rows[0][0] + rows[0][1]) // 2,
                (min(y for _, _, y in rows) + max(y for _, _, y in rows)) // 2)

    assert qmp.command('query-status')['running']
    qmp.click(815, 1020)
    time.sleep(.5)
    launch(qa_path)
    type_text('abc')
    original = frame()
    assert region(original, 1000, 905, 300, 20) == b'\xff' * (300 * 20 * 3), 'Old footer remains'
    qmp.click(1664, 155)
    dialog = frame()
    focused_button(dialog)
    assert body(dialog) != body(original), 'Unsaved close did not show dialog'
    qmp.key('g')
    qmp.click(250, 200)
    qmp.move(1000, 200)
    qmp.command('input-send-event', {'events': [
        {'type': 'btn', 'data': {'button': 'wheel-down', 'down': True}},
        {'type': 'btn', 'data': {'button': 'wheel-down', 'down': False}}]})
    assert frame() == dialog, 'Modal input changed the underlying document'
    qmp.screenshot(root / 'build/notepad-message-unsaved.png')
    qmp.key('esc')
    assert body(frame()) == body(original), 'Escape lost text or left dim pixels'
    qmp.key('d')
    edited = frame()
    assert body(edited) != body(original), 'Modifier release got stuck during modal'
    qmp.key('ctrl', 'q')
    cancel = focused_button(frame())
    qmp.click(*cancel)
    assert body(frame()) == body(edited), 'Mouse Cancel failed'
    qmp.key('ctrl', 'q')
    qmp.key('left')
    discard = focused_button(frame())
    qmp.move(250, 200)
    qmp.command('input-send-event', {'events': [
        {'type': 'btn', 'data': {'button': 'left', 'down': True}}]})
    qmp.move(*discard)
    qmp.command('input-send-event', {'events': [
        {'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
    focused_button(frame())  # A release alone must not discard the document.
    qmp.key('esc')
    assert body(frame()) == body(edited)
    qmp.key('ctrl', 's')
    qmp.screenshot(root / 'build/notepad-message-saved.png')
    qmp.click(*focused_button(frame()))
    assert body(frame()) == body(edited), 'Saved notification failed to dismiss cleanly'
    qmp.key('ctrl', 'q')
    time.sleep(.5)
    launch(qa_path)
    assert body(frame()) == body(edited), 'Saved text did not survive reopen'
    qmp.key('e')
    replaced = frame()
    assert body(replaced) != body(edited), 'Existing document did not change'
    qmp.key('ctrl', 's')
    qmp.click(*focused_button(frame()))
    qmp.key('ctrl', 'q')
    time.sleep(.5)
    launch(qa_path)
    assert body(frame()) == body(replaced), 'Replacement save did not survive reopen'
    qmp.key('ctrl', 'q')
    time.sleep(.5)
    launch('/message-qa-missing-parent/notes.txt')
    type_text('abc')
    unsaved = frame()
    qmp.key('ctrl', 's')
    focused_button(frame())
    qmp.screenshot(root / 'build/notepad-message-save-failed.png')
    qmp.key('esc')
    assert body(frame()) == body(unsaved), 'Save failure lost edited contents'
    qmp.key('ctrl', 'q')
    qmp.click(*focused_button(frame()))  # Cancel must remain available after failure.
    assert body(frame()) == body(unsaved)
    qmp.key('ctrl', 'q')
    qmp.key('left')
    qmp.click(*focused_button(frame()))  # Discard this QA-only unsaved document.
    time.sleep(.5)
    qmp.click(1664, 155)
    assert qmp.command('query-status')['running']
print('Guest dialogs: modal input, Cancel/Discard, initial and replacement save/reopen, failure preservation and footer removal passed')
