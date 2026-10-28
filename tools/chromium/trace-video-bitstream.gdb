# Compare submitted CPU-mapped Vulkan slice bytes with the matching input packet.
set pagination off
set confirm off
python
import gdb,json,re,sys,hashlib,os
from pathlib import Path
sys.path.insert(0,str(Path('tools/chromium').resolve()))
from guest_memory import read_user
from trace_build import validate_chrome_build,_chrome_build_id
validate_chrome_build('Build ID: bade6abd6c51df18')
expected=('Build ID: 364ba8b6c4ed89fc5e1e42951a2ee41f630ccb6e'
          if os.environ.get('OS_VIDEO_PADDING_CANDIDATE')=='1' else
          'Build ID: 92ef122fa8ec44beedd1866b47c1bd2180669d26')
candidate=os.environ.get('OS_VIDEO_PADDING_CANDIDATE')=='1'
for path in ('build/nvidia/vulkan/libos-vulkan-video.so.1',
             'build/esp/usr/apps/chrome/lib/libos-vulkan-video.so.1'):
    if _chrome_build_id(Path(path))!=expected:
        raise gdb.GdbError('Stale video bitstream probe')
layout=json.loads(Path('build/nvidia/vulkan/video-decode-layout.json').read_text())
if layout['layout_version']!=1 or any(not 0<=v<16*1024*1024 for v in layout.values()):
    raise gdb.GdbError('Invalid bitstream layout')
gdb.execute('target remote /tmp/system-video-bitstream.sock')
root=None
packet=None
probe=None
records=[]
def read(address,size):
    return read_user(gdb.selected_inferior(),root,address,size)
def number(address,size=8):return int.from_bytes(read(address,size),'little')
def field(address,name):return number(address+layout[name])
def vcl(data):
    return [nal.rstrip(b'\0') for nal in re.split(b'\x00\x00\x00?\x01',data)
            if nal and nal[0]&31 in (1,5)]

class Slices(gdb.Breakpoint):
    def stop(self):
        if int(gdb.parse_and_eval('$cr3')) & ~4095!=root:return False
        picture=int(gdb.parse_and_eval('$rdx'))
        frame=int(gdb.parse_and_eval('$rsi'))
        pts=field(frame,'frame_pts')
        if packet is None or packet[0]!=pts:
            raise gdb.GdbError('Input/frame PTS mismatch; refuse guessed alignment')
        size=field(picture,'picture_slice_bytes')
        buffer=field(field(picture,'picture_slices'),'buffer_ref_data')
        mapped=field(buffer,'vk_buffer_mapped')
        capacity=field(buffer,'vk_buffer_size')
        if not mapped or not 0<size<=4*1024*1024 or size>capacity:
            raise gdb.GdbError('Invalid mapped bitstream geometry')
        data=read(mapped,size)
        same=vcl(data)==packet[1]
        codec=int(gdb.parse_and_eval('$rdi'))
        decoder=field(field(codec,'codec_internal'),'internal_hwaccel')
        shared=field(decoder,'decode_shared')
        alignment=field(shared+layout['shared_video_caps'],'bitstream_alignment')
        if not 1<=alignment<=4*1024*1024 or alignment & (alignment-1):
            raise gdb.GdbError('Invalid bitstream alignment')
        aligned=(size+alignment-1) & ~(alignment-1)
        if aligned>capacity:raise gdb.GdbError('Bitstream range exceeds allocation')
        tail=read(mapped+size,aligned-size)
        record=dict(index=len(records),pts_us=pts,size=size,equal=same,
                    sha256=hashlib.sha256(data).hexdigest(),
                    aligned_size=aligned,trailing_nonzero=sum(b!=0 for b in tail))
        records.append(record)
        print('VIDEO_BITSTREAM '+json.dumps(record),flush=True)
        return not candidate and len(records)>=96

class PaddingDone(gdb.Breakpoint):
    def stop(self):
        if int(gdb.parse_and_eval('$cr3')) & ~4095!=root:return False
        picture=int(gdb.parse_and_eval('$r13'))
        buffer=int(gdb.parse_and_eval('$r12'))
        size=field(picture,'picture_slice_bytes')
        aligned=int(gdb.parse_and_eval('$r14'))
        if not records or records[-1]['size']!=size or records[-1]['aligned_size']!=aligned:
            raise gdb.GdbError('Post-padding probe lost frame alignment')
        tail=read(field(buffer,'vk_buffer_mapped')+size,aligned-size)
        records[-1]['trailing_nonzero_after']=sum(b!=0 for b in tail)
        print('VIDEO_PADDING_VERIFIED '+json.dumps(records[-1]),flush=True)
        return len(records)>=96

class Packet(gdb.Breakpoint):
    def stop(self):
        global root,packet,probe
        current=int(gdb.parse_and_eval('$cr3')) & ~4095
        if root is not None and root!=current:return False
        root=current
        size=int(gdb.parse_and_eval('$rdx')) & 0xffffffff
        if not 0<size<=4*1024*1024:return False
        data=read(int(gdb.parse_and_eval('$rsi')),size)
        packet=(int(gdb.parse_and_eval('$rcx')),vcl(data))
        if probe is None:
            native=int(gdb.parse_and_eval('$rbx'))
            base=number(native+0x190)-0x1b940
            if not 0x10000<=base<0x800000000000 or base%4096:
                raise gdb.GdbError('Invalid bridge base')
            probe=Slices('*%#x'%(base+0xfef10),type=gdb.BP_HARDWARE_BREAKPOINT)
            if candidate:
                PaddingDone('*%#x'%(base+0xff0ce),type=gdb.BP_HARDWARE_BREAKPOINT)
        return False
Packet('*0x45948a7b',type=gdb.BP_HARDWARE_BREAKPOINT)
end
continue
python
if len(records)!=96:raise gdb.GdbError('Incomplete bitstream trace')
print('VIDEO_BITSTREAM_RESULT '+json.dumps(dict(count=len(records),
    mismatches=sum(not r['equal'] for r in records),
    nonzero_tail_frames=sum(r['trailing_nonzero']>0 for r in records),
    nonzero_tail_after_frames=(sum(r.get('trailing_nonzero_after',-1)!=0
                                  for r in records) if candidate else None))),flush=True)
if candidate:
    if any(r.get('trailing_nonzero_after',-1)!=0 for r in records):
        raise gdb.GdbError('Submitted padding was not verified zero for every frame')
end
delete breakpoints
detach
quit
