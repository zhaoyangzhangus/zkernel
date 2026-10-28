"""Exercise production cache publication across an in-flight damaged frame."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/shell.c').read_text()

def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdatomic.h>
#define DESKTOP_APP_NONE 0
typedef struct { unsigned x0,y0,x1,y1; } Rect;
typedef struct {
 Rect bounds; unsigned hovered_app; volatile uint32_t *target;
 unsigned target_width,target_height,target_stride;
 bool wallpaper_only;
} desktop_render_context_t;
static struct {unsigned display_width,display_height,display_stride;}
 g_window_server = {64,48,64};
static struct {bool desktop_cache_ready;} g_compositor_snapshot;
static uint32_t *g_desktop_cache;
static unsigned g_desktop_cache_width,g_desktop_cache_height,g_desktop_cache_stride;
static atomic_bool g_desktop_cache_ready;
static bool assets=true,locked,dirty,render_ok=true;
static unsigned copies,clears,hovers;
static bool desktop_shell_assets_available(void) {return assets;}
static void *kzalloc(size_t bytes,unsigned flags) {(void)flags;return calloc(1,bytes);}
#define kfree free
static bool desktop_render_assets_locked(desktop_render_context_t *c) {
 assert(c->wallpaper_only);
 assert(!atomic_load(&g_desktop_cache_ready));
 for(unsigned i=0;i<c->target_stride*c->target_height;++i)c->target[i]=0x123456;
 return render_ok;
}
static void window_mark_dirty_locked(void) {
 assert(locked);assert(atomic_load(&g_desktop_cache_ready));dirty=true;
}
static void desktop_clear_region_locked(void) {++clears;}
static void desktop_copy_cached_region(void) {++copies;}
static void desktop_draw_hover_locked(void) {++hovers;}
''' + '\n'.join(function(s) for s in [
    'bool desktop_shell_build_cache(void)',
    'void desktop_shell_publish_cache_locked(void)',
    'bool desktop_shell_cache_ready(void)',
    'void desktop_draw_wallpaper_locked(void)',
]) + r'''
int main(void) {
 assert(!desktop_shell_cache_ready());
 render_ok=false;assert(!desktop_shell_build_cache());
 assert(!desktop_shell_cache_ready() && !dirty);
 render_ok=true;assert(desktop_shell_build_cache());
 assert(!desktop_shell_cache_ready() && !dirty);
 for(unsigned i=0;i<64*48;++i)assert(g_desktop_cache[i]==0x123456);
 /* Frame already captured before activation: every damaged region stays solid. */
 g_compositor_snapshot.desktop_cache_ready=desktop_shell_cache_ready();
 desktop_draw_wallpaper_locked();assert(clears==1 && !copies);
 locked=true;desktop_shell_publish_cache_locked();
 assert(dirty && desktop_shell_cache_ready());locked=false;
 for(unsigned i=0;i<32;++i)desktop_draw_wallpaper_locked();
 assert(clears==33 && !copies);
 /* Next frame sees both complete cache and its full-screen damage. */
 locked=true;assert(dirty);
 g_compositor_snapshot.desktop_cache_ready=desktop_shell_cache_ready();
 dirty=false;locked=false;
 for(unsigned i=0;i<32;++i)desktop_draw_wallpaper_locked();
 assert(copies==32 && hovers==32 && clears==33);
 assets=false;assert(!desktop_shell_cache_ready());assets=true;
 ++g_window_server.display_width;assert(!desktop_shell_cache_ready());
 free(g_desktop_cache);
}
'''
compositor = (root / 'kernel/graphics/compositor.c').read_text()
assert 'snapshot->desktop_cache_ready = desktop_shell_cache_ready();' in compositor
with tempfile.TemporaryDirectory(prefix='desktop-cache-publication-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Desktop cache publication: failed/private build hidden; full damage; 32 old/new frame regions consistent PASS')
