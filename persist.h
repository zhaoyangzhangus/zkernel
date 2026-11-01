#ifndef ZKERNEL_FRAME_POOL_PERSIST_H
#define ZKERNEL_FRAME_POOL_PERSIST_H
#include <stdint.h>

typedef struct fm_persist {
    int fd;
    uint64_t sequence;
    uint64_t high_water_pages;
} fm_persist;

/* Double-slot non-volatile high-water file. Not a mapping of physical memory.
 * Max survives a restart; physical addresses deliberately DO NOT. */
int fm_persist_open(fm_persist *p, const char *path);
int fm_persist_store(fm_persist *p, uint64_t new_high_water);
void fm_persist_close(fm_persist *p);
#endif
