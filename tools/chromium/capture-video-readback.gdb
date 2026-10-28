# Capture borrowed decoder output, before Chrome's NV12 copy or composition.
# Offset is specific to the verified post-SDK Chrome executable below.
set pagination off
set confirm off
python
import gdb
import os
import re
import struct
import sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/chromium').resolve()))
from trace_build import validate_chrome_build,_chrome_build_id
from guest_memory import read_user
validate_chrome_build('Build ID: bade6abd6c51df18')
destination = Path(os.environ['OS_VIDEO_CAPTURE_DIR']).resolve()
if not destination.is_dir():
    raise gdb.GdbError('Capture directory must already exist')
capture_count=int(os.environ.get('OS_VIDEO_CAPTURE_COUNT','8'))
capture_prefix=os.environ.get('OS_VIDEO_CAPTURE_PREFIX','decoded-y')
packet_count=int(os.environ.get('OS_VIDEO_PACKET_COUNT','0'))
capture_mapped=os.environ.get('OS_VIDEO_CAPTURE_MAPPED')=='1'
if capture_mapped:
    if not packet_count:
        raise gdb.GdbError('Mapped capture requires the packet locator')
    for name in ('build/nvidia/vulkan/libos-vulkan-video.so.1',
                 'build/esp/usr/apps/chrome/lib/libos-vulkan-video.so.1'):
        if _chrome_build_id(Path(name))!='Build ID: 92ef122fa8ec44beedd1866b47c1bd2180669d26':
            raise gdb.GdbError('Stale mapped-buffer probe offsets')
if (not 1<=capture_count<=128 or not 0<=packet_count<=256 or
        not re.fullmatch(r'[a-z0-9-]{1,64}',capture_prefix)):
    raise gdb.GdbError('Invalid bounded capture count or filename prefix')
gdb.execute('target remote /tmp/system-video-readback.sock')

packet_capture=None
mapped_probe=None
mapped_pixels=None
mapped_matches=0

class MappedPlane(gdb.Breakpoint):
    def __init__(self,base,root):
        super().__init__('*%#x'%(base+0x140e40),type=gdb.BP_HARDWARE_BREAKPOINT)
        self.base,self.root=base,root

    def stop(self):
        global mapped_pixels
        if capture.count>=capture_count or int(gdb.parse_and_eval('$cr3')) & ~4095!=self.root:
            return False
        memory=gdb.selected_inferior()
        caller=int.from_bytes(read_user(memory,self.root,int(gdb.parse_and_eval('$rsp')),8),'little')
        if not self.base+0x13dd50<=caller<self.base+0x13f000:
            return False
        width=int(gdb.parse_and_eval('$r8')) & 0xffffffff
        height=int(gdb.parse_and_eval('$r9')) & 0xffffffff
        stride=int(gdb.parse_and_eval('$rcx')) & 0xffffffff
        if height!=720: return False
        if not 1280<=width<=stride<=65536:
            raise gdb.GdbError('Unexpected mapped Y-plane geometry')
        source=int(gdb.parse_and_eval('$rdx'))
        pixels=b''.join(read_user(memory,self.root,source+y*stride,1280) for y in range(96))
        mapped_pixels=pixels
        path=destination/('%s-mapped-%02d.pgm'%(capture_prefix,capture.count))
        with path.open('xb') as output:
            output.write(b'P5\n1280 96\n255\n'+pixels)
        return False

def completed():
    return capture.count>=capture_count and (
        packet_capture is None or packet_capture.count>=packet_count)

