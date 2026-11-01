#define _POSIX_C_SOURCE 200809L
#include "persist.h"
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define MAGIC UINT64_C(0x4652414D45504F4C)
#define VERSION UINT64_C(1)

typedef struct record {
    uint64_t magic;
    uint64_t version;
    uint64_t sequence;
    uint64_t high_water_pages;
    uint64_t checksum;
} record;

static uint64_t digest(const record *r) {
    const uint64_t *x = (const uint64_t *)r;
    uint64_t h = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t val = x[i];
        for (unsigned j = 0; j < 8; ++j) {
            h ^= (val >> (j * 8)) & 255u;
            h *= UINT64_C(1099511628211);
        }
    }
    return h;
}

static int valid(const record *r) {
    return r->magic == MAGIC && r->version == VERSION &&
           r->checksum == digest(r);
}

int fm_persist_open(fm_persist *p, const char *path) {
    if (!p || !path) return -1;
    *p = (fm_persist){ .fd = -1 };
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
    record a = {0}, b = {0};
    int va = pread(fd, &a, sizeof(a), 0) == (ssize_t)sizeof(a) && valid(&a);
    int vb = pread(fd, &b, sizeof(b), sizeof(record)) == (ssize_t)sizeof(b) && valid(&b);
    if (va || vb) {
        record *r = va && (!vb || a.sequence >= b.sequence) ? &a : &b;
        p->sequence = r->sequence;
        p->high_water_pages = r->high_water_pages;
    }
    p->fd = fd;
    return 0;
}

int fm_persist_store(fm_persist *p, uint64_t high_water) {
    if (!p || p->fd < 0) return -1;
    if (high_water <= p->high_water_pages) return 0;
    record r = {
        .magic = MAGIC, .version = VERSION,
        .sequence = p->sequence + 1,
        .high_water_pages = high_water
    };
    r.checksum = digest(&r);
    off_t offset = (off_t)((r.sequence & 1u) * sizeof(record));
    if (pwrite(p->fd, &r, sizeof(r), offset) != (ssize_t)sizeof(r) ||
        fsync(p->fd)) return -1;
    p->sequence = r.sequence;
    p->high_water_pages = high_water;
    return 0;
}

void fm_persist_close(fm_persist *p) {
    if (p && p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
}
