"""Verify the address probe does not mistake a tooltip for a moved toolbar."""
import ast
from pathlib import Path

root = Path(__file__).resolve().parents[2]
tree = ast.parse((root/'tools/chromium/qmp-address-latency.py').read_text())
functions = [n for n in tree.body if isinstance(n, ast.FunctionDef)
             and n.name in ('toolbar_extent','address_is_focused')]
namespace = {}
exec(compile(ast.Module(body=functions, type_ignores=[]), '<toolbar>', 'exec'), namespace)
extent = namespace['toolbar_extent']
focused = namespace['address_is_focused']
width = 1920
for left in (11, 287, 650):
    pixels = bytearray(width*100*3)
    for y in (84,86,88,90,92):
        start = (y*width+left)*3
        pixels[start:start+943*3] = b'\xff'*(943*3)
        # 工具栏字形只是小间隔，不是独立浏览器窗口。
        start = (y*width+left+23)*3
        pixels[start:start+9*3] = b'\0'*(9*3)
        if left > 200:
            start = (y*width+83)*3
            pixels[start:start+40*3] = b'\xff'*(40*3)
    assert extent(width, pixels) == (left,left+943)
    assert not focused(width,pixels,left)
    for y in (56,57,89,90):
        start=(y*width+left+140)*3
        pixels[start:start+200*3]=bytes((0,80,255))*200
    assert focused(width,pixels,left)
    for y in (56,57,89,90):
        start=(y*width+left+140)*3
        pixels[start:start+200*3]=b'\xff'*(200*3)
    assert not focused(width,pixels,left)
    for y in range(65,83):
        start=(y*width+left+175)*3
        pixels[start:start+165*3]=bytes((20,80,230))*165
    assert focused(width,pixels,left)
    # 输入框获得焦点时的细蓝色边框不能导致窗口检查失败。
    start = (90*width+left+130)*3
    pixels[start:start+600*3] = b'\0'*(600*3)
    assert extent(width, pixels) == (left,left+943)
assert extent(width, bytearray(width*100*3)) is None
print('Address probe: moved toolbar, disconnected tooltip, glyph gaps and focus outline passed')
