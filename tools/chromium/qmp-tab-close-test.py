"""Create a guest tab, click its actual X, and verify header restoration."""
import argparse
import json
from pathlib import Path
import tempfile
import time
import statistics

parser = argparse.ArgumentParser()
parser.add_argument('--cycles', type=int, default=3)
parser.add_argument('--tab-strip-y', type=int, default=70,
                    help='Inspected tab center: 70 with compositor frame, 30 for legacy frameless layout')
parser.add_argument('--focus-body', action='store_true',
                    help='Focus the page before each Ctrl+T, not the omnibox')
parser.add_argument('--verify-google-ntp', action='store_true',
                    help='Also require the inspected default Google NTP logo; not for other providers/themes')
evidence=parser.add_mutually_exclusive_group()
evidence.add_argument('--no-png-evidence', action='store_true',
                     help='Skip extra PNG encoding; keep all raw pixel gates (default)')
evidence.add_argument('--png-evidence', dest='no_png_evidence', action='store_false',
                     help='Opt into extra PNG encoding; use outside latency profiling')
parser.set_defaults(no_png_evidence=True)
parser.add_argument('--poll-ms', type=int, default=5,
                    help='Pixel-gate polling interval, not actual application latency')
parser.add_argument('--key-hold-ms', type=int, default=40)
parser.add_argument('--key-injection', choices=('chord', 'send-key'), default='chord',
                    help='Direct simultaneous key events, or QEMU delayed send-key compatibility probe')
args = parser.parse_args()
if not 1 <= args.cycles <= 20:
    parser.error('cycles must be 1..20')
if not 20 <= args.tab_strip_y <= 200:
    parser.error('tab-strip-y must be 20..200')
if not 1 <= args.poll_ms <= 100 or not 1 <= args.key_hold_ms <= 200:
    parser.error('poll-ms must be 1..100 and key-hold-ms 1..200')
root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
qmp = QMPClient()

def command(name, arguments=None):
    return qmp.command(name, arguments)

def relative(dx, dy):
    while dx or dy:
        x, y = max(-100, min(100, dx)), max(-100, min(100, dy))
        command('input-send-event', {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': x}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': y}}]})
        dx, dy = dx - x, dy - y
        time.sleep(.1)

def difference(a, b):
    assert len(a)==len(b) and a
    return sum(abs(x-y) > 20 for x, y in zip(a, b)) / len(a)

def google_ntp_logo(width, height, pixels, offset_y=0):
    # 明确限定为已检查的普通 945x1060 默认新标签页。
    # 空白正文或只有页眉更新都不能满足此门禁。
    assert (width, height) == (1920, 1080)
    assert len(pixels) == width*height*3
    counts = [0, 0, 0, 0]
    # 一次性 --no-sandbox 信息栏属于原始标签页，不属于新建标签页。
    # 覆盖已检查的两种垂直 Logo 位置。
    for y in range(215+offset_y, 375+offset_y, 3):
        for x in range(340, 630, 3):
            r, g, b = pixels[(y*width+x)*3:(y*width+x)*3+3]
            counts[0] += b > 180 and b > r+60 and b > g+20
            counts[1] += r > 180 and r > g+70 and r > b+70
            counts[2] += r > 180 and g > 150 and b < 90
            counts[3] += g > 120 and g > r+40 and g > b+20
    return all(count >= 40 for count in counts)


def tab_header_regions(width, height, pixels, tab_y):
    assert (width, height) == (1920, 1080) and len(pixels)==width*height*3
    assert 20 <= tab_y <= 200
    activation=b''.join(pixels[(y*width+100)*3:(y*width+220)*3]
                        for y in range(tab_y+9,tab_y+13))
    header=b''.join(pixels[(y*width+320)*3:(y*width+450)*3]
                    for y in range(tab_y-14,tab_y+10))
    return activation,header

