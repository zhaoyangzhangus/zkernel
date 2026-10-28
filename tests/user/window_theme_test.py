"""Shared Chrome-style controls retain clipping, glyphs and close hit areas."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
for name in ('gshell', 'notepad', 'imageview', 'taskmgr'):
    assert 'user_client_chrome_app_icon(' not in (root / 'user/desktop' / name / 'main.c').read_text()
assert 'user_client_chrome_app_icon(' not in (root / 'user/desktop/settings/main.c').read_text()
assert 'fm4_search_icon(12U, FM4_HEADER_ICON_Y' not in (root / 'user/desktop/fileman/main.c').read_text()
program = r'''
#include <assert.h>
#include <stdint.h>
#include <uapi/window.h>
#include "user/client_chrome.h"
#include "user/runtime/text.h"
enum { W = 320, H = 180 };
static uint32_t storage[W * H + 2];
static uint32_t font_height = 24U;
uint32_t ui_text_line_height(void) { return font_height; }
int main(void) {
    uint32_t *pixels = storage + 1;
    storage[0] = storage[W * H + 1] = 0xdeadbeef;
    assert(USER_CLIENT_CHROME_BACKGROUND == 0x00d3e3fd);
    assert(OS_TEXT_DEFAULT_SIZE == 16);
    assert(USER_CLIENT_CHROME_CLOSE_BG == USER_CLIENT_CHROME_BACKGROUND);
    for (unsigned i = 0; i < W * H; ++i) pixels[i] = OS_THEME_SURFACE;
    user_client_chrome_fill_rect(pixels, W, W, H, 0, 0, W,
                                USER_CLIENT_CHROME_HEIGHT,
                                USER_CLIENT_CHROME_BACKGROUND);
    user_client_chrome_frame(pixels, W, W, H, USER_CLIENT_CHROME_HEIGHT);
    assert(pixels[20 * W + 100] == OS_THEME_HEADER);
    assert(pixels[100 * W + 100] == OS_THEME_SURFACE);
    assert(pixels[39 * W + 100] == OS_THEME_BORDER);
    user_client_chrome_close(pixels, W, W, H, USER_CLIENT_CHROME_HEIGHT,
                             USER_CLIENT_CHROME_CLOSE_BG, USER_CLIENT_CHROME_CLOSE_FG);
    unsigned ink = 0;
    for (unsigned y = 12; y < 28; ++y)
        for (unsigned x = W - 26; x < W - 10; ++x)
            ink += pixels[y * W + x] != OS_THEME_HEADER;
    assert(ink > 20);
    assert(user_client_chrome_close_hit(W - 1, 20, W, 40));
    assert(!user_client_chrome_close_hit(W - 37, 20, W, 40));
    assert(!user_client_chrome_close_hit(W - 1, 40, W, 40));
    user_client_chrome_round_rect(pixels, W, W, H, W - 3, H - 3,
                                  24, 24, OS_THEME_SELECTED);
    user_client_chrome_close(pixels, W, 10, 10, 40, OS_THEME_HEADER, OS_THEME_TEXT);
    assert(storage[0] == 0xdeadbeef && storage[W * H + 1] == 0xdeadbeef);
    font_height = 60U;
    assert(USER_CLIENT_CHROME_HEIGHT == 76U);
    assert(USER_CLIENT_CHROME_TITLE_Y == 8U);
    assert(os_window_caption_height(OS_WINDOW_CAPTION_HEIGHT(76U)) == 76U);
}
'''
with tempfile.TemporaryDirectory(prefix='system-window-theme-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root), '-I' + str(root / 'include'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Window theme: Chrome palette, title/content separation, close glyph/hits and edge clipping passed')
