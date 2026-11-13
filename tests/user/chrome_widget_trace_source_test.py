"""Verify actual configured Chrome widget producers use neutral trace names."""
from pathlib import Path
import re
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

root = chromium_source()
widget = (root/'ui/views/widget/desktop_aura/desktop_window_tree_host_platform.cc').read_text()
surface = (root/'ui/ozone/platform/nativeguest/nativeguest_surface_factory.cc').read_text()
service = (root/'components/keyed_service/core/dependency_manager.cc').read_text()
assert 'void PlatformKeyedServiceMark(const char* phase, const char* name)' in service
assert 'LiteOSKeyedServiceMark' not in service
assert '"OS_PROFILE_SERVICE_"' in service and 'LITEOS_PROFILE_SERVICE_' not in service
for name in ('cc/trees/layer_tree_host.cc',
             'ui/compositor/external_begin_frame_adapter.cc',
             'content/browser/renderer_host/render_process_host_impl.cc',
             'components/keyed_service/core/dependency_manager.cc'):
    source = (root/name).read_text()
    assert 'LITEOS_STARTUP_TRACE' not in source, name
    assert 'defined(OS_STARTUP_TRACE)' in source, name
assert 'LogLiteosWidgetStage' not in widget
assert 'void LogPlatformWidgetStage(const char* marker)' in widget
assert 'LITEOS_WIDGET_' not in widget and 'LITEOS_SURFACE_PRESENT' not in surface
stages = set(re.findall(r'"(OS_WIDGET_INIT_[A-Z_]+)"', widget))
assert len(stages) == 9 and 'OS_WIDGET_INIT_DONE' in stages
assert 'write(2, "OS_SURFACE_PRESENT\\n"' in surface
assert 'sizeof("OS_SURFACE_PRESENT\\n") - 1U' in surface
for name in ('chrome/app/chrome_main.cc', 'chrome/app/chrome_main_delegate.cc',
             'chrome/browser/browser_process_impl.cc',
             'chrome/browser/chrome_browser_main.cc', 'content/app/content_main.cc',
             'content/app/content_main_runner_impl.cc'):
    source = (root/name).read_text()
    assert 'liteos' not in source.replace('__NATIVE_OS__', '').lower(), name
    assert re.search(r'"OS_[A-Z_]+', source), name
webui = (root/'chrome/browser/ui/ui_features.cc').read_text()
assert 'bool IsNativeGuestOzonePlatform()' in webui
assert 'IsLiteOSOzonePlatform' not in webui
for name in ('cc/trees/proxy_main.cc', 'cc/trees/proxy_impl.cc',
             'cc/scheduler/scheduler.cc', 'cc/mojo_embedder/async_layer_tree_frame_sink.cc',
             'chrome/browser/profiles/profile_impl.cc', 'chrome/browser/ui/tabs/tab_model.cc',
             'chrome/browser/ui/navigator/browser_navigator.cc',
             'chrome/browser/ui/toolbar/app_menu_icon_controller.cc',
             'chrome/browser/ui/views/frame/browser_view.cc',
             'chrome/browser/ui/views/toolbar/toolbar_view.cc',
             'content/browser/browser_main_loop.cc',
             'content/browser/renderer_host/render_frame_host_impl.cc',
             'content/browser/renderer_host/render_widget_host_impl.cc',
             'content/browser/web_contents/web_contents_impl.cc',
             'components/viz/common/frame_sinks/begin_frame_source.cc',
             'components/viz/service/display_embedder/software_output_device_ozone.cc',
             'ui/views/widget/root_view.cc', 'ui/views/widget/widget.cc'):
    source = (root/name).read_text()
    assert not re.search(r'\bLITEOS_[A-Z0-9_]+', source), name
    assert re.search(r'\bOS_[A-Z0-9_]+', source), name
gl_config = (root/'ui/gl/BUILD.gn').read_text()
gl_fence = (root/'ui/gl/gl_fence.cc').read_text()
assert '"OS_CHROMIUM"' in gl_config and '!defined(OS_CHROMIUM)' in gl_fence
assert 'LITEOS_CHROMIUM' not in gl_config + gl_fence
backend = root/'ui/ozone/platform/nativeguest'
backend_build = (backend/'BUILD.gn').read_text()
assert 'source_set("nativeguest")' in backend_build
for filename in re.findall(r'"([^"\n]+\.(?:cc|h))"', backend_build):
    assert (backend/filename).is_file(), filename
for path in backend.glob('*'):
    if path.suffix in ('.cc', '.h'):
        source = path.read_text()
        assert 'liteos' not in source.lower(), path.name
        for included in re.findall(r'#include "(ui/ozone/platform/[^"\n]+)"', source):
            assert (root/included).is_file(), (path.name, included)
extra = (root/'build/config/ozone_extra.gni').read_text()
assert 'ozone_external_platforms = [ "nativeguest" ]' in extra
assert '"platform/nativeguest"' in extra
assert '"ozone-platform") == "nativeguest"' in webui
# 生成器使用 platform.capitalize()，而不是按驼峰形式拆分单词。
suffix = 'nativeguest'.capitalize()
for filename, typename in (('ozone_platform_nativeguest.cc', 'OzonePlatform'),
                           ('client_native_pixmap_factory_nativeguest.cc', 'ClientNativePixmapFactory')):
    assert 'Create'+typename+suffix+'()' in (backend/filename).read_text()
for name in ('third_party/blink/renderer/modules/canvas/canvas2d/canvas_rendering_context_2d.cc',
             'third_party/blink/renderer/modules/canvas/canvas2d/base_rendering_context_2d.cc',
             'third_party/blink/renderer/platform/graphics/canvas_2d_resource_provider.cc',
             'third_party/blink/renderer/platform/graphics/gpu/drawing_buffer.cc',
             'third_party/blink/renderer/core/html/canvas/html_canvas_element.cc'):
    source = (root/name).read_text()
    assert 'LITEOS_' not in source, name
    assert re.search(r'"OS_[A-Z_]+', source), name
crashpad = root/'third_party/crashpad/crashpad'
assert 'crashpad_is_native_os' in (crashpad/'build/crashpad_buildconfig.gni').read_text()
assert '"crashpad_client_native_os.cc"' in (crashpad/'client/BUILD.gn').read_text()
assert (crashpad/'client/crashpad_client_native_os.cc').is_file()
assert not (crashpad/'client/crashpad_client_liteos.cc').exists()
assert 'liteos_chromium' not in (root/'v8/BUILD.gn').read_text()
for name in ('chrome/browser/ui/startup/startup_browser_creator_impl.cc',
             'chrome/browser/ui/views/frame/browser_view.cc',
             'components/viz/service/display_embedder/output_surface_provider_impl.cc',
             'content/browser/compositor/viz_process_transport_factory.cc'):
    source = (root/name).read_text()
    assert '"liteos"' not in source and '"nativeguest"' in source, name
for build, filename in (('chrome/browser/BUILD.gn', 'chrome/browser/platform_util_native_os.cc'),
                        ('chrome/renderer/BUILD.gn', 'chrome/renderer/accessibility/phrase_segmentation/dependency_parser_model_native_os.cc'),
                        ('components/crash/core/app/BUILD.gn', 'components/crash/core/app/crashpad_native_os.cc'),
                        ('sandbox/policy/BUILD.gn', 'sandbox/policy/linux/landlock_util_native_os.cc')):
    assert (root/filename).is_file(), filename
    assert Path(filename).name in (root/build).read_text(), filename
print('Configured Chrome widget/surface/service/startup/WebUI producers: neutral helpers and markers passed')
