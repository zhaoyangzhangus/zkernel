"""Compile and run the actual frame trace helper in both configurations."""
from pathlib import Path
import subprocess
import tempfile
source=(Path(__file__).resolve().parents[2]/
        'kernel/drivers/display/nvidia/nv_nvkms_backend.c').read_text()
start=source.index('static inline void nvkms_frame_trace(')
end=source.index('\nstatic void nvkms_gpu_pick_callback',start)
helper=source[start:end]
hot=source[source.index('static kstatus_t nvkms_copy_request_to_surface('):
           source.index('static bool nvkms_select_display(')]
assert 'realtest_flush(' not in hot
for marker in ('COPY_BEGIN','COPY_MAP_OK','COPY_DONE','ATOMIC_LOCK_OK',
               'ATOMIC_ACCEPTED','APPLY_BEGIN','APPLY_OK'):
    assert f'nvkms_frame_trace("OS_NVKMS_NATIVE_{marker}' in hot
assert '#define OS_NVKMS_FRAME_TRACE 0' in source
with tempfile.TemporaryDirectory(prefix='nvkms-frame-trace-') as temporary:
    for enabled in (0,1):
        code=(f'#define OS_NVKMS_FRAME_TRACE {enabled}\n' +
              '#include <assert.h>\nstatic unsigned calls;\n' +
              'static inline void nvkms_trace(const char *s){assert(s);++calls;}\n' +
              helper + f'\nint main(void){{nvkms_frame_trace("frame");assert(calls=={enabled});}}')
        binary=str(Path(temporary)/f'test-{enabled}')
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2',
                        '-x','c','-','-o',binary],input=code,text=True,check=True)
        subprocess.run([binary],check=True)
print('NVKMS per-frame trace disabled by default; opt-in works; no hot-path FAT flush PASS')
