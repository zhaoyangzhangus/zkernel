"""Run actual display preparation and input diagnostic boundaries."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
for path in (ROOT / 'kernel/graphics').glob('*.c'):
    assert 'LITEOS_' not in path.read_text(), path
display = (ROOT / 'kernel/graphics/display.c').read_text()
display = '\n'.join(line for line in display.splitlines() if not line.startswith('#include'))
source = (ROOT / 'kernel/graphics/input_router.c').read_text()
start = source.index('static void input_route_diag_key(')
opening = source.index('{',start)
depth, end = 1, opening+1
while depth:
    depth += (source[end] == '{') - (source[end] == '}'); end += 1
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define printf test_console_printf
''' + f'#include "{ROOT / "kernel/console_printf.c"}"\n' + r'''
#undef printf
''' + f'#include "{ROOT / "kernel/graphics/internal.h"}"\n' + r'''
#include <kernel/display.h>
window_server_state_t g_window_server;
static bool query_ok;
static uint32_t width, height, stride, format;
static uint64_t framebuffer;
static char output[1024];
static size_t used;
static unsigned records;
static void reset(void) { memset(output,0,sizeof(output)); used=records=0; }
void kernel_serial_write(const char *text) {
    assert(used+strlen(text)<sizeof(output)); strcpy(output+used,text); used+=strlen(text); ++records;
}
void kernel_serial_write_u32(uint32_t value) {
    char text[32]; snprintf(text,sizeof(text),"%u",value); kernel_serial_write(text);
}
void kernel_serial_write_serial_only(const char *text) { kernel_serial_write(text); }
bool display_core_query(uint32_t index, uint32_t *w, uint32_t *h, uint32_t *s, uint32_t *f) {
    assert(!index); if (!query_ok) return false;
    *w=width; *h=height; *s=stride; *f=format; return true;
}
uint64_t display_core_framebuffer_virtual(void) { return framebuffer; }
''' + display + source[start:end] + r'''
int main(void) {
    window_display_reset_locked();
    assert(!g_window_server.display_width && !g_window_server.framebuffer);
    assert(!window_display_prepare());
    assert(!strcmp(output,"OS_WINDOW_DISPLAY_PREPARE_FAIL QUERY\r\n"));
    query_ok=true; width=1920; height=1080; stride=2048; format=3;
    framebuffer=0x123456789abcdef0;
    reset(); assert(window_display_prepare());
    assert(g_window_server.display_width==1920 && g_window_server.display_stride==2048);
    assert((uintptr_t)g_window_server.framebuffer == framebuffer);
    assert(!strcmp(output,"OS_DISPLAY_GEOMETRY width=1920 height=1080 stride=2048 format=3 fb_lo=2596069104 fb_hi=305419896\r\n"));
    for (unsigned invalid=0; invalid<4; ++invalid) {
        width=1920; height=1080; stride=2048; framebuffer=0x10000;
        if (invalid==0) width=0;
        if (invalid==1) height=0;
        if (invalid==2) stride=1919;
        if (invalid==3) framebuffer=0;
        reset(); assert(!window_display_prepare());
        assert(strstr(output,"OS_WINDOW_DISPLAY_PREPARE_FAIL GEOMETRY WIDTH="));
        assert(strstr(output,"\r\n") && !strstr(output,"OS_DISPLAY_GEOMETRY"));
    }
    reset(); input_event_t event={.type=INPUT_EVENT_KEY,.value=INPUT_VALUE_PRESS,.code=0x2e};
    window_server_window_t target={.identifier=UINT32_MAX};
    g_window_server.focused_identifier=7;
    input_route_diag_key(NULL,&target,true); input_route_diag_key(&event,NULL,true);
    input_route_diag_key(&event,&target,false); assert(!records);
    event.code=0x2d; input_route_diag_key(&event,&target,true); assert(!records);
    event.code=0x2e; event.value=INPUT_VALUE_RELEASE;
    input_route_diag_key(&event,&target,true); assert(!records);
    event.value=INPUT_VALUE_PRESS; input_route_diag_key(&event,&target,true);
    assert(!strcmp(output,"OS_DIAG_INPUT_ROUTE code=46 value=1 focus=7 target=4294967295 delivered=1\r\n"));
    window_display_reset_locked(); assert(!g_window_server.framebuffer && !g_window_server.display_height);
    puts("GRAPHICS_DIAGNOSTIC_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='graphics-diagnostics-') as temporary:
    fixture=Path(temporary)/'test.c'; binary=Path(temporary)/'test'
    fixture.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fno-builtin',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I',str(ROOT/'include'),str(fixture),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
