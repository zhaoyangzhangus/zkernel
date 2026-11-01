#ifndef ZKERNEL_FRAME_POOL_SIM_HOST_H
#define ZKERNEL_FRAME_POOL_SIM_HOST_H
#include "frame_pool.h"
#include "persist.h"

typedef struct fm_sim {
    int ram_fd;
    uint64_t base_physical;
    size_t chunks;
    uint64_t *root_physical;
    uint64_t *root_metadata;
    uint64_t *metadata_4k_va;
    size_t metadata_reserved;
    fm_persist persist;
    int fail_next_map;
    int fail_next_persist;
} fm_sim;

int fm_sim_open(fm_sim *s, size_t chunks, const char *persist_path);
void fm_sim_close(fm_sim *s);
fm_config fm_sim_config(fm_sim *s);
void *fm_sim_map_alias(fm_sim *s, uint64_t pa);
void fm_sim_unmap_alias(void *alias);
#endif
