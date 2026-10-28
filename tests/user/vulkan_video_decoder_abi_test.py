"""Actual isolated library ABI and validation; not a hardware playback test."""
from pathlib import Path
import ctypes
import subprocess

root = Path(__file__).resolve().parents[2]
binary = root / 'build/nvidia/vulkan/libos-vulkan-video.so.1'
exports = subprocess.check_output(['nm', '-D', '--defined-only', str(binary)], text=True)
names = {line.split()[-1].split('@')[0] for line in exports.splitlines()}
expected = {'OS_VIDEO_1', 'os_video_decoder_abi', 'os_video_decoder_create_h264',
            'os_video_decoder_create',
            'os_video_decoder_query',
            'os_video_decoder_send', 'os_video_decoder_receive',
            'os_video_decoder_reset', 'os_video_decoder_destroy'}
assert names == expected, names
library = ctypes.CDLL(str(binary), mode=ctypes.RTLD_LOCAL)
library.os_video_decoder_abi.restype = ctypes.c_uint32
assert library.os_video_decoder_abi() == 1
query = library.os_video_decoder_query
query.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
assert query(2, None) == -22
caps = (ctypes.c_uint32 * 4)(1, 2, 3, 4)
assert query(99, ctypes.byref(caps)) == -22 and list(caps) == [0, 0, 0, 0]
create = library.os_video_decoder_create_h264
create.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)]
create.restype = ctypes.c_int
assert create(None, 0, None) == -22
for length in (1, 16 * 1024 * 1024 + 1, 0xffffffff):
    output = ctypes.c_void_p(1234)
    assert create(None, length, ctypes.byref(output)) == -22
    assert output.value is None
send = library.os_video_decoder_send
send.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int64]
assert send(None, None, 0, 0) == -22
receive = library.os_video_decoder_receive
receive.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
assert receive(None, None) == -22
for operation in ('reset', 'destroy'):
    method = getattr(library, 'os_video_decoder_' + operation)
    method.argtypes = [ctypes.c_void_p]
    method(None)
print('Vulkan decoder ABI: exact exports, no FFmpeg symbol leakage, validation and null cleanup passed')
