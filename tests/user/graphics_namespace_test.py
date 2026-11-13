"""Keep native graphics naming independent of the browser compatibility ABI."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
files = ('user/runtime/graphics.c', 'user/runtime/graphics.h',
         'user/runtime/text.c', 'user/runtime/text.h',
         'user/runtime/message_box.h')
for name in files:
    source = (ROOT / name).read_text()
    assert not re.search(r'\b(?:liteos|LITEOS)_(?:gfx|text|message|TEXT|MESSAGE)', source), name
for name in ('libliteos_gfx.c', 'liteos_gfx.h', 'liteos_text.c',
             'liteos_text.h', 'liteos_message_box.h'):
    assert not (ROOT / 'user/runtime' / name).exists(), name
header = (ROOT / 'user/runtime/graphics.h').read_text()
implementation = (ROOT / 'user/runtime/graphics.c').read_text()
for function in ('gfx_clear', 'gfx_fill_rect', 'gfx_gradient_rect', 'gfx_frame'):
    assert f'void {function}(' in header, function
    assert f'void {function}(' in implementation, function
make = (ROOT / 'makefile').read_text()
assert '--soname libgraphics.so.1' in make
assert '$(BUILD)/esp/lib/libgraphics.so.1: $(DYNAMIC_GFX_LIBRARY_ELF)' in make
assert 'libliteosgfx' not in make
assert '--soname libsystemc.so.1' in make
assert '$(BUILD)/esp/lib/libsystemc.so.1: $(DYNAMIC_LIBC_ELF)' in make
# 当前浏览器和原生应用只部署中性的运行时名称。
assert '$(BUILD)/esp/lib/libliteosc.so.1' not in make
assert '$(BUILD)/esp/lib/ld-linux-liteos.so.1' not in make
assert '$(BUILD)/esp/lib/ld-linux-system.so.1' in make
print('Native graphics namespace: interfaces, source paths, SONAME and neutral browser runtime passed')
