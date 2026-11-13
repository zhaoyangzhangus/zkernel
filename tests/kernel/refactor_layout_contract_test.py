"""执行实际布局审计规则，区分进程链表、几何运算与旧结构残留。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import re

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/verify-refactor-layout.sh').read_text()
helpers = source[source.index('failures=0'):source.index('# 阶段 1：')]
thread = source[source.index('# 线程不再参与全局链表'):
                source.index("require_text include/kernel/sched.h 'void sched_initialize_new_thread'")]
rect = source[source.index('require_file include/kernel/rect.h'):
              source.index('for path in \\\n    kernel/graphics/window_geometry.c')]
removed = source[source.index('for path in user/font12x24.h'):
                 source.index('for path in compat/include/usb/device.h')]

desktop_start = source.rfind('require_text kernel/graphics/input_router.c', 0,
                             source.index("'void window_input_reset_desktop_state_locked'"))
desktop_end = source.rfind('require_text kernel/graphics/input_router.c', 0,
                           source.index("'void window_clear_title_capture_locked'", desktop_start))
desktop_owner = source[desktop_start:desktop_end]
plan_start = source.rfind('for path in kernel/graphics/*.c; do', 0,
                         source.index('        kernel/graphics/render_plan.c) ;;'))
plan_owner = source[plan_start:source.index('\ndone', plan_start) + len('\ndone')]

def ownership_loop(owner):
    start = source.rfind('for path in kernel/graphics/*.c; do', 0,
                         source.index('        kernel/graphics/' + owner + ') ;;'))
    return source[start:source.index('\ndone', start) + len('\ndone')]

drag_owner = ownership_loop('input_drag.c')
cursor_owner = ownership_loop('present_cursor.c')
retired_cursor = '\n'.join(line for line in source.splitlines()
    if any(line.startswith("forbidden_text '"+name+"'") for name in (
        'window_present_cursor_overlay', 'compositor_snapshot_overwrites_presented_cursor')))
assert len(retired_cursor.splitlines()) == 2
private_graphics = source[source.index('# 图形私有步骤：'):
    source.index("require_text kernel/graphics/raster.c 'REFACTOR_P7E_RASTER_OWNER'")]
private_entries = re.findall(r"'([a-z_]+\.c):([^']+)'", private_graphics)
assert sum(len(names.split()) for _, names in private_entries) == 13


with tempfile.TemporaryDirectory(prefix='layout-contract-') as directory:
    fixture = Path(directory)
    for name in ('include/kernel/sched.h', 'include/kernel/process.h',
                 'include/kernel/rect.h', 'kernel/process/process.c',
                 'kernel/graphics/shell.c', 'kernel/graphics/input_router.c',
                 'kernel/graphics/compositor.c', 'kernel/graphics/render_plan.c',
                 'kernel/graphics/desktop_interaction.c', 'kernel/graphics/input_drag.c',
                 'kernel/graphics/present_cursor.c', 'kernel/graphics/window.c',
                 'kernel/graphics/internal.h', 'kernel/graphics/cursor_occlusion.c',
                 'kernel/graphics/render.c', 'kernel/graphics/compose_cpu.c',
                 'kernel/graphics/launcher.c'):
        target = fixture / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / name, target)
    (fixture / 'kernel/sched').mkdir()
    (fixture / 'kernel/sched/fixture.c').write_text('/* 调度器占位。 */\n')
    (fixture / 'kernel/graphics/arithmetic.c').write_text('''
int difference = clip.x1-left;
int height = bottom-top;
int member = icon.leftover + icon.topology;
''')

    def run(rules=thread + rect + removed + desktop_owner + plan_owner + "\n" + drag_owner + "\n" + cursor_owner + "\n" + retired_cursor + "\n" + private_graphics):
        return subprocess.run(['bash', '-c', 'set -eu\n' + helpers + rules +
                               '\n[[ "$failures" == 0 ]]\n'],
                              cwd=fixture, text=True, capture_output=True, timeout=10)

    result = run()
    assert result.returncode == 0, result.stderr

    def reject(name, addition, marker):
        target = fixture / name
        existed = target.exists()
        previous = target.read_bytes() if existed else b''
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(previous + addition.encode())
        try:
            result = run()
            assert result.returncode != 0 and marker in result.stderr, result
        finally:
            if existed:
                target.write_bytes(previous)
            else:
                target.unlink()
        assert run().returncode == 0

    for owner, names in private_entries:
        for name in names.split():
            reject('kernel/graphics/internal.h', '\nvoid '+name+'(void);\n', name)
            target = fixture / 'kernel/graphics' / owner
            previous = target.read_text()
            broken, count = re.subn(r'(?m)^static (bool|void|kstatus_t) '+name+r'\(',
                                   r'\1 '+name+'(', previous)
            assert count == 1
            try:
                target.write_text(broken)
                result = run()
                assert result.returncode != 0 and name in result.stderr, result
            finally:
                target.write_text(previous)
    for name, owner in (
        ('window_present_cursor_overlay', 'present_cursor.c'),
        ('compositor_snapshot_overwrites_presented_cursor', 'cursor_occlusion.c')):
        for target in ('kernel/graphics/internal.h', 'kernel/graphics/'+owner):
            reject(target, '\nbool '+name+'(void);\n', name)
    reject('include/kernel/sched.h', '\nlist_head_t global_node;\n', 'global_node')
    reject('kernel/sched/fixture.c', '\nthread->global_node = next;\n', 'global_node')
    for flag in ('THREAD_FLAG_INITIAL_PLACEMENT', 'THREAD_FLAG_EXECUTION_REAP_QUEUED'):
        reject('kernel/process/process.c', '\nthread->flags |= ' + flag + ';\n', flag)
    for legacy in ('WindowRect', 'RegionRect', 'window_damage_rect_t'):
        reject('kernel/graphics/arithmetic.c', '\n' + legacy + ' bounds;\n', legacy)
    for field in ('left', 'top', 'right', 'bottom'):
        for accessor in ('.', '->'):
            reject('kernel/graphics/arithmetic.c', '\nvalue = bounds' + accessor + field + ';\n',
                   'forbidden legacy symbol')
    for name in ('user/font12x24.h', 'user/font12x24_data.h', 'tools/font12x24_gen.c',
                 'assets/desktop/old.raw', 'assets/desktop/old.rgba'):
        reject(name, '/* 已删除的后备路径不能恢复。 */\n', name)

    for field in ('desktop_pending_launch', 'desktop_gui_mask', 'desktop_tab_consumed',
                  'desktop_focus_cycle_requested'):
        reject('kernel/graphics/shell.c', '\ng_window_server.' + field + ' = 1;\n', field)
    for field in ('count', 'valid'):
        reject('kernel/graphics/compositor.c', '\ng_compositor_render_plan_' + field + ' = 1;\n',
               'g_compositor_render_plan_')

    for field in ('dragging_identifier', 'drag_offset_x', 'drag_offset_y', 'resize_edges',
                  'drag_blit_valid', 'drag_old_x', 'drag_old_y', 'drag_new_x', 'drag_new_y',
                  'drag_width', 'drag_height'):
        reject('kernel/graphics/window.c', '\ng_window_server.' + field + ' = 1;\n', field)
    for field in ('presented_pointer_x', 'presented_pointer_y', 'presented_pointer_valid',
                  'presented_cursor_shape'):
        reject('kernel/graphics/compositor.c', '\ng_window_server.' + field + ' = 1;\n',
               'presented_pointer_')

    # 反向修改审计规则，必须重新暴露原来的两类误报。
    old_thread = thread.replace("forbidden_text 'global_node' include/kernel/sched.h kernel/sched",
                                "forbidden_text 'global_node' include/kernel/sched.h include/kernel/process.h kernel/sched kernel/process")
    assert run(old_thread).returncode != 0
    old_rect = rect.replace('(^|[.]|->)', '(^|[.>-])')
    assert run(old_rect).returncode != 0

print('布局审计：实际进程注册表与图标缓存通过，71 组旧结构/已删入口/私有步骤重新公开与跨模块写入反例被拒绝，两类误报反向验证通过')
