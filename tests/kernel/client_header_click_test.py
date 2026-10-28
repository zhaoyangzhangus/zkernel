"""Run production client-header staging/replay across former drag strips."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/input_router.c').read_text()
clear = source[source.index('void window_clear_title_capture_locked('):
               source.index('void window_input_reset_desktop_state_locked(')]
moved = source[source.index('bool window_title_capture_moved_locked('):
               source.index('uint32_t window_input_cursor_shape_locked(')]
start = source.index('            } else if (((target->flags & OS_WINDOW_EXPLICIT_MOVE_RESIZE)')
client_start = source.index('            } else if (\n                window_client_decorations(target->flags)', start)
end = source.index('            } else if (', client_start + 20)
press = source[start:end].lstrip().removeprefix('} else ') + '}\n'
start = source.index('            if (g_window_server.title_pressed_client) {')
end = source.index('            } else if (g_window_server.dragging_identifier', start)
release = source[start:end] + '}\n'
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define WINDOW_CLIENT_DRAG_REGION_HEIGHT 40
#define WINDOW_TITLE_DRAG_DISTANCE 6
#define WINDOW_TITLE_BUTTON_NONE 0
#define INPUT_VALUE_PRESS 1
#define OS_WINDOW_EXPLICIT_MOVE_RESIZE 64
typedef struct { int value, code; } input_event_t;
typedef struct { uint32_t flags, identifier; int32_t x, y; bool client_button_capture; } window_server_window_t;
static struct {
 uint32_t pointer_x, pointer_y, title_pressed_identifier, title_pressed_button;
 uint32_t title_pressed_pointer_x, title_pressed_pointer_y;
 bool title_pressed_client; input_event_t title_pressed_event;
} g_window_server;
static window_server_window_t window = {.flags=4,.identifier=3,.x=10,.y=10};
static bool alive = true;
static input_event_t delivered[2];
static int count;
static bool window_client_decorations(uint32_t flags) { return (flags & 4) != 0; }
static bool window_standard_caption(uint32_t flags) { return (flags & 20) == 20; }
static uint32_t os_window_caption_height(uint32_t flags) {
 uint32_t height=(flags>>16)&255; return height>=40 && height<=128 ? height : 40;
}
static window_server_window_t *window_scene_find_locked(uint32_t id) {
 return alive && id == window.identifier ? &window : 0;
}
static void window_enqueue_event_locked(window_server_window_t *w, const input_event_t *e) {
 assert(w == &window && count < 2); delivered[count++] = *e;
}
''' + clear + moved + r'''
static bool stage(window_server_window_t *target, const input_event_t *event) {
 bool deliver_input = true;
''' + press + r'''
 return deliver_input;
}
static bool replay(window_server_window_t *target, const input_event_t *event) {
 bool deliver_input = true;
''' + release + r'''
 return deliver_input;
}
int main(void) {
 input_event_t down = {1, 1}, up = {0, 1};
 const uint32_t xs[] = {45, 100, 150, 300, 509, 700, 800, 930};
 for (unsigned i = 0; i < sizeof(xs)/sizeof(xs[0]); ++i) {
  count = 0; g_window_server.pointer_x = xs[i]; g_window_server.pointer_y = 30;
  assert(!stage(&window, &down)); assert(g_window_server.title_pressed_client);
  assert(count == 0); assert(!replay(&window, &up));
  assert(count == 2 && delivered[0].value == 1 && delivered[1].value == 0);
  assert(!g_window_server.title_pressed_client);
 }
 count = 0; g_window_server.pointer_x = 509;
 assert(!stage(&window, &down)); g_window_server.pointer_x += 6;
 assert(window_title_capture_moved_locked()); assert(!replay(&window, &up));
 assert(count == 0 && !g_window_server.title_pressed_client);
 g_window_server.pointer_x = 509; assert(!stage(&window, &down)); alive = false;
 assert(!replay(0, &up)); assert(count == 0); alive = true;
 g_window_server.pointer_y = 80;
 assert(stage(&window, &down)); assert(!g_window_server.title_pressed_client);
 window.flags |= OS_WINDOW_EXPLICIT_MOVE_RESIZE;
 for(unsigned i=0;i<sizeof(xs)/sizeof(xs[0]);++i){
  window.client_button_capture=false;
  g_window_server.pointer_x=xs[i];g_window_server.pointer_y=30;
  assert(stage(&window,&down)); // The client receives the press immediately.
  assert(window.client_button_capture&&!g_window_server.title_pressed_client);
  assert(replay(&window,&up)); // No synthetic replay or guessed title drag.
 }
 // Keeping the opt-in across a frame-mode switch must not steal a native
 // caption press into implicit client capture.
 window.flags=2|16|OS_WINDOW_EXPLICIT_MOVE_RESIZE;
 window.client_button_capture=false;
 assert(stage(&window,&down)&&!window.client_button_capture);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-header-click-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Client header: tab/button clicks, ordered replay, drag cancellation and destroyed target passed')
