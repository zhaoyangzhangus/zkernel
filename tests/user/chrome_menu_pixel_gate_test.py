"""Distinguish a real white-on-white menu from an empty surface or tooltip."""
import ast
from pathlib import Path

root = Path(__file__).resolve().parents[2]
tree = ast.parse((root/'tools/chromium/qmp-app-menu-test.py').read_text())
function = next(node for node in tree.body
                if isinstance(node, ast.FunctionDef) and node.name == 'menu_is_painted')
namespace = {}
exec(compile(ast.Module(body=[function], type_ignores=[]), '<real-menu-gate>', 'exec'), namespace)
gate = namespace['menu_is_painted']
width, height = 1920, 1080
pixels = bytearray(b'\xff' * (width * height * 3))
assert not gate(width,pixels)  # A white page alone is not a menu.
def rect(left,top,right,bottom,color):
    for y in range(top,bottom):
        pixels[(y*width+left)*3:(y*width+right)*3] = bytes(color)*(right-left)
rect(533,78,969,842,(0,0,0))
assert not gate(width,pixels)  # Uninitialized black popup is not painted.
rect(557,90,945,806,(255,255,255))
# 完整菜单包含文本。检查不再依赖过时的纯黑 alpha 边框来区分菜单和下方白色页面。
for y in range(230, 591, 45):
    rect(620,y,750,y+5,(50,50,50))
assert gate(width,pixels)  # Complete panel over an equally white page.
rect(533,78,969,842,(255,255,255))
rect(660,150,800,185,(0,0,0))
assert not gate(width,pixels)  # Small tooltip cannot satisfy the geometry.
print('Chrome menu gate: white-on-white panel accepted, empty surface and tooltip rejected')

# 通过模拟淡入执行正式观测循环：大部分像素会先出现，但完整的不透明阴影检查仍必须等待。
from types import SimpleNamespace
wait = next(node for node in ast.walk(tree)
            if isinstance(node,ast.FunctionDef) and node.name=='wait_for')
clock=[0.0]
frames=iter([(bytes(3000),False),(bytes([255])*3000,False),
             (bytes([255])*3000,True)])
env={'time':SimpleNamespace(monotonic=lambda:clock[0],
         sleep=lambda d:clock.__setitem__(0,clock[0]+d)),
     'frame':lambda:next(frames),'args':SimpleNamespace(capture_stall_after_ms=0),
     'diagnostic_taken':False,'diagnostic_pause_ms':0.0}
exec(compile(ast.Module(body=[wait],type_ignores=[]),'menu-observation','exec'),env)
complete,changed,first=env['wait_for'](bytes(3000),lambda c,p:p,0.0,'open')
assert abs(first-.02*1000)<1e-6 and abs(complete-.04*1000)<1e-6
assert changed==3000 and first<complete
print('Menu latency: first visual change is separate from opaque completion; full paint gate retained')
