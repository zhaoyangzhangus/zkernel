"""QEMU 与真机统一使用固件 framebuffer，禁止专用翻页路径回流。"""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
for path in (root / 'kernel').rglob('*'):
    if path.suffix not in ('.c', '.h'):
        continue
    source = path.read_text()
    for forbidden in ('qemu_stdvga_', 'compositor_qemu_', 'QEMU_STDVGA_PAGE_FLIP',
                      'compositor_commit_qemu_stdvga', 'g_compositor_scanout_flipped'):
        assert forbidden not in source, (path, forbidden)
assert not (root / 'kernel/drivers/display/qemu_stdvga.c').exists()
assert not (root / 'include/kernel/qemu_stdvga.h').exists()
display = (root / 'kernel/graphics/display.c').read_text()
assert 'display_core_framebuffer_virtual()' in display
print('QEMU GOP-only: 专用翻页驱动、隐藏页修复和光标分支均已删除 PASS')
