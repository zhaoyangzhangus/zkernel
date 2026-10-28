#include <ascii_font.h>
#include <kernel/caption_text.h>
#include <kernel/kmem.h>
#include <kernel/string.h>
#include <uapi/control_tree.h>
#include <uapi/utf8.h>

caption_text_t *caption_text_create(const char *text, uint32_t height) {
    if (!text || !height || height>96U) return 0;
    uint32_t length=0,width=0;
    while (length<32U && text[length]) ++length;
    if (length==32U) return 0;
    for (size_t i=0;i<length;) {
        size_t used;uint32_t code=os_utf8_next(text+i,length-i,&used);
        if (!used) return 0;
        width+=os_control_text_cell_width(code,height);i+=used;
    }
    caption_text_t *run=kmalloc(sizeof(*run)+(size_t)width*height,0);
    if (!run) return 0;
    atomic_init(&run->references,1U);run->width=width;run->height=height;
    uint8_t glyph[96U*96U];
    uint32_t x=0;
    for (size_t i=0;i<length;) {
        size_t used;uint32_t code=os_utf8_next(text+i,length-i,&used);i+=used;
        uint32_t w=os_control_text_cell_width(code,height);
        if (!ui_font_copy_glyph(code,w,height,glyph,sizeof(glyph)) &&
            !ui_font_copy_glyph('?',w,height,glyph,sizeof(glyph))) {
            kfree(run);return 0;
        }
        for (uint32_t y=0;y<height;++y)
            memcpy(run->pixels+(size_t)y*width+x,glyph+(size_t)y*w,w);
        x+=w;
    }
    return run;
}

void caption_text_get(caption_text_t *text) {
    if (text) atomic_fetch_add_explicit(&text->references,1U,memory_order_relaxed);
}
void caption_text_put(caption_text_t *text) {
    if (text && atomic_fetch_sub_explicit(&text->references,1U,memory_order_acq_rel)==1U)
        kfree(text);
}
