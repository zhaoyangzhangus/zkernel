"""Serial source regressions against one explicitly configured Chromium checkout."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys

from chromium_source import chromium_source

TESTS = (
    'chrome_font_character_test.py', 'chrome_font_route_test.py',
    'chrome_frame_trace_test.py', 'chrome_ipc_startup_timeout_test.py',
    'kernel/chrome_capture_deferred_show_test.py', 'kernel/chrome_capture_route_test.py',
    'kernel/chrome_capture_state_test.py', 'kernel/chrome_cjk_fallback_test.py',
    'kernel/chrome_menu_hover_restore_test.py', 'kernel/chrome_mouse_release_flags_test.py',
    'kernel/chrome_pointer_shared_state_test.py', 'user/chrome_async_main_frame_test.py',
    'user/chrome_cursor_screen_point_test.py', 'user/chrome_frame_pacing_test.py',
    'user/chrome_frame_trace_identity_test.py', 'user/chrome_server_frame_test.py', 'user/chrome_resize_state_test.py', 'user/chrome_minimize_abi_test.py',
    'user/chrome_interactive_window_test.py',
    'user/chrome_native_trace_abi_test.py', 'user/chrome_new_tab_trace_test.py',
    'user/chrome_nonactivatable_show_test.py', 'user/chrome_window_activation_test.py',
    'user/chrome_window_bounds_test.py', 'user/chrome_window_watch_test.py',
    'user/chromium_quarantine_timing_test.py', 'user/chromium_renderer_init_timing_test.py',
    'user/chromium_window_destroy_visual_test.py', 'user/chromium_window_transparency_test.py',
    'user/chrome_widget_trace_source_test.py',
    'user/chrome_vulkan_backend_test.py',
    'user/chrome_vulkan_canvas_test.py',
    'user/chrome_surface_lifetime_test.py',
    'user/chrome_vulkan_compile_test.py',
    'user/chrome_glibc_toolchain_test.py',
    'user/chrome_vulkan_inprocess_test.py',
    'user/chrome_vulkan_only_context_test.py',
    'user/chrome_vulkan_context_loss_test.py',
    'user/chrome_guest_library_policy_test.py',
    'user/chromium_toolchain_root_test.py',
    'user/chrome_external_protocol_test.py',
)

# These validate symbol addresses/build identity against the deployed ESP,
# not just source behavior. Keep them mandatory in the default full run.
DEPLOYED_TESTS = (
    'user/chrome_frame_trace_identity_test.py',
    'user/chrome_new_tab_trace_test.py',
)


def selected_tests(phase):
    if phase == 'all':
        return TESTS
    if phase == 'deployed':
        return DEPLOYED_TESTS
    if phase == 'source':
        return tuple(name for name in TESTS if name not in DEPLOYED_TESTS)
    raise ValueError('Unknown Chromium regression phase: ' + phase)


def run_test(name, source, timeout=90):
    process = subprocess.Popen([sys.executable, str(Path(__file__).parent/name)],
                               env=dict(os.environ, CHROMIUM_SRC=str(source)),
                               start_new_session=True)
    try:
        return process.wait(timeout=timeout) == 0
    except subprocess.TimeoutExpired:
        print(f'CHROMIUM_SOURCE_TIMEOUT {name}', flush=True)
        # Terminate this test's own process group, including compiler children.
        for sig, grace in ((signal.SIGTERM, 5), (signal.SIGKILL, 5)):
            try:
                os.killpg(process.pid, sig)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=grace)
                break
            except subprocess.TimeoutExpired:
                if sig == signal.SIGKILL:
                    raise RuntimeError(f'Test still live after termination: {name}')
                continue
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--list', action='store_true', help='list tests without running them')
    parser.add_argument('--phase', choices=('all', 'source', 'deployed'), default='all',
                        help='all (default), pre-deployment source checks, or deployed identity checks')
    args = parser.parse_args()
    tests = selected_tests(args.phase)
    if args.list:
        print('\n'.join(tests))
        return 0
    try:
        source = chromium_source()
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 2
    print(f'Chromium source: {source}', flush=True)
    print(f'Chromium regression phase: {args.phase}', flush=True)
    failures = []
    for name in tests:
        print(f'CHROMIUM_SOURCE_TEST {name}', flush=True)
        if not run_test(name, source):
            failures.append(name)
    print(f'Chromium regressions ({args.phase}): {len(tests)-len(failures)}/{len(tests)} passed', flush=True)
    for name in failures:
        print(f'CHROMIUM_SOURCE_FAILED {name}', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
