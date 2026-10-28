#include <kernel/display.h>
#include "internal.h"

/* GOP 扫描线统一交给显示核心发布，光标包围盒由调用方跳过。 */
void compositor_copy_wc_scanline(
    volatile uint32_t *__restrict destination,
    const uint32_t *__restrict source,
    uint32_t pixels) {

    display_core_publish_xrgb8888_span(
        destination,
        source,
        pixels);
}