png_captures = []
def capture_png(name):
    if args.no_png_evidence:
        return
    started = time.monotonic()
    command('screendump', {'filename': str(root / f'build/{name}.png'), 'format': 'png'})
    png_captures.append({'name': name, 'capture_ms': (time.monotonic()-started)*1000})


assert command('query-status')['running']
# 此门禁假定浏览器尺寸为已检查的 945x1060，且初始只有一个标签页。
# 不要用于最大化或位置不同的浏览器窗口。
relative(-2000, -1200)
relative(509, args.tab_strip_y)
results = []
frame_captures = []
frame_state = {}
keys_down = False
key_release_due = 0.0
def release_chord(wait=False):
    global keys_down
    if keys_down:
        remaining = key_release_due-time.monotonic()
        if remaining > 0 and not wait:
            return
        if remaining > 0:
            time.sleep(remaining)
        command('input-send-event', {'events': [
            {'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': key}}}
            for key in ('t', 'ctrl')]})
        keys_down = False
class ChordGuard:
    def __enter__(self):
        return self
    def __exit__(self, kind, value, traceback):
        release_chord(wait=True)

with tempfile.TemporaryDirectory(prefix='system-tab-close-') as directory, ChordGuard():
    path = str(Path(directory) / 'frame.ppm')
    def frame():
        started = time.monotonic()
        command('screendump', {'filename': path})
        with open(path, 'rb') as file:
            assert file.readline().strip() == b'P6'
            width, height = map(int, file.readline().split())
            assert (width, height) == (1920, 1080)
            assert file.readline().strip() == b'255'
            pixels = file.read()
            assert len(pixels)==width*height*3
        if args.verify_google_ntp:
            frame_state['ntp_logo'] = google_ntp_logo(width, height, pixels,
                                                     args.tab_strip_y-30)
        # 第一个标签页下方的背景条带排除了标题/加载指示器以及第二个标签页关闭按钮的悬停区。
        # 其活动/非活动颜色变化可提供独立于标签缩小动画的响应门禁。
        activation,roi=tab_header_regions(width,height,pixels,args.tab_strip_y)
        frame_state['activation'] = activation
        # 选取第一个标签页标题/加载指示器之外，以及 x=509 关闭按钮/光标悬停区域之外的位置。
        frame_captures.append((time.monotonic()-started)*1000)
        return roi
    baseline = frame()
    baseline_activation = frame_state['activation']
    capture_png('chrome-tab-close-baseline')
    for cycle in range(1, args.cycles+1):
        if args.focus_body:
            # 使用已检查的右侧空白边缘，而不是 (300,450)：在 Bing 页面该点是结果链接，会在
            # Ctrl+T 前创建额外标签页，导致第二个标签页的固定 X 坐标失效。
            relative(411, 370)
            for down in (True, False):
                command('input-send-event', {'events': [
                    {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
                time.sleep(.03)
            relative(-411, -370)
        creation_started = time.monotonic()
        if args.key_injection == 'chord':
            keys_down = True
            key_release_due = time.monotonic()+args.key_hold_ms/1000
            command('input-send-event', {'events': [
                {'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': key}}}
                for key in ('ctrl', 't')]})
        else:
            command('send-key', {'keys': [{'type': 'qcode', 'data': key}
                                         for key in ('ctrl', 't')], 'hold-time': args.key_hold_ms})
        key_command_ms = (time.monotonic()-creation_started)*1000
        deadline = time.monotonic()+10
        creation_first_change_ms = None
        creation_activation_ms = None
        while True:
            release_chord()
            changed = difference(baseline, frame())
            if creation_activation_ms is None and difference(
                    baseline_activation, frame_state['activation']) > .03:
                creation_activation_ms = (time.monotonic()-creation_started)*1000
            if changed > .005 and creation_first_change_ms is None:
                creation_first_change_ms = (time.monotonic()-creation_started)*1000
            if changed >= .05:
                break
            if time.monotonic() >= deadline:
                release_chord(wait=True)
                capture_png('chrome-tab-create-failure')
                raise RuntimeError('New tab not visibly created; cannot test closing')
            time.sleep(args.poll_ms/1000)
        creation_ms = (time.monotonic()-creation_started)*1000
        release_chord(wait=True)
        time.sleep(.5)
        body_verified_ms = None
        if args.verify_google_ntp:
            body_deadline = time.monotonic()+5
            while True:
                frame()
                if frame_state['ntp_logo']:
                    body_verified_ms = (time.monotonic()-creation_started)*1000
                    break
                if time.monotonic() >= body_deadline:
                    capture_png('chrome-tab-body-failure')
                    raise RuntimeError('Default Google NTP body not rendered; header alone is not a pass')
                time.sleep(.05)
            capture_png(f'chrome-tab-body-verified-{cycle}')
        capture_png('chrome-tab-close-before')
        closing_baseline = frame()
        closing_activation = frame_state['activation']
        started = time.monotonic()
        for down in (True, False):
            command('input-send-event', {'events': [
                {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
            if down:
                time.sleep(.03)
        deadline = time.monotonic()+5
        close_first_change_ms = None
        close_activation_ms = None
        while True:
            sample = frame()
            if close_activation_ms is None and difference(
                    closing_activation, frame_state['activation']) > .03:
                close_activation_ms = (time.monotonic()-started)*1000
            if close_first_change_ms is None and difference(closing_baseline, sample) > .005:
                close_first_change_ms = (time.monotonic()-started)*1000
            restored = difference(baseline, sample)
            if restored < .02:
                break
            if time.monotonic() >= deadline:
                capture_png('chrome-tab-close-failure')
                raise RuntimeError(f'X click did not close tab: header difference={restored}')
            time.sleep(args.poll_ms/1000)
        result = {'cycle': cycle, 'new_tab_header_ms': creation_ms, 'click_to_header_restore_ms':
                  (time.monotonic()-started)*1000, 'restored_difference': restored,
                  'key_command_ms': key_command_ms,
                  'new_tab_first_change_ms': creation_first_change_ms,
                  'close_first_change_ms': close_first_change_ms,
                  'new_tab_activation_ms': creation_activation_ms,
                  'close_tab_activation_ms': close_activation_ms,
                  'google_ntp_logo_verified': True if args.verify_google_ntp else None,
                  'new_tab_body_verified_ms': body_verified_ms}
        results.append(result)
        print(json.dumps(result), flush=True)
        time.sleep(.3)
    capture_png('chrome-tab-close-final')
(root / 'build/chrome-tab-close-test.json').write_text(json.dumps({
    'cycles': results, 'focus_body': args.focus_body, 'tab_strip_y': args.tab_strip_y,
    'google_ntp_logo_gate_enabled': args.verify_google_ntp,
    'png_evidence_enabled': not args.no_png_evidence, 'png_captures': png_captures,
    'poll_ms': args.poll_ms, 'key_hold_ms': args.key_hold_ms,
    'key_injection': args.key_injection,
    'frame_capture_median_ms': statistics.median(frame_captures),
    'frame_capture_max_ms': max(frame_captures),
    'verification': 'Real guest X click and tab-header restoration',
    'body_timing_limitation': 'new_tab_body_verified_ms is observation time from key delivery, including the deliberate 500ms post-header dwell; it is not first-paint latency.',
    'limitation': 'Activation fields measure the first tab background changing active/inactive; first-change fields measure sampled second-tab header pixels. Neither measures event-dispatch time. Creation includes key delivery and polling. Direct chord events hold keys concurrently with sampling; send-key adds a QEMU inter-key delay. Closing includes 30ms button hold, capture and polling. Header restoration includes tab animation; fixed inspected layout. The optional body gate checks only the inspected default Google logo, not NTP interactivity or other providers/themes.'
}, indent=2)+'\n')