class EncodedPacket(gdb.Breakpoint):
    def __init__(self):
        # Decode+0x23b: immediately before send_(decoder,data,size,pts).
        super().__init__('*0x45948a7b',type=gdb.BP_HARDWARE_BREAKPOINT)
        self.count=0
        self.total=0
        self.path=destination/(capture_prefix+'.packets')
        self.path.open('xb').close()  # Never overwrite earlier evidence.

    def stop(self):
        global mapped_probe
        if self.count>=packet_count:
            return completed()
        address=int(gdb.parse_and_eval('$rsi'))
        size=int(gdb.parse_and_eval('$rdx')) & 0xffffffff
        pts=int(gdb.parse_and_eval('$rcx'))
        if not address or not size:
            return False
        if capture_mapped and mapped_probe is None:
            root=int(gdb.parse_and_eval('$cr3')) & ~4095
            native=int(gdb.parse_and_eval('$rbx'))
            send=int.from_bytes(read_user(gdb.selected_inferior(),root,native+0x190,8),'little')
            base=send-0x1b940
            if not 0x10000<=base<0x800000000000 or base%4096:
                raise gdb.GdbError('Invalid video bridge base')
            mapped_probe=MappedPlane(base,root)
        if size>4*1024*1024 or self.total+size>32*1024*1024:
            raise gdb.GdbError('Compressed packet capture exceeds bound')
        pixels=read_user(gdb.selected_inferior(),
            int(gdb.parse_and_eval('$cr3')),address,size)
        with self.path.open('ab') as output:
            output.write(struct.pack('<qI',pts,size)+pixels)
        print('ENCODED_PACKET index=%d pts_us=%d size=%d head=%s'%
              (self.count,pts,size,pixels[:16].hex()),flush=True)
        self.count+=1
        self.total+=size
        return completed()

class DecodedFrame(gdb.Breakpoint):
    def __init__(self):
        # Drain+0x5a: receive_ has returned, decoded is still at rbp-0x60.
        super().__init__('*0x4594840a', type=gdb.BP_HARDWARE_BREAKPOINT)
        self.count = 0

    def stop(self):
        global mapped_pixels,mapped_matches
        if self.count>=capture_count:
            return completed()
        if packet_capture is not None and not packet_capture.count:
            return False
        if int(gdb.parse_and_eval('$rax')) & 0xffffffff:
            return False
        memory = gdb.selected_inferior()
        root = int(gdb.parse_and_eval('$cr3'))
        address = int(gdb.parse_and_eval('$rbp')) - 0x60
        width,height,stride_y,stride_uv,y,uv,pts = struct.unpack(
            '<IIiiQQq', read_user(memory, root, address, 40))
        if not (0 < width <= 4096 and 0 < height <= 2160 and
                width <= stride_y <= 65536 and y and uv):
            raise gdb.GdbError('Invalid decoded frame layout; refuse memory dump')
        rows = min(height, 96)
        pixels = b''.join(read_user(memory, root, y + row*stride_y, width)
                          for row in range(rows))
        if capture_mapped:
            if width!=1280 or height!=720 or mapped_pixels is None:
                raise gdb.GdbError('Missing corresponding mapped Y-plane')
            same=pixels==mapped_pixels
            print('MAPPED_COPY_MATCH index=%d equal=%s'%(self.count,same),flush=True)
            if same: mapped_matches+=1
            mapped_pixels=None
        # Lossless grayscale (Y) is sufficient to inspect white timestamp
        # strokes against black; no conversion/compositor is involved.
        path = destination / ('%s-%02d.pgm' % (capture_prefix,self.count))
        path.write_bytes(('P5\n%d %d\n255\n' % (width,rows)).encode()+pixels)
        print('DECODED_FRAME index=%d pts_us=%d size=%dx%d stride=%d path=%s' %
              (self.count,pts,width,height,stride_y,path), flush=True)
        self.count += 1
        return completed()

capture=DecodedFrame()
if packet_count:
    packet_capture=EncodedPacket()
print('VIDEO_READBACK_CAPTURE_READY', flush=True)
end
continue
python
if not completed():
    raise gdb.GdbError('Incomplete video capture; do not treat debugger exit as success')
if capture_mapped:
    print('MAPPED_COPY_RESULT matches=%d total=%d'%(mapped_matches,capture.count),flush=True)
end
delete breakpoints
detach
quit
