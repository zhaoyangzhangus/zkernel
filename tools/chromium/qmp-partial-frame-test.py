"""Pixel oracle for the staged /sbin/partial-frame-test.html in the sole QEMU.

Stage tests/browser/partial-frame.html before materializing the canonical disk.
Requires an already-open, normal Chrome with a viewport at least 760x400.
"""
import argparse
import json
import shutil
from pathlib import Path
import sys
import tempfile
import time

root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame


def frame_diagnostics(pixels):
    """Preserve scanline extents; a later screenshot is not the failed frame."""
    marker=b'\xff\x00\xff'
    offset=pixels.find(marker*16)
    if offset<0 or offset%3:
        return {'marker_found':False}
    x,y=(offset//3)%1920,(offset//3)//1920
    rows=[]
    if x+754>1920 or y+386>1080:
        return {'marker_found':True,'marker':[x,y],'clipped':True}
    for row in range(288):
        start=((y+98+row)*1920+x+34)*3
        line=pixels[start:start+720*3]
        red=[col for col in range(720)
             if line[col*3]>200 and line[col*3+1]<80 and line[col*3+2]<80]
        if red:rows.append({'y':row,'left':min(red),'right':max(red),'count':len(red)})
    return {'marker_found':True,'marker':[x,y],'red_rows':rows,
            'red_pixels':sum(row['count'] for row in rows)}


def save_failure(frame, evidence_dir, error):
    shutil.copyfile(frame,evidence_dir/'chrome-partial-frame-failure.ppm')
    report=frame_diagnostics(read_qemu_frame(frame))
    report['error']=str(error)
    (evidence_dir/'chrome-partial-frame-failure.json').write_text(
        json.dumps(report,indent=2)+'\n')


def inspect_frame(pixels):
    marker=b'\xff\x00\xff'
    offset=pixels.find(marker*16)
    if offset<0 or offset%3:
        raise ValueError('Test-page marker is not visible')
    x,y=(offset//3)%1920,(offset//3)//1920
    if x+754>1920 or y+386>1080:
        raise ValueError('Test-page arena is clipped')
    for row in range(y,y+16):
        start=(row*1920+x)*3
        if pixels[start:start+48]!=marker*16:
            raise ValueError('Incomplete test-page marker')
    left,top=x+34,y+98
    red=[]
    for row in range(288):
        line=pixels[((top+row)*1920+left)*3:((top+row)*1920+left+720)*3]
        for col in range(720):
            rgb=line[col*3:col*3+3]
            if rgb[0]>200 and rgb[1]<80 and rgb[2]<80:
                red.append((col,row))
            elif min(rgb)<250:
                raise ValueError(f'Stale/nonwhite arena pixel at {col},{row}: {list(rgb)}')
    if len(red)!=64*32:
        raise ValueError(f'Expected one 2048-pixel tile, got {len(red)} red pixels')
    xs,ys=zip(*red)
    if max(xs)-min(xs)!=63 or max(ys)-min(ys)!=31:
        raise ValueError('Split tile or trails outside its current rectangle')
    return min(xs),min(ys)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--samples',type=int,default=40)
    parser.add_argument('--evidence-dir',type=Path,default=root/'build')
    parser.add_argument('--already-open',action='store_true',
                        help='Check the current animation without reloading/resetting its buffers')
    args=parser.parse_args()
    if not 10<=args.samples<=120:parser.error('--samples must be 10..120')
    args.evidence_dir.mkdir(parents=True,exist_ok=True)
    codes={'/':'slash',':':'semicolon','.':'dot','-':'minus'}
    qmp=QMPClient()
    positions=[]
    try:
        assert qmp.command('query-status')['running'], 'Do not resume a paused guest'
        if not args.already_open:
            qmp.key('ctrl','l')
            for character in 'file:///sbin/partial-frame-test.html':
                if character==':':qmp.key('shift',codes[character])
                else:qmp.key(codes.get(character,character))
            qmp.key('ret')
        time.sleep(1)
        with tempfile.TemporaryDirectory(prefix='chrome-frame-oracle-') as directory:
            frame=Path(directory)/'frame.ppm'
            deadline=time.monotonic()+10
            while True:
                qmp.command('screendump',{'filename':str(frame)})
                try:inspect_frame(read_qemu_frame(frame));break
                except ValueError as error:
                    if time.monotonic()>deadline:
                        save_failure(frame,args.evidence_dir,error)
                        raise
                    time.sleep(.2)
            qmp.screenshot(args.evidence_dir/'chrome-partial-frame-start.png')
            for _ in range(args.samples):
                qmp.command('screendump',{'filename':str(frame)})
                try:
                    positions.append(inspect_frame(read_qemu_frame(frame)))
                except ValueError as error:
                    save_failure(frame,args.evidence_dir,error)
                    raise
                time.sleep(.04)
            if len(set(positions))<5:
                raise ValueError('Too few distinct tile positions: animation may be stalled')
            qmp.screenshot(args.evidence_dir/'chrome-partial-frame-end.png')
        print(json.dumps({'samples':len(positions),'distinct_positions':len(set(positions)),
                          'positions':positions,'pixel_oracle':'pass'},indent=2),flush=True)
    except BaseException:
        # 这里只尽力收集上下文：绝不能用 QMP 断连错误替代原始基准失败，也不能将后续图像
        # 与原始失败混淆。
        try:qmp.screenshot(args.evidence_dir/'chrome-partial-frame-after-failure.png')
        except Exception:pass
        raise
    finally:
        qmp.close()


if __name__=='__main__':
    main()
