"""Exercise the actual image loader with interrupted I/O and failed decodes."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'user/desktop/imageview/main.c').read_text()
loader=source[source.index('static bool load_image_file('):source.index('static void clamp_pan(')]
assert 'imageview_cleanup(' not in loader
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <uapi/image.h>
#include <uapi/syscall.h>
enum { NORMAL, SHORT_READ, INTERRUPTED, EMPTY, HUGE, SEEK_END_FAIL, SEEK_START_FAIL,
       OPEN_FAIL, ENCODED_ALLOC_FAIL, PIXELS_ALLOC_FAIL, EARLY_EOF, READ_FAIL, BAD_READ,
       INFO_FAIL, INFO_FORMAT, INFO_WIDTH, INFO_HEIGHT, INFO_STRIDE, INFO_OVERFLOW,
       INFO_SIZE, INFO_LIMIT, DECODE_FAIL, DECODE_FORMAT, DECODE_WIDTH, DECODE_HEIGHT,
       DECODE_STRIDE, DECODE_SIZE, CASES };
static unsigned mode, allocation_calls, reads, closes, decodes, infos, active;
static void *allocations[16];
static unsigned char *previous, *g_image_pixels;
static uint32_t g_image_width, g_image_height, g_image_stride;
static char g_path[]="/test.png";
static void *tracked_malloc(size_t size) {
    ++allocation_calls;
    if ((mode==ENCODED_ALLOC_FAIL && allocation_calls==1) ||
        (mode==PIXELS_ALLOC_FAIL && allocation_calls==2)) return 0;
    void *p=malloc(size); assert(p);
    for (unsigned i=0;i<16;++i) if (!allocations[i]) {
        allocations[i]=p; ++active; return p;
    }
    assert(0); return 0;
}
static void tracked_free(void *p) {
    if (!p) return;
    for (unsigned i=0;i<16;++i) if (allocations[i]==p) {
        allocations[i]=0; --active; free(p); return;
    }
    assert(0);
}
static int fake_open(const char *p,int flags) {
    assert(p==g_path && flags==O_RDONLY); return mode==OPEN_FAIL?-1:41;
}
static off_t fake_seek(int fd,off_t offset,int whence) {
    assert(fd==41 && !offset);
    if (whence==SEEK_END) {
        if (mode==EMPTY) return 0;
        if (mode==HUGE) return OS_IMAGE_MAX_ENCODED_BYTES+1;
        if (mode==SEEK_END_FAIL) return -1;
        return 8;
    }
    assert(whence==SEEK_SET); return mode==SEEK_START_FAIL?-1:0;
}
static ssize_t fake_read(int fd,void *p,size_t length) {
    assert(fd==41 && length && length<=8); ++reads;
    if (mode==INTERRUPTED && reads<=3) {errno=EINTR;return -1;}
    if (mode==EARLY_EOF) return 0;
    if (mode==READ_FAIL) {errno=EIO;return -1;}
    if (mode==BAD_READ) return length+1;
    if (mode==SHORT_READ || mode==INTERRUPTED) length=length>2?2:length;
    memset(p,0x64,length); return length;
}
static int fake_close(int fd) {assert(fd==41);++closes;return 0;}
static int64_t __libc_native_syscall6(uint64_t n,uint64_t address,uint64_t b,
                                     uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    assert(!b && !c && !d && !e && !f);
    assert(g_image_pixels==previous);
    assert(g_image_width==(previous?2U:0U) && g_image_height==(previous?2U:0U) && g_image_stride==(previous?8U:0U));
    if (n==OS_SYS_IMAGE_INFO) {
        ++infos; os_image_info_t *p=(void *)(uintptr_t)address;
        assert(p->hdr.size==sizeof(*p) && p->hdr.version==OS_SYSCALL_ABI_VERSION);
        assert(p->encoded_size==8);
        for (unsigned i=0;i<8;++i) assert(((unsigned char *)(uintptr_t)p->encoded)[i]==0x64);
        p->width=2;p->height=3;p->stride=8;p->pixel_bytes=24;p->format=OS_IMAGE_PIXEL_RGBA8888;
        if (mode==INFO_FAIL) return -22;
        if (mode==INFO_FORMAT) p->format=0;
        if (mode==INFO_WIDTH) p->width=0;
        if (mode==INFO_HEIGHT) p->height=0;
        if (mode==INFO_STRIDE) p->stride=7;
        if (mode==INFO_OVERFLOW) p->width=UINT32_C(0x40000002);
        if (mode==INFO_SIZE) p->pixel_bytes=23;
        if (mode==INFO_LIMIT) {p->height=OS_IMAGE_MAX_PIXEL_BYTES/8+1;p->pixel_bytes=(uint64_t)p->height*8;}
        return 0;
    }
    assert(n==OS_SYS_IMAGE_DECODE);++decodes;
    os_image_decode_t *p=(void *)(uintptr_t)address;
    assert(p->hdr.size==sizeof(*p) && p->hdr.version==OS_SYSCALL_ABI_VERSION);
    assert(p->encoded_size==8 && p->capacity==24 && (void *)(uintptr_t)p->pixels!=previous);
    memset((void *)(uintptr_t)p->pixels,0xab,p->capacity);
    p->width=2;p->height=3;p->stride=8;p->bytes_written=24;p->format=OS_IMAGE_PIXEL_RGBA8888;
    if (mode==DECODE_FAIL) return -5;
    if (mode==DECODE_FORMAT) p->format=0;
    if (mode==DECODE_WIDTH) p->width=1;
    if (mode==DECODE_HEIGHT) p->height=4;
    if (mode==DECODE_STRIDE) p->stride=7;
    if (mode==DECODE_SIZE) p->bytes_written=23;
    return 0;
}
#define open fake_open
#define lseek fake_seek
#define read fake_read
#define close fake_close
#define malloc tracked_malloc
#define free tracked_free
''' + loader + r'''
int main(void) {
    const unsigned initial[]={NORMAL, DECODE_FAIL, ENCODED_ALLOC_FAIL};
    for (unsigned i=0;i<3;++i) {
        mode=initial[i];previous=g_image_pixels=0;
        g_image_width=g_image_height=g_image_stride=0;
        allocation_calls=reads=closes=decodes=infos=0;
        assert(load_image_file()==(mode==NORMAL));
        assert(active==(mode==NORMAL?1U:0U) && closes==1);
        if (mode==NORMAL) tracked_free(g_image_pixels);
        else assert(!g_image_pixels && !g_image_width && !g_image_height && !g_image_stride);
        g_image_pixels=0;assert(!active);
    }
    for (unsigned test=0;test<CASES;++test) {
        mode=NORMAL;previous=tracked_malloc(16);memset(previous,0x27,16);
        g_image_pixels=previous;g_image_width=2;g_image_height=2;g_image_stride=8;
        mode=test;allocation_calls=reads=closes=decodes=infos=0;
        bool success=load_image_file();
        assert(success==(mode<=INTERRUPTED));
        assert(active==1 && closes==(mode!=OPEN_FAIL));
        if (success) {
            assert(g_image_pixels!=previous && g_image_width==2 && g_image_height==3 && g_image_stride==8);
            for (unsigned i=0;i<24;++i) assert(g_image_pixels[i]==0xab);
            assert(decodes==1 && infos==1 && allocation_calls==2);
            assert(reads==(mode==NORMAL?1:mode==SHORT_READ?4:7));
        } else {
            assert(g_image_pixels==previous && g_image_width==2 && g_image_height==2 && g_image_stride==8);
            for (unsigned i=0;i<16;++i) assert(previous[i]==0x27);
            if (mode>=INFO_FAIL && mode<=INFO_LIMIT) assert(!decodes && allocation_calls==1);
        }
        tracked_free(g_image_pixels);g_image_pixels=0;assert(!active);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='imageview-load-') as directory:
    c=Path(directory)/'test.c'; c.write_text(program)
    exe=Path(directory)/'test'
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(ROOT/'include'),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Image loader: 30 cases, initial loads, EINTR/short reads, metadata bounds, failure state retention and leak-free replacement passed')
