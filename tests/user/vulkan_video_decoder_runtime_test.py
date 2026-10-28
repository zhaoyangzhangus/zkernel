"""Real ELF deployment closure; invalid fixture trees never touch the ESP."""
from pathlib import Path
import importlib.util
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('video_decoder_gate',
    root / 'tools/nvidia/video-decoder-runtime-gate.py')
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
actual = root / 'build/esp/usr/apps/chrome/lib'
library = root / 'build/nvidia/vulkan/libos-vulkan-video.so.1'
with tempfile.TemporaryDirectory() as directory:
    runtime = Path(directory)
    for file in actual.iterdir():
        if file.is_file(): (runtime / file.name).symlink_to(file)
    selected = gate.check(runtime, library)
    assert 'libos-vulkan-video.so.1' in selected and 'libvulkan.so.1' in selected
    for missing in ('libvulkan.so.1', 'libc.so.6', 'libm.so.6'):
        (runtime / missing).unlink()
        try:
            gate.check(runtime, library)
        except (OSError, ValueError, RuntimeError):
            pass
        else:
            raise AssertionError('Missing dependency accepted: ' + missing)
        (runtime / missing).symlink_to(actual / missing)
    try:
        gate.check(runtime, actual / 'libm.so.6')
    except ValueError:
        pass
    else:
        raise AssertionError('Wrong dlopen root accepted')
    gate.check(runtime, library)
print('Actual decoder ELF gate: closure, missing loader/libc/libm, wrong exports and restored tree passed')
