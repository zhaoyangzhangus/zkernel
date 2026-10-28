/* Phase 12: reuse the verified EGL/OpenGL path and add CPU HBUFFER present. */
#define P11_APP_ROOT "/usr/apps/p12-window"
#define P11_WINDOW_BRIDGE 1
#define P11_FRAME_COUNT 2U
#define P11_SURFACE_WIDTH 320U
#define P11_SURFACE_HEIGHT 240U
#define P11_TEXTURE_WIDTH 384U
#define P11_TEXTURE_HEIGHT 288U
#define P11_RESIZED_WIDTH 384U
#define P11_RESIZED_HEIGHT 288U
#define P11_WINDOW_BUFFER_ADDRESS 0x07000000ULL

#define main phase12_window_probe_main
#include "phase11_triangle_test.c"
#undef main

int main(void) {
    return phase12_window_probe_main();
}
