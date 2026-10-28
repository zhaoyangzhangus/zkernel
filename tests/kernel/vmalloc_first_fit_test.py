"""Compare production virtual-run search with a page-by-page first-fit oracle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/vmalloc.c').read_text()
start = source.index('static bool find_virtual_run(')
search = source[start:source.index('\n}', start) + 2]
search=search.replace('uint64_t suffix = used >> bit;',
                      '++span_visits; uint64_t suffix = used >> bit;')
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define VMALLOC_PAGE_COUNT 262144U
#define VMALLOC_BITMAP_WORDS (VMALLOC_PAGE_COUNT / 64U)
static uint64_t g_vmalloc_bitmap[VMALLOC_BITMAP_WORDS];
static unsigned span_visits;
''' + search + r'''
static bool oracle(uint32_t count, uint32_t *out) {
    uint32_t start = 0, length = 0;
    if (!count || count > VMALLOC_PAGE_COUNT) return false;
    for (uint32_t page = 0; page < VMALLOC_PAGE_COUNT; ++page) {
        if (!(g_vmalloc_bitmap[page / 64] & (1ULL << (page % 64)))) {
            if (!length) start = page;
            if (++length == count) { *out = start; return true; }
        } else length = 0;
    }
    return false;
}
static void compare(uint32_t count) {
    uint32_t actual = UINT32_MAX, expected = UINT32_MAX;
    assert(find_virtual_run(count, &actual) == oracle(count, &expected));
    assert(actual == expected);
}
static void free_range(uint32_t start, uint32_t count) {
    for (uint32_t i = start; i < start + count; ++i)
        g_vmalloc_bitmap[i / 64] &= ~(1ULL << (i % 64));
}
static uint64_t rng = 9128371;
static uint64_t random_word(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng;
}
int main(void) {
    const uint32_t counts[] = {0, 1, 2, 3, 63, 64, 65, 127, 128, 129,
                               1024, VMALLOC_PAGE_COUNT, VMALLOC_PAGE_COUNT + 1};
    for (unsigned filled = 0; filled < 2; ++filled) {
        memset(g_vmalloc_bitmap, filled ? 255 : 0, sizeof(g_vmalloc_bitmap));
        for (unsigned i = 0; i < sizeof(counts)/sizeof(*counts); ++i) compare(counts[i]);
    }
    /* Every possible word-edge alignment, exact fits and too-short runs. */
    for (uint32_t offset = 0; offset < 64; ++offset) {
        memset(g_vmalloc_bitmap, 255, sizeof(g_vmalloc_bitmap));
        free_range(64 + offset, 193);
        free_range(VMALLOC_PAGE_COUNT - 67, 67);
        compare(67); compare(193); compare(194); compare(64); compare(1);
    }
    /* Sparse, dense and fragmented mixtures, including cross-word suffixes. */
    memset(g_vmalloc_bitmap,0,sizeof(g_vmalloc_bitmap));
    g_vmalloc_bitmap[0]=1;
    span_visits=0;compare(63);assert(span_visits==2);
    /* Exhaust every 16-bit fragment, in both a word prefix and suffix.
     * Empty following words ensure a result without full-arena oracle scans. */
    for(unsigned pattern=0;pattern<65536;++pattern) {
        g_vmalloc_bitmap[0]=(uint64_t)pattern;
        g_vmalloc_bitmap[1]=(uint64_t)pattern<<48;
        for(unsigned count=1;count<=17;++count)compare(count);
        g_vmalloc_bitmap[0]=((uint64_t)pattern<<48)|((1ULL<<48)-1U);
        g_vmalloc_bitmap[1]=pattern;
        for(unsigned count=1;count<=17;++count)compare(count);
    }
    memset(g_vmalloc_bitmap,0,sizeof(g_vmalloc_bitmap));
    for (unsigned trial = 0; trial < 500; ++trial) {
        for (unsigned i = 0; i < VMALLOC_BITMAP_WORDS; ++i) {
            uint64_t v = random_word();
            g_vmalloc_bitmap[i] = v % 3 == 0 ? 0 : (v % 3 == 1 ? UINT64_MAX : v);
        }
        compare(1 + random_word() % 1024);
    }
    puts("vmalloc first-fit: 2228224 exhaustive fragment/count cases, sparse word 2 span visits, capacity/boundaries, cross-word fits and 500 random maps passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vmalloc-fit-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=20)
